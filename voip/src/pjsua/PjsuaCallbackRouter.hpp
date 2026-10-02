#ifndef VOIP_PJSUA_CALLBACK_ROUTER_HPP
#define VOIP_PJSUA_CALLBACK_ROUTER_HPP

#include "PjsuaApi.hpp"
#include "PjsuaCallRecords.hpp"
#include <voip/VoipTypes.hpp>
#include <array>
#include <cstdint>
#include <thread>

namespace voip {
class PjsuaMediaManager;
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
class PjsuaCallCallbackSink {
public:
    virtual ~PjsuaCallCallbackSink() noexcept = default;
    virtual void OnIncomingCall(pjsua_acc_id, pjsua_call_id) noexcept = 0;
    virtual void OnCallState(pjsua_call_id) noexcept = 0;
    virtual void OnCallMediaState(pjsua_call_id) noexcept = 0;
    virtual void OnCallTransaction(const PjsuaCallTransactionRecord &) noexcept {}
    virtual void OnCallMediaUpdateResult(pjsua_call_id, pj_status_t) noexcept {}
};
class PjsuaCallbackRouter final {
public:
    explicit PjsuaCallbackRouter(PjsuaCallbackSink &sink,
                                 const PjsuaApi &api = NativePjsuaApi()) noexcept
        : sink_(sink), api_(api) {}
    explicit PjsuaCallbackRouter(PjsuaCallbackSink &sink,
                                 PjsuaCallCallbackSink &calls,
                                 const PjsuaApi &api = NativePjsuaApi(),
                                 PjsuaMediaManager *media=nullptr) noexcept
        : sink_(sink), calls_(&calls), api_(api), media_(media) {}
    Error Attach(pjsua_callback *) noexcept;
    void BeginQuiescence() noexcept;
    void MarkNativeDestroyed() noexcept;
    void Detach() noexcept;
#if defined(CONFIG_VOIP_PJSUA_PLAN3_COMPONENT_TEST) || defined(CONFIG_VOIP_PJSUA_REGISTRATION_TEST)
    static const PjsuaCallbackRouter *ActiveForTest() noexcept;
    static unsigned AttachAttemptCountForTest() noexcept;
    static unsigned CallbackEntryCountForTest() noexcept;
#endif
private:
    static void OnRegistrationStarted(pjsua_acc_id, pjsua_reg_info *) noexcept;
    static void OnRegistrationState(pjsua_acc_id, pjsua_reg_info *) noexcept;
    static void OnIncomingCall(pjsua_acc_id, pjsua_call_id, pjsip_rx_data *) noexcept;
    static void OnCallState(pjsua_call_id, pjsip_event *) noexcept;
    static void OnCallMediaState(pjsua_call_id) noexcept;
    static void OnCallTransaction(pjsua_call_id, pjsip_transaction *, pjsip_event *) noexcept;
    static void OnCallMediaUpdateResult(pjsua_call_id, pj_status_t) noexcept;
    static void OnConferenceOperation(const pjmedia_conf_op_info *) noexcept;
    void ForwardStarted(pjsua_acc_id, const pjsua_reg_info *) noexcept;
    void ForwardState(pjsua_acc_id, const pjsua_reg_info *) noexcept;
    void ForwardIncoming(pjsua_acc_id, pjsua_call_id) noexcept;
    void ForwardCallState(pjsua_call_id) noexcept;
    void ForwardCallMediaState(pjsua_call_id) noexcept;
    void AssertActor() const noexcept;
    PjsuaCallbackSink &sink_; PjsuaCallCallbackSink *calls_{}; const PjsuaApi &api_;
    PjsuaMediaManager *media_=nullptr;
    bool quiescent_ = false; bool attached_ = false; bool native_destroyed_ = false;
    std::thread::id actor_thread_{};
    static PjsuaCallbackRouter *active_;
#if defined(CONFIG_VOIP_PJSUA_PLAN3_COMPONENT_TEST) || defined(CONFIG_VOIP_PJSUA_REGISTRATION_TEST)
    static unsigned callback_entry_count_;
    static unsigned attach_attempt_count_;
#endif
};
} // namespace voip
#endif
