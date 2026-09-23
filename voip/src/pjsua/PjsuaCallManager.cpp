#include "PjsuaCallManager.hpp"

#include <cstring>

namespace voip {

namespace {
Error TerminalError(const pjsua_call_info &info) noexcept {
    if (info.last_status == 0) return Error::signaling_failed;
    if (info.last_status == 487) return Error::cancelled;
    if (info.last_status == 415 || info.last_status == 488 ||
        info.last_status == 606)
        return Error::negotiation_failed;
    if (info.media_status == PJSUA_CALL_MEDIA_ERROR) return Error::media_failed;
    if (info.last_status >= 300 && info.last_status <= 699)
        return Error::remote_rejected;
    return Error::ok;
}
}

std::uint32_t PjsuaCallManager::NextToken() noexcept {
    ++next_token_;
    if (next_token_ == 0) ++next_token_;
    return next_token_;
}

PjsuaCallContext *PjsuaCallManager::Find(pjsua_call_id native_id) noexcept {
    for (PjsuaCallContext &context : contexts_)
        if (context.phase != PjsuaCallPhase::free && context.native_id == native_id)
            return &context;
    return nullptr;
}

PjsuaCallContext *PjsuaCallManager::FindToken(std::uint32_t token) noexcept {
    for (PjsuaCallContext &context : contexts_)
        if (context.phase != PjsuaCallPhase::free && context.token == token)
            return &context;
    return nullptr;
}

Error PjsuaCallManager::AdmitIncoming(CallHandle logical, AgentHandle agent,
                                      std::uint32_t token) noexcept {
    PjsuaCallContext *context = FindToken(token);
    if (context == nullptr || !logical.IsValid() ||
        context->agent.slot != agent.slot || context->agent.generation != agent.generation ||
        context->phase != PjsuaCallPhase::queued_incoming ||
        !OwnsNativeUserData(*context) || api_.call_setting_default == nullptr ||
        api_.call_answer2 == nullptr)
        return Error::invalid_handle;
    context->logical = logical;
    context->logical_bound = true;
    pjsua_call_setting setting{};
    api_.call_setting_default(&setting);
    if (api_.call_answer2(context->native_id, &setting, 180, nullptr, nullptr) != PJ_SUCCESS)
        return Error::signaling_failed;
    context->phase = PjsuaCallPhase::admitted_incoming;
    return Error::ok;
}

Error PjsuaCallManager::PromoteIncoming(CallHandle logical, AgentHandle agent,
                                        std::uint32_t token) noexcept {
    if (stopping_) return Error::shutting_down;
    PjsuaCallContext *context = FindToken(token);
    if (context == nullptr || !context->logical_bound ||
        context->logical.slot != logical.slot || context->logical.generation != logical.generation ||
        context->agent.slot != agent.slot || context->agent.generation != agent.generation ||
        context->phase != PjsuaCallPhase::admitted_incoming ||
        !OwnsNativeUserData(*context))
        return Error::invalid_handle;
    return Error::ok;
}

Error PjsuaCallManager::RejectProvisional(std::uint32_t token,
                                          std::uint16_t status) noexcept {
    PjsuaCallContext *context = FindToken(token);
    if (context == nullptr || !OwnsNativeUserData(*context))
        return Error::invalid_handle;
    Decline(context->native_id, status);
    return Error::ok;
}

Error PjsuaCallManager::Native(pjsua_call_id native_id,
                               pj_status_t status) const noexcept {
    (void)native_id;
    return status == PJ_SUCCESS ? Error::ok : Error::signaling_failed;
}

Error PjsuaCallManager::Answer(std::uint32_t token) noexcept {
    PjsuaCallContext *context = FindToken(token);
    if (context == nullptr || context->terminal_seen || !OwnsNativeUserData(*context) ||
        api_.call_setting_default == nullptr || api_.call_answer2 == nullptr)
        return Error::invalid_handle;
    pjsua_call_setting setting{};
    api_.call_setting_default(&setting);
    return Native(context->native_id,
                  api_.call_answer2(context->native_id, &setting, 200, nullptr,
                                    nullptr));
}

Error PjsuaCallManager::Reject(std::uint32_t token, std::uint16_t status) noexcept {
    if (status < 400 || status > 699) return Error::invalid_argument;
    PjsuaCallContext *context = FindToken(token);
    if (context == nullptr || !OwnsNativeUserData(*context) ||
        api_.call_answer == nullptr)
        return Error::invalid_handle;
    return Native(context->native_id,
                  api_.call_answer(context->native_id, status, nullptr, nullptr));
}

Error PjsuaCallManager::Cancel(std::uint32_t token) noexcept {
    PjsuaCallContext *context = FindToken(token);
    if (context == nullptr || !OwnsNativeUserData(*context) ||
        api_.call_hangup == nullptr)
        return Error::invalid_handle;
    return Native(context->native_id,
                  api_.call_hangup(context->native_id, 487, nullptr, nullptr));
}

Error PjsuaCallManager::Hangup(std::uint32_t token) noexcept {
    PjsuaCallContext *context = FindToken(token);
    if (context == nullptr || !OwnsNativeUserData(*context) ||
        api_.call_hangup == nullptr)
        return Error::invalid_handle;
    return Native(context->native_id,
                  api_.call_hangup(context->native_id, 0, nullptr, nullptr));
}

Error PjsuaCallManager::SetHeld(std::uint32_t token, bool held) noexcept {
    PjsuaCallContext *context = FindToken(token);
    if (context == nullptr || !OwnsNativeUserData(*context))
        return Error::invalid_handle;
    if (context->terminal_seen || !context->confirmed_once) return Error::invalid_state;
    if (context->media_control != PjsuaCallContext::MediaControl::none) return Error::busy;
    if ((held && context->phase != PjsuaCallPhase::confirmed) ||
        (!held && context->phase != PjsuaCallPhase::local_hold)) return Error::invalid_state;
    // Install before entering PJSUA: media callbacks may be synchronous.
    context->media_control = held ? PjsuaCallContext::MediaControl::hold
                                  : PjsuaCallContext::MediaControl::resume;
    Error result = Error::unsupported_configuration;
    if (held) {
        if (api_.call_set_hold2 != nullptr) result = Native(context->native_id,
                      api_.call_set_hold2(context->native_id, 0, nullptr));
    } else if (api_.call_setting_default != nullptr && api_.call_reinvite2 != nullptr) {
        pjsua_call_setting setting{};
        api_.call_setting_default(&setting);
        setting.flag |= PJSUA_CALL_UNHOLD;
        result = Native(context->native_id,
                  api_.call_reinvite2(context->native_id, &setting, nullptr));
    }
    if (result != Error::ok && context->media_control != PjsuaCallContext::MediaControl::result)
        context->media_control = PjsuaCallContext::MediaControl::none;
    return result;
}

PjsuaCallContext *PjsuaCallManager::Reserve(AgentHandle agent,
                                            pjsua_call_id native_id) noexcept {
    if (Find(native_id) != nullptr) return nullptr;
    for (PjsuaCallContext &context : contexts_) {
        if (context.phase != PjsuaCallPhase::free) continue;
        context = {};
        context.token = NextToken();
        context.agent = agent;
        context.native_id = native_id;
        context.phase = PjsuaCallPhase::provisional_incoming;
        context.incoming = true;
        ++count_;
        return &context;
    }
    return nullptr;
}

PjsuaCallContext *PjsuaCallManager::ReserveOutgoing(CallHandle logical,
                                                     AgentHandle agent) noexcept {
    if (!logical.IsValid()) return nullptr;
    for (PjsuaCallContext &context : contexts_) {
        if (context.phase != PjsuaCallPhase::free) continue;
        context = {};
        context.token = NextToken();
        context.logical = logical;
        context.logical_bound = true;
        context.agent = agent;
        context.phase = PjsuaCallPhase::calling;
        ++count_;
        return &context;
    }
    return nullptr;
}

Error PjsuaCallManager::PromoteOutgoing(CallHandle logical, AgentHandle agent,
                                        const char *uri,
                                        std::uint32_t *token) noexcept {
    if (stopping_) return Error::shutting_down;
    if (uri == nullptr || token == nullptr || !logical.IsValid())
        return Error::invalid_argument;
    *token = 0;
    pjsua_acc_id account = PJSUA_INVALID_ID;
    if (accounts_.NativeId(agent, &account) != Error::ok)
        return Error::invalid_handle;
    if (api_.call_setting_default == nullptr || api_.call_make_call == nullptr ||
        api_.call_get_user_data == nullptr)
        return Error::unsupported_configuration;
    char copied_uri[max_uri_length + 1]{};
    std::size_t length = 0;
    while (length <= max_uri_length && uri[length] != '\0') ++length;
    if (length == 0 || length > max_uri_length) return Error::invalid_argument;
    std::memcpy(copied_uri, uri, length + 1);
    PjsuaCallContext *context = ReserveOutgoing(logical, agent);
    if (context == nullptr) return Error::resource_exhausted;
    pjsua_call_setting setting{};
    api_.call_setting_default(&setting);
    setting.aud_cnt = 1;
    setting.vid_cnt = 0;
    pj_str_t destination{copied_uri, static_cast<pj_ssize_t>(length)};
    pjsua_call_id native_id = PJSUA_INVALID_ID;
    const pj_status_t status = api_.call_make_call(account, &destination,
                                                    &setting, context, nullptr,
                                                    &native_id);
    if (status != PJ_SUCCESS) {
        Release(*context);
        return status == PJ_ETOOMANY ? Error::resource_exhausted
                                     : Error::signaling_failed;
    }
    context->native_id = native_id;
    context->user_data_bound = api_.call_get_user_data(native_id) == context;
    if (!context->user_data_bound) {
        // Do not recycle storage after native make-call acceptance. A callback
        // may still observe the address supplied to PJSUA. Only detach and
        // hang up after a fresh exact ownership check; never clear a foreign
        // user-data pointer.
        if (api_.call_get_user_data(native_id) == context) {
            context->user_data_bound = true;
            if (api_.call_set_user_data != nullptr)
                (void)api_.call_set_user_data(native_id, nullptr);
            context->user_data_bound = false;
        }
        if (api_.call_hangup != nullptr)
            (void)api_.call_hangup(native_id, 0, nullptr, nullptr);
        context->phase = PjsuaCallPhase::disconnecting;
        return Error::signaling_failed;
    }
    *token = context->token;
    return Error::ok;
}

bool PjsuaCallManager::OwnsNativeUserData(const PjsuaCallContext &context) const noexcept {
    return context.user_data_bound && api_.call_get_user_data != nullptr &&
           api_.call_get_user_data(context.native_id) == &context;
}

bool PjsuaCallManager::EnqueueIncoming(PjsuaCallContext &context,
                                       const pjsua_call_info &info) noexcept {
    return EnqueueCall(context, RuntimeNotification::Type::incoming_call, info);
}

RuntimeNotification PjsuaCallManager::CopyCall(const PjsuaCallContext &context,
                                               RuntimeNotification::Type type,
                                               const pjsua_call_info &info,
                                               Error error) const noexcept {
    RuntimeNotification notification{};
    notification.type = type;
    notification.agent = context.agent;
    notification.token = context.token;
    notification.sip_status = static_cast<std::uint16_t>(info.last_status);
    notification.error = error;
    notification.status.error = error;
    notification.status.sip_status = notification.sip_status;
    notification.media_active = info.media_status == PJSUA_CALL_MEDIA_ACTIVE;
    notification.media_send_enabled = notification.media_active &&
        (info.media_dir & PJMEDIA_DIR_ENCODING) != 0;
    notification.media_receive_enabled = notification.media_active &&
        (info.media_dir & PJMEDIA_DIR_DECODING) != 0;
    if (info.remote_info.ptr != nullptr && info.remote_info.slen > 0) {
        const std::size_t length = static_cast<std::size_t>(info.remote_info.slen) > max_uri_length
            ? max_uri_length : static_cast<std::size_t>(info.remote_info.slen);
        std::memcpy(notification.remote_uri, info.remote_info.ptr, length);
        notification.remote_uri[length] = '\0';
    }
    if (info.remote_contact.ptr != nullptr && info.remote_contact.slen > 0) {
        const std::size_t length = static_cast<std::size_t>(info.remote_contact.slen) > max_address_length
            ? max_address_length : static_cast<std::size_t>(info.remote_contact.slen);
        std::memcpy(notification.remote_address, info.remote_contact.ptr, length);
        notification.remote_address[length] = '\0';
    }
    if (info.last_status_text.ptr != nullptr && info.last_status_text.slen > 0) {
        const std::size_t length = static_cast<std::size_t>(info.last_status_text.slen) > max_reason_length
            ? max_reason_length : static_cast<std::size_t>(info.last_status_text.slen);
        for (std::size_t i = 0; i < length; ++i) {
            const unsigned char c = static_cast<unsigned char>(info.last_status_text.ptr[i]);
            notification.status.reason[i] = c < 32 || c == 127 ? ' ' : static_cast<char>(c);
        }
        notification.status.reason[length] = '\0';
    }
    return notification;
}

bool PjsuaCallManager::EnqueueCall(PjsuaCallContext &context,
                                   RuntimeNotification::Type type,
                                   const pjsua_call_info &info,
                                   Error error) noexcept {
    PjsuaCallNotification &record = type == RuntimeNotification::Type::incoming_call
        ? context.admission : (type == RuntimeNotification::Type::call_held ||
                              type == RuntimeNotification::Type::call_resumed)
        ? context.media_result : context.outcome;
    // Producers are one-shot or gated by media_control until consumption.
    record.notification = CopyCall(context, type, info, error);
    if (++next_sequence_ == 0) ++next_sequence_;
    record.sequence = next_sequence_;
    return true;
}

void PjsuaCallManager::TryRelease(PjsuaCallContext &context) noexcept {
    if (!context.terminal_seen || context.terminal_reported ||
        !context.media_quiescent || context.callback_depth != 0 ||
        !context.future_security_clear) return;
    if (OwnsNativeUserData(context)) {
        if (api_.call_set_user_data == nullptr ||
            api_.call_set_user_data(context.native_id, nullptr) != PJ_SUCCESS) return;
    }
    context.user_data_bound = false;
    if (!context.logical_bound && context.admission.sequence == 0) {
        Release(context);
        return;
    }
    // Native ownership is unreachable. Retain copied records and the context
    // reservation until the actor consumes finish; never reuse behind a backlog.
    context.terminal_reported = true;
    if (++next_sequence_ == 0) ++next_sequence_;
    context.finish.sequence = next_sequence_;
}

void PjsuaCallManager::FinishCallback(PjsuaCallContext &context) noexcept {
    if (context.callback_depth != 0) --context.callback_depth;
    TryRelease(context);
}

void PjsuaCallManager::Decline(pjsua_call_id native_id, unsigned status) noexcept {
    if (api_.call_answer != nullptr) (void)api_.call_answer(native_id, status, nullptr, nullptr);
    if (api_.call_hangup != nullptr) (void)api_.call_hangup(native_id, status, nullptr, nullptr);
}

void PjsuaCallManager::Release(PjsuaCallContext &context) noexcept {
    if (context.user_data_bound && api_.call_set_user_data != nullptr &&
        api_.call_get_user_data != nullptr &&
        api_.call_get_user_data(context.native_id) == &context)
        (void)api_.call_set_user_data(context.native_id, nullptr);
    context = {};
    if (count_ != 0) --count_;
}

void PjsuaCallManager::OnIncomingCall(pjsua_acc_id account, pjsua_call_id native_id) noexcept {
    if (stopping_) { Decline(native_id, 486); return; }
    PjsuaAccountContext *account_context = accounts_.Resolve(account);
    if (account_context == nullptr) {
        Decline(native_id, 404);
        return;
    }
    if (api_.call_set_user_data == nullptr || api_.call_get_user_data == nullptr ||
        api_.call_get_info == nullptr) {
        Decline(native_id, 486);
        return;
    }
    // A PJSUA call can have a native owner outside this manager. Never replace
    // or later clear that owner's pointer. A duplicate callback for one of our
    // contexts is likewise declined without rebinding the already-owned slot.
    if (api_.call_get_user_data(native_id) != nullptr || Find(native_id) != nullptr) {
        Decline(native_id, 486);
        return;
    }
    PjsuaCallContext *context = Reserve(account_context->agent, native_id);
    if (context == nullptr) {
        Decline(native_id, 486);
        return;
    }
    if (api_.call_set_user_data(native_id, context) != PJ_SUCCESS) {
        Release(*context);
        Decline(native_id, 486);
        return;
    }
    context->user_data_bound = true;
    pjsua_call_info info{};
    if (api_.call_get_info(native_id, &info) != PJ_SUCCESS ||
        !EnqueueIncoming(*context, info)) {
        Release(*context);
        Decline(native_id, 486);
        return;
    }
    context->phase = PjsuaCallPhase::queued_incoming;
}

void PjsuaCallManager::OnCallState(pjsua_call_id native_id) noexcept {
    PjsuaCallContext *context = Find(native_id);
    if (context == nullptr || !OwnsNativeUserData(*context) || api_.call_get_info == nullptr) return;
    ++context->callback_depth;
    pjsua_call_info info{};
    if (api_.call_get_info(native_id, &info) != PJ_SUCCESS) {
        FinishCallback(*context);
        return;
    }
    context->media_quiescent = info.media_status != PJSUA_CALL_MEDIA_ACTIVE;
    if (info.state == PJSIP_INV_STATE_CALLING &&
        (context->phase == PjsuaCallPhase::calling ||
         context->phase == PjsuaCallPhase::promoting)) {
        context->phase = PjsuaCallPhase::calling;
    } else if (info.state == PJSIP_INV_STATE_EARLY &&
               (context->phase == PjsuaCallPhase::calling ||
                context->phase == PjsuaCallPhase::promoting ||
                context->phase == PjsuaCallPhase::admitted_incoming)) {
        context->phase = PjsuaCallPhase::early;
    } else if (info.state == PJSIP_INV_STATE_CONFIRMED && !context->terminal_seen) {
        if (!context->confirmed_once) {
            context->confirmed_once = true;
            context->phase = PjsuaCallPhase::confirmed;
            (void)EnqueueCall(*context, RuntimeNotification::Type::call_accepted,
                              info);
        }
    } else if (info.state == PJSIP_INV_STATE_DISCONNECTED) {
        context->phase = PjsuaCallPhase::disconnected;
        if (!context->terminal_seen) {
            context->terminal_seen = true;
            const bool rejected = info.last_status >= 300 && info.last_status <= 699 &&
                                  !context->confirmed_once;
            if (rejected && context->logical_bound)
                (void)EnqueueCall(*context, RuntimeNotification::Type::call_rejected,
                                  info, TerminalError(info));
            const Error terminal_error = TerminalError(info);
            context->finish.notification = CopyCall(*context,
                RuntimeNotification::Type::call_finished, info, terminal_error);
        }
    }
    FinishCallback(*context);
}

void PjsuaCallManager::OnCallMediaState(pjsua_call_id native_id) noexcept {
    PjsuaCallContext *context = Find(native_id);
    if (context == nullptr || !OwnsNativeUserData(*context) || api_.call_get_info == nullptr) return;
    ++context->callback_depth;
    pjsua_call_info info{};
    if (api_.call_get_info(native_id, &info) == PJ_SUCCESS) {
        context->media_quiescent = info.media_status != PJSUA_CALL_MEDIA_ACTIVE;
        if (context->phase == PjsuaCallPhase::confirmed ||
            context->phase == PjsuaCallPhase::local_hold) {
            if (info.media_status == PJSUA_CALL_MEDIA_ERROR &&
                (context->media_control == PjsuaCallContext::MediaControl::hold ||
                 context->media_control == PjsuaCallContext::MediaControl::resume)) {
                const auto type = context->media_control == PjsuaCallContext::MediaControl::hold
                    ? RuntimeNotification::Type::call_held : RuntimeNotification::Type::call_resumed;
                context->media_control = PjsuaCallContext::MediaControl::result;
                (void)EnqueueCall(*context, type, info, Error::media_failed);
            }
            const bool active = info.media_status == PJSUA_CALL_MEDIA_ACTIVE;
            const bool held = active && info.media_dir == PJMEDIA_DIR_ENCODING;
            if (held && context->media_control == PjsuaCallContext::MediaControl::hold) {
                context->media_control = PjsuaCallContext::MediaControl::result;
                context->phase = PjsuaCallPhase::local_hold;
                (void)EnqueueCall(*context, RuntimeNotification::Type::call_held, info);
            } else if (active && info.media_dir == PJMEDIA_DIR_ENCODING_DECODING &&
                       context->media_control == PjsuaCallContext::MediaControl::resume) {
                context->media_control = PjsuaCallContext::MediaControl::result;
                context->phase = PjsuaCallPhase::confirmed;
                (void)EnqueueCall(*context, RuntimeNotification::Type::call_resumed, info);
            }
        }
    }
    FinishCallback(*context);
}

bool PjsuaCallManager::TryGetNotification(RuntimeNotification *notification) noexcept {
    if (notification == nullptr) return false;
    PjsuaCallNotification *oldest = nullptr;
    PjsuaCallContext *owner = nullptr;
    for (auto &context : contexts_) {
        TryRelease(context);
        for (auto *record : {&context.admission, &context.outcome,
                             &context.media_result, &context.finish}) {
            if (record->sequence != 0 && (oldest == nullptr ||
                static_cast<std::int64_t>(record->sequence - oldest->sequence) < 0)) {
                oldest = record; owner = &context;
            }
        }
    }
    if (oldest == nullptr) return false;
    *notification = oldest->notification;
    oldest->sequence = 0;
    if (oldest == &owner->media_result)
        owner->media_control = PjsuaCallContext::MediaControl::none;
    if (oldest == &owner->finish) Release(*owner);
    return true;
}

Error PjsuaCallManager::BeginShutdown() noexcept {
    stopping_ = true;
    bool pending = false;
    for (PjsuaCallContext &context : contexts_) {
        if (context.phase == PjsuaCallPhase::free) continue;
        if (context.phase != PjsuaCallPhase::disconnected &&
            context.phase != PjsuaCallPhase::disconnecting && api_.call_hangup != nullptr) {
            context.phase = PjsuaCallPhase::disconnecting;
            (void)api_.call_hangup(context.native_id, 0, nullptr, nullptr);
        }
        TryRelease(context);
        if (context.phase != PjsuaCallPhase::free) pending = true;
    }
    return pending ? Error::busy : Error::ok;
}

void PjsuaCallManager::Reset() noexcept {
    for (PjsuaCallContext &context : contexts_)
        if (context.phase != PjsuaCallPhase::free) Release(context);
    next_sequence_ = 0;
    stopping_ = false;
}

} // namespace voip
