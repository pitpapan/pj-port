#include "PjsuaCallbackRouter.hpp"
#include <cassert>
namespace voip {
PjsuaCallbackRouter *PjsuaCallbackRouter::active_ = nullptr;
Error PjsuaCallbackRouter::Attach(pjsua_callback *callbacks) noexcept {
    if (callbacks == nullptr || active_ != nullptr || attached_) return Error::invalid_state;
    active_ = this; attached_ = true;
    callbacks->on_reg_started2 = &OnRegistrationStarted;
    callbacks->on_reg_state2 = &OnRegistrationState;
    callbacks->on_incoming_call = &OnIncomingCall;
    return Error::ok;
}
void PjsuaCallbackRouter::Detach() noexcept { if (active_ == this) active_ = nullptr; attached_ = false; }
void PjsuaCallbackRouter::OnRegistrationStarted(pjsua_acc_id account, pjsua_reg_info *info) noexcept { if (active_ != nullptr) active_->ForwardStarted(account, info); }
void PjsuaCallbackRouter::OnRegistrationState(pjsua_acc_id account, pjsua_reg_info *info) noexcept { if (active_ != nullptr) active_->ForwardState(account, info); }
void PjsuaCallbackRouter::OnIncomingCall(pjsua_acc_id, pjsua_call_id call, pjsip_rx_data *) noexcept {
    if (active_ != nullptr && !active_->quiescent_) {
        (void)pjsua_call_answer(call, 486, nullptr, nullptr);
        (void)pjsua_call_hangup(call, 486, nullptr, nullptr);
    }
}
void PjsuaCallbackRouter::ForwardStarted(pjsua_acc_id account, pjsua_reg_info *info) noexcept { sink_.OnRegistrationStarted({account, info != nullptr && info->renew != PJ_FALSE}); }
void PjsuaCallbackRouter::ForwardState(pjsua_acc_id account, pjsua_reg_info *info) noexcept { sink_.OnRegistrationState({account, info != nullptr && info->renew != PJ_FALSE}); }
} // namespace voip
