#include "PjsuaCallbackRouter.hpp"
#include "PjsuaMediaManager.hpp"
#include <cassert>
#include <cstring>
namespace voip {
PjsuaCallbackRouter *PjsuaCallbackRouter::active_ = nullptr;
#if defined(CONFIG_VOIP_PJSUA_PLAN3_COMPONENT_TEST) || defined(CONFIG_VOIP_PJSUA_REGISTRATION_TEST)
unsigned PjsuaCallbackRouter::callback_entry_count_ = 0;
unsigned PjsuaCallbackRouter::attach_attempt_count_ = 0;
const PjsuaCallbackRouter *PjsuaCallbackRouter::ActiveForTest() noexcept {
    return active_;
}
unsigned PjsuaCallbackRouter::AttachAttemptCountForTest() noexcept {
    return attach_attempt_count_;
}
unsigned PjsuaCallbackRouter::CallbackEntryCountForTest() noexcept {
    return callback_entry_count_;
}
#endif
void PjsuaCallbackRouter::AssertActor() const noexcept {
#ifndef NDEBUG
    assert(actor_thread_ == std::thread::id{} || actor_thread_ == std::this_thread::get_id());
#endif
}
Error PjsuaCallbackRouter::Attach(pjsua_callback *callbacks) noexcept {
    AssertActor();
#if defined(CONFIG_VOIP_PJSUA_PLAN3_COMPONENT_TEST) || defined(CONFIG_VOIP_PJSUA_REGISTRATION_TEST)
    ++attach_attempt_count_;
#endif
    if (callbacks == nullptr || active_ != nullptr || attached_) return Error::invalid_state;
    actor_thread_ = std::this_thread::get_id();
    native_destroyed_ = false;
    active_ = this; attached_ = true;
    callbacks->on_reg_started2 = &OnRegistrationStarted;
    callbacks->on_reg_state2 = &OnRegistrationState;
    callbacks->on_incoming_call = &OnIncomingCall;
    callbacks->on_call_state = &OnCallState;
    callbacks->on_call_media_state = &OnCallMediaState;
    callbacks->on_call_tsx_state = &OnCallTransaction;
    callbacks->on_call_media_update_result = &OnCallMediaUpdateResult;
    if (media_) callbacks->on_conf_op_completed=&OnConferenceOperation;
    return Error::ok;
}
void PjsuaCallbackRouter::BeginQuiescence() noexcept {
    AssertActor();
    quiescent_ = true;
}
void PjsuaCallbackRouter::OnConferenceOperation(const pjmedia_conf_op_info *info) noexcept {
    if (!active_ || !active_->media_ || !info) return;
    active_->AssertActor();
    const pjmedia_conf_op_info copied=*info;
    active_->media_->OnConferenceOperation(copied);
}
void PjsuaCallbackRouter::MarkNativeDestroyed() noexcept {
    AssertActor();
    native_destroyed_ = true;
}
void PjsuaCallbackRouter::Detach() noexcept {
    AssertActor();
    if (!native_destroyed_) return;
    if (active_ == this) active_ = nullptr;
    attached_ = false;
    actor_thread_ = {};
}
void PjsuaCallbackRouter::OnRegistrationStarted(pjsua_acc_id account, pjsua_reg_info *info) noexcept {
#if defined(CONFIG_VOIP_PJSUA_PLAN3_COMPONENT_TEST)
    ++callback_entry_count_;
#endif
    if (active_ != nullptr) active_->ForwardStarted(account, info);
}
void PjsuaCallbackRouter::OnRegistrationState(pjsua_acc_id account, pjsua_reg_info *info) noexcept {
#if defined(CONFIG_VOIP_PJSUA_PLAN3_COMPONENT_TEST)
    ++callback_entry_count_;
#endif
    if (active_ != nullptr) active_->ForwardState(account, info);
}
void PjsuaCallbackRouter::OnIncomingCall(pjsua_acc_id account, pjsua_call_id call, pjsip_rx_data *) noexcept {
#if defined(CONFIG_VOIP_PJSUA_PLAN3_COMPONENT_TEST)
    ++callback_entry_count_;
#endif
    if (active_ != nullptr) active_->ForwardIncoming(account, call);
}
void PjsuaCallbackRouter::OnCallState(pjsua_call_id call, pjsip_event *event) noexcept {
    // A transport error can disconnect the dialog before its normal
    // transaction callback. Copy the pending INVITE result before release.
    if (event != nullptr && event->type == PJSIP_EVENT_TSX_STATE)
        OnCallTransaction(call, event->body.tsx_state.tsx, event);
    if (active_ != nullptr) active_->ForwardCallState(call);
}
void PjsuaCallbackRouter::OnCallTransaction(pjsua_call_id call,
        pjsip_transaction *tsx, pjsip_event *event) noexcept {
    if (active_ == nullptr || active_->calls_ == nullptr || tsx == nullptr) return;
    active_->AssertActor();
    PjsuaCallTransactionRecord record{};
    record.native_id = call; record.method = tsx->method.id;
    record.role = tsx->role; record.cseq = tsx->cseq;
    record.state = tsx->state; record.sip_status = tsx->status_code;
    record.transport_result = tsx->transport_err;
    record.auth_retry = tsx->last_tx != nullptr && tsx->last_tx->auth_retry;
    if (event != nullptr && event->type == PJSIP_EVENT_TSX_STATE) {
        record.timeout = event->body.tsx_state.type == PJSIP_EVENT_TIMER;
        if (event->body.tsx_state.type == PJSIP_EVENT_TRANSPORT_ERROR)
            record.transport_result = event->body.tsx_state.src.status;
    }
    if (tsx->status_text.ptr != nullptr && tsx->status_text.slen > 0) {
        const auto length = static_cast<std::size_t>(tsx->status_text.slen);
        const auto copied = length > max_reason_length ? max_reason_length : length;
        std::memcpy(record.reason, tsx->status_text.ptr, copied);
    }
    active_->calls_->OnCallTransaction(record);
}
void PjsuaCallbackRouter::OnCallMediaUpdateResult(pjsua_call_id call,
                                                pj_status_t status) noexcept {
    if (active_ == nullptr || active_->calls_ == nullptr) return;
    active_->AssertActor();
    active_->calls_->OnCallMediaUpdateResult(call, status);
}
void PjsuaCallbackRouter::OnCallMediaState(pjsua_call_id call) noexcept {
    if (active_ != nullptr) active_->ForwardCallMediaState(call);
}
void PjsuaCallbackRouter::ForwardIncoming(pjsua_acc_id account, pjsua_call_id call) noexcept {
    AssertActor();
    if (calls_ != nullptr) calls_->OnIncomingCall(account, call);
    else {
        (void)api_.call_answer(call, 486, nullptr, nullptr);
        (void)api_.call_hangup(call, 486, nullptr, nullptr);
    }
}
void PjsuaCallbackRouter::ForwardCallState(pjsua_call_id call) noexcept {
    AssertActor();
    if (calls_ != nullptr) calls_->OnCallState(call);
}
void PjsuaCallbackRouter::ForwardCallMediaState(pjsua_call_id call) noexcept {
    AssertActor();
    if (calls_ != nullptr) calls_->OnCallMediaState(call);
}
namespace {
PjsuaRegistrationRecord CopyRegistration(pjsua_acc_id account,
                                         const pjsua_reg_info *info) noexcept {
    PjsuaRegistrationRecord result{};
    result.account = account;
    if (info == nullptr) return result;
    result.renew = info->renew != PJ_FALSE;
    const pjsip_regc_cbparam *param = info->cbparam;
    if (param == nullptr) return result;
    result.native_status = param->status;
    result.sip_status = param->code;
    result.unregistration = param->is_unreg != PJ_FALSE;
    result.expiration = param->expiration;
    if (param->reason.ptr != nullptr && param->reason.slen > 0) {
        const std::size_t length = static_cast<std::size_t>(param->reason.slen) > max_reason_length
            ? max_reason_length : static_cast<std::size_t>(param->reason.slen);
        std::memcpy(result.reason, param->reason.ptr, length);
        result.reason[length] = '\0';
    }
    return result;
}
}
void PjsuaCallbackRouter::ForwardStarted(pjsua_acc_id account, const pjsua_reg_info *info) noexcept {
    AssertActor();
    sink_.OnRegistrationStarted(CopyRegistration(account, info));
}
void PjsuaCallbackRouter::ForwardState(pjsua_acc_id account, const pjsua_reg_info *info) noexcept {
    AssertActor();
    sink_.OnRegistrationState(CopyRegistration(account, info));
}
} // namespace voip
