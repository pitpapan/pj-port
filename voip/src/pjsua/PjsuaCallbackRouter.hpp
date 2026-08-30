#ifndef VOIP_PJSUA_CALLBACK_ROUTER_HPP
#define VOIP_PJSUA_CALLBACK_ROUTER_HPP

#include "PjsuaApi.hpp"
#include <array>

namespace voip {
struct PjsuaRegistrationRecord { pjsua_acc_id account = PJSUA_INVALID_ID; bool renew = false; };
class PjsuaCallbackSink {
public:
    virtual ~PjsuaCallbackSink() noexcept = default;
    virtual void OnRegistrationStarted(const PjsuaRegistrationRecord &) noexcept = 0;
    virtual void OnRegistrationState(const PjsuaRegistrationRecord &) noexcept = 0;
};
class PjsuaCallbackRouter final {
public:
    explicit PjsuaCallbackRouter(PjsuaCallbackSink &sink) noexcept : sink_(sink) {}
    Error Attach(pjsua_callback *) noexcept;
    void BeginQuiescence() noexcept { quiescent_ = true; }
    void Detach() noexcept;
private:
    static void OnRegistrationStarted(pjsua_acc_id, pjsua_reg_info *) noexcept;
    static void OnRegistrationState(pjsua_acc_id, pjsua_reg_info *) noexcept;
    static void OnIncomingCall(pjsua_acc_id, pjsua_call_id, pjsip_rx_data *) noexcept;
    void ForwardStarted(pjsua_acc_id, pjsua_reg_info *) noexcept;
    void ForwardState(pjsua_acc_id, pjsua_reg_info *) noexcept;
    PjsuaCallbackSink &sink_; bool quiescent_ = false; bool attached_ = false;
    static PjsuaCallbackRouter *active_;
};
} // namespace voip
#endif
