#include "PjsuaCallManager.hpp"

#include <cstring>

namespace voip {

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

bool PjsuaCallManager::OwnsNativeUserData(const PjsuaCallContext &context) const noexcept {
    return context.user_data_bound && api_.call_get_user_data != nullptr &&
           api_.call_get_user_data(context.native_id) == &context;
}

bool PjsuaCallManager::EnqueueIncoming(const PjsuaCallContext &context,
                                       const pjsua_call_info &info) noexcept {
    if (notification_size_ == notifications_.size()) return false;
    RuntimeNotification notification{};
    notification.type = RuntimeNotification::Type::incoming_call;
    notification.agent = context.agent;
    notification.token = context.token;
    if (info.remote_info.ptr != nullptr && info.remote_info.slen > 0) {
        const std::size_t length = static_cast<std::size_t>(info.remote_info.slen) > max_uri_length
            ? max_uri_length : static_cast<std::size_t>(info.remote_info.slen);
        std::memcpy(notification.remote_uri, info.remote_info.ptr, length);
        notification.remote_uri[length] = '\0';
    }
    notifications_[(notification_head_ + notification_size_) % notifications_.size()].notification = notification;
    ++notification_size_;
    return true;
}

void PjsuaCallManager::Decline(pjsua_call_id native_id, unsigned status) noexcept {
    if (api_.call_answer != nullptr) (void)api_.call_answer(native_id, status, nullptr, nullptr);
    if (api_.call_hangup != nullptr) (void)api_.call_hangup(native_id, status, nullptr, nullptr);
}

void PjsuaCallManager::Release(PjsuaCallContext &context) noexcept {
    if (context.user_data_bound && api_.call_set_user_data != nullptr)
        (void)api_.call_set_user_data(context.native_id, nullptr);
    context = {};
    if (count_ != 0) --count_;
}

void PjsuaCallManager::OnIncomingCall(pjsua_acc_id account, pjsua_call_id native_id) noexcept {
    PjsuaAccountContext *account_context = accounts_.Resolve(account);
    if (account_context == nullptr || api_.call_set_user_data == nullptr ||
        api_.call_get_info == nullptr) {
        Decline(native_id, 404);
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
    pjsua_call_info info{};
    if (api_.call_get_info(native_id, &info) != PJ_SUCCESS) return;
    if (info.state == PJSIP_INV_STATE_DISCONNECTED) {
        context->phase = PjsuaCallPhase::disconnected;
        Release(*context);
    }
}

void PjsuaCallManager::OnCallMediaState(pjsua_call_id native_id) noexcept {
    PjsuaCallContext *context = Find(native_id);
    if (context == nullptr || !OwnsNativeUserData(*context) || api_.call_get_info == nullptr) return;
    pjsua_call_info info{};
    (void)api_.call_get_info(native_id, &info);
}

bool PjsuaCallManager::TryGetNotification(RuntimeNotification *notification) noexcept {
    if (notification == nullptr || notification_size_ == 0) return false;
    *notification = notifications_[notification_head_].notification;
    notification_head_ = (notification_head_ + 1) % notifications_.size();
    --notification_size_;
    return true;
}

void PjsuaCallManager::Reset() noexcept {
    for (PjsuaCallContext &context : contexts_)
        if (context.phase != PjsuaCallPhase::free) Release(context);
    notification_head_ = 0;
    notification_size_ = 0;
}

} // namespace voip
