#include "PjsuaCallbackRouter.hpp"
#include <cassert>
namespace voip {
PjsuaCallbackRouter *PjsuaCallbackRouter::active_ = nullptr;
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
    assert(native_destroyed_);
    if (active_ == this) active_ = nullptr;
    attached_ = false;
    actor_thread_ = {};
}
void PjsuaCallbackRouter::OnRegistrationStarted(pjsua_acc_id account, pjsua_reg_info *info) noexcept { if (active_ != nullptr) active_->ForwardStarted(account, info); }
void PjsuaCallbackRouter::OnRegistrationState(pjsua_acc_id account, pjsua_reg_info *info) noexcept { if (active_ != nullptr) active_->ForwardState(account, info); }
void PjsuaCallbackRouter::OnIncomingCall(pjsua_acc_id, pjsua_call_id call, pjsip_rx_data *) noexcept {
    if (active_ != nullptr && !active_->quiescent_) active_->GuardIncoming(call);
}
void PjsuaCallbackRouter::GuardIncoming(pjsua_call_id call) noexcept {
    AssertActor();
    (void)api_.call_answer(call, 486, nullptr, nullptr);
    (void)api_.call_hangup(call, 486, nullptr, nullptr);
}
void PjsuaCallbackRouter::ForwardStarted(pjsua_acc_id account, pjsua_reg_info *info) noexcept {
    AssertActor();
    sink_.OnRegistrationStarted({account, info != nullptr && info->renew != PJ_FALSE});
}
void PjsuaCallbackRouter::ForwardState(pjsua_acc_id account, pjsua_reg_info *info) noexcept {
    AssertActor();
    sink_.OnRegistrationState({account, info != nullptr && info->renew != PJ_FALSE});
}
} // namespace voip
