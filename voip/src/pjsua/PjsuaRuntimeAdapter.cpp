#include "PjsuaRuntimeAdapter.hpp"

#if defined(CONFIG_VOIP_PJSUA_REGISTRATION_TEST)
// Owned by applications/voip_integration/test_support.  This test-only
// declaration keeps the native endpoint out of the public service ABI.
extern "C" pj_status_t voip_pjsua_registration_test_install(
    pjsip_endpoint *endpoint) noexcept;
#endif

namespace voip {
PjsuaRuntimeAdapter::PjsuaRuntimeAdapter(const PjsuaApi &api) noexcept
    : api_(api), accounts_(api), router_(accounts_, api), runtime_(api), transport_(api) {}

bool PjsuaRuntimeAdapter::Enqueue(const RuntimeNotification &notification) noexcept {
    if (notification_size_ == notifications_.size()) return false;
    notifications_[(notification_head_ + notification_size_) % notifications_.size()] = notification;
    ++notification_size_;
    return true;
}

void PjsuaRuntimeAdapter::DrainAccountNotifications() noexcept {
    PjsuaRegistrationNotification account_notification{};
    while (notification_size_ != notifications_.size() &&
           accounts_.TryGetNotification(&account_notification)) {
        RuntimeNotification notification{};
        notification.type = RuntimeNotification::Type::registration_state;
        notification.agent = account_notification.agent;
        notification.registration = account_notification.registration;
        notification.status = account_notification.status;
        (void)Enqueue(notification);
    }
}

Error PjsuaRuntimeAdapter::Rollback() noexcept {
    if (runtime_.Created()) {
        const Error registered = runtime_.EnsureActorRegistered();
        // Every following teardown operation enters PJLIB. Do not make a
        // native call while the actor lacks its PJLIB registration.
        if (registered != Error::ok) {
            startup_rollback_pending_ = true;
            return registered;
        }
    }
    (void)accounts_.Shutdown();
    (void)transport_.Shutdown();
    const Error destroyed = runtime_.Destroy();
    if (destroyed != Error::ok) {
        startup_rollback_pending_ = true;
        return destroyed;
    }
    router_.MarkNativeDestroyed();
    router_.Detach();
    initialized_ = false;
    shutdown_started_ = false;
    startup_rollback_pending_ = false;
    return Error::ok;
}

Error PjsuaRuntimeAdapter::Initialize(const AgentRegistry &registry,
                                      const SecurityPolicy &security,
                                      const PcmFormat &format) noexcept {
    if (initialized_ || security.signaling != SignalingSecurity::none ||
        security.media != MediaSecurity::none)
        return security.signaling != SignalingSecurity::none || security.media != MediaSecurity::none
                   ? Error::unsupported_configuration : Error::invalid_state;
    pjsua_callback callbacks{};
    if (router_.Attach(&callbacks) != Error::ok) return Error::invalid_state;
    if (runtime_.CreateAndInitialize(callbacks, format) != Error::ok) { Rollback(); return Error::internal_failure; }
    if (runtime_.EnsureActorRegistered() != Error::ok) { Rollback(); return Error::internal_failure; }
    if (transport_.Initialize(SignalingTransportPolicy::tcp_plain) != Error::ok) { Rollback(); return Error::internal_failure; }
#if defined(CONFIG_VOIP_PJSUA_REGISTRATION_TEST)
    if (voip_pjsua_registration_test_install(pjsua_get_pjsip_endpt()) != PJ_SUCCESS) {
        Rollback(); return Error::internal_failure;
    }
#endif
    if (runtime_.Start() != Error::ok) { Rollback(); return Error::internal_failure; }
    if (runtime_.EnsureActorRegistered() != Error::ok) { Rollback(); return Error::internal_failure; }
    if (accounts_.Initialize(registry, transport_.Id()) != Error::ok) { Rollback(); return Error::internal_failure; }
    initialized_ = true;
    // Per-account request failures are copied as isolated notifications by
    // the account manager; no successfully created account is rolled back.
    if (runtime_.EnsureActorRegistered() != Error::ok) { Rollback(); return Error::internal_failure; }
    (void)accounts_.StartInitialRegistration();
    DrainAccountNotifications();
    return Error::ok;
}

Error PjsuaRuntimeAdapter::Pump(std::uint64_t now_ms, std::uint32_t timeout_ms) noexcept {
    if (!initialized_) return Error::shutting_down;
    // Native callbacks can schedule refresh/retry work.  Give them this
    // turn's actor time before dispatch, then process timers afterwards.
    now_ms_ = now_ms;
    accounts_.SetNow(now_ms);
    const Error result = runtime_.Pump(timeout_ms);
    if (result != Error::ok) return result;
    if (runtime_.EnsureActorRegistered() != Error::ok) return Error::internal_failure;
    (void)accounts_.Pump(now_ms);
    DrainAccountNotifications();
    return Error::ok;
}

bool PjsuaRuntimeAdapter::TryGetNotification(RuntimeNotification *notification) noexcept {
    if (notification == nullptr || notification_size_ == 0) return false;
    *notification = notifications_[notification_head_];
    notification_head_ = (notification_head_ + 1) % notifications_.size();
    --notification_size_;
    return true;
}

Error PjsuaRuntimeAdapter::PromoteOutgoing(AgentHandle, const char *, std::uint32_t *) noexcept { return Error::unsupported_configuration; }
Error PjsuaRuntimeAdapter::PromoteIncoming(AgentHandle, std::uint32_t, std::uint32_t *) noexcept { return Error::unsupported_configuration; }
Error PjsuaRuntimeAdapter::Answer(std::uint32_t) noexcept { return Error::unsupported_configuration; }
Error PjsuaRuntimeAdapter::Reject(std::uint32_t, std::uint16_t) noexcept { return Error::unsupported_configuration; }
Error PjsuaRuntimeAdapter::Cancel(std::uint32_t) noexcept { return Error::unsupported_configuration; }
Error PjsuaRuntimeAdapter::Hangup(std::uint32_t) noexcept { return Error::unsupported_configuration; }
Error PjsuaRuntimeAdapter::SetHeld(std::uint32_t, bool) noexcept { return Error::unsupported_configuration; }
Error PjsuaRuntimeAdapter::BeginCallTeardown(std::uint32_t) noexcept { return Error::unsupported_configuration; }

#if defined(CONFIG_VOIP_PJSUA_PLAN3_COMPONENT_TEST)
void PjsuaRuntimeAdapter::FillNotificationRingForTest() noexcept {
    RuntimeNotification notification{};
    notification.type = RuntimeNotification::Type::registration_state;
    notification.registration = RegistrationState::registering;
    while (Enqueue(notification)) {}
}
#endif

Error PjsuaRuntimeAdapter::Shutdown() noexcept {
    if (!initialized_) return startup_rollback_pending_ ? Rollback() : Error::ok;
    if (!shutdown_started_) {
        router_.BeginQuiescence();
        if (runtime_.EnsureActorRegistered() != Error::ok) return Error::internal_failure;
        (void)accounts_.BeginShutdown();
        DrainAccountNotifications();
        shutdown_started_ = true;
        return Error::busy;
    }
    // Callback-reachable contexts remain intact until every accepted or busy
    // unregister obligation completes. Public wait timeouts are handled by
    // VoipRuntime; elapsed adapter time never authorizes forced teardown.
    if (accounts_.PendingUnregistrations() != 0)
        return Error::busy;
    return Rollback();
}
} // namespace voip
