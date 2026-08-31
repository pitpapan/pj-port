#ifndef VOIP_PJSUA_CALLBACK_ROUTER_HPP
#define VOIP_PJSUA_CALLBACK_ROUTER_HPP

#include "PjsuaApi.hpp"
#include <voip/VoipTypes.hpp>
#include <array>
#include <cstdint>
#include <thread>

namespace voip {
struct PjsuaRegistrationRecord {
    pjsua_acc_id account{PJSUA_INVALID_ID};
    pj_status_t native_status{PJ_SUCCESS};
    int sip_status{};
    char reason[max_reason_length + 1]{};
    bool renew{};
    bool unregistration{};
    unsigned expiration{};
};
class PjsuaCallbackSink {
public:
    virtual ~PjsuaCallbackSink() noexcept = default;
    virtual void OnRegistrationStarted(const PjsuaRegistrationRecord &) noexcept = 0;
    virtual void OnRegistrationState(const PjsuaRegistrationRecord &) noexcept = 0;
};
class PjsuaCallbackRouter final {
public:
    explicit PjsuaCallbackRouter(PjsuaCallbackSink &sink,
                                 const PjsuaApi &api = NativePjsuaApi()) noexcept
        : sink_(sink), api_(api) {}
    Error Attach(pjsua_callback *) noexcept;
    void BeginQuiescence() noexcept;
    void MarkNativeDestroyed() noexcept;
    void Detach() noexcept;
private:
    static void OnRegistrationStarted(pjsua_acc_id, pjsua_reg_info *) noexcept;
    static void OnRegistrationState(pjsua_acc_id, pjsua_reg_info *) noexcept;
    static void OnIncomingCall(pjsua_acc_id, pjsua_call_id, pjsip_rx_data *) noexcept;
    void ForwardStarted(pjsua_acc_id, const pjsua_reg_info *) noexcept;
    void ForwardState(pjsua_acc_id, const pjsua_reg_info *) noexcept;
    void GuardIncoming(pjsua_call_id) noexcept;
    void AssertActor() const noexcept;
    PjsuaCallbackSink &sink_; const PjsuaApi &api_;
    bool quiescent_ = false; bool attached_ = false; bool native_destroyed_ = false;
    std::thread::id actor_thread_{};
    static PjsuaCallbackRouter *active_;
};
} // namespace voip
#endif
