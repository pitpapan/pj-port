#include "PjsuaRuntimeAdapter.hpp"

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

void PjsuaRuntimeAdapter::Rollback() noexcept {
    (void)accounts_.Shutdown();
    (void)transport_.Shutdown();
    (void)runtime_.Destroy();
    router_.MarkNativeDestroyed();
    router_.Detach();
    initialized_ = false;
    shutdown_started_ = false;
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
    if (transport_.Initialize(SignalingTransportPolicy::tcp_plain) != Error::ok) { Rollback(); return Error::internal_failure; }
    if (runtime_.Start() != Error::ok) { Rollback(); return Error::internal_failure; }
    if (accounts_.Initialize(registry, transport_.Id()) != Error::ok) { Rollback(); return Error::internal_failure; }
    initialized_ = true;
    // Per-account request failures are copied as isolated notifications by
    // the account manager; no successfully created account is rolled back.
    (void)accounts_.StartInitialRegistration();
    DrainAccountNotifications();
    return Error::ok;
}

Error PjsuaRuntimeAdapter::Pump(std::uint64_t now_ms, std::uint32_t timeout_ms) noexcept {
    if (!initialized_) return Error::shutting_down;
    const Error result = runtime_.Pump(timeout_ms);
    if (result != Error::ok) return result;
    now_ms_ = now_ms;
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

Error PjsuaRuntimeAdapter::Shutdown() noexcept {
    if (!initialized_) return Error::ok;
    if (!shutdown_started_) {
        router_.BeginQuiescence();
        (void)accounts_.BeginShutdown();
        DrainAccountNotifications();
        shutdown_started_ = true;
        return Error::busy;
    }
    // Callback-reachable contexts remain intact until every successfully
    // requested unregister completes.  Public wait timeouts are handled by
    // VoipRuntime; elapsed adapter time never authorizes forced teardown.
    if (accounts_.PendingUnregistrations() != 0)
        return Error::busy;
    Rollback();
    return Error::ok;
}
} // namespace voip
