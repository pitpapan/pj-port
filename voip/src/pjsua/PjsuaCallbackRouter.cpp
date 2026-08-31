#include "PjsuaCallbackRouter.hpp"
#include <cassert>
#include <cstring>
namespace voip {
PjsuaCallbackRouter *PjsuaCallbackRouter::active_ = nullptr;
#if defined(CONFIG_VOIP_PJSUA_PLAN3_COMPONENT_TEST)
unsigned PjsuaCallbackRouter::callback_entry_count_ = 0;
const PjsuaCallbackRouter *PjsuaCallbackRouter::ActiveForTest() noexcept {
    return active_;
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
    if (callbacks == nullptr || active_ != nullptr || attached_) return Error::invalid_state;
    actor_thread_ = std::this_thread::get_id();
    native_destroyed_ = false;
    active_ = this; attached_ = true;
    callbacks->on_reg_started2 = &OnRegistrationStarted;
    callbacks->on_reg_state2 = &OnRegistrationState;
    callbacks->on_incoming_call = &OnIncomingCall;
    return Error::ok;
}
void PjsuaCallbackRouter::BeginQuiescence() noexcept {
    AssertActor();
    quiescent_ = true;
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
void PjsuaCallbackRouter::OnIncomingCall(pjsua_acc_id, pjsua_call_id call, pjsip_rx_data *) noexcept {
#if defined(CONFIG_VOIP_PJSUA_PLAN3_COMPONENT_TEST)
    ++callback_entry_count_;
#endif
    if (active_ != nullptr) active_->GuardIncoming(call);
}
void PjsuaCallbackRouter::GuardIncoming(pjsua_call_id call) noexcept {
    AssertActor();
    (void)api_.call_answer(call, 486, nullptr, nullptr);
    (void)api_.call_hangup(call, 486, nullptr, nullptr);
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
