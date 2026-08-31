#ifndef VOIP_CORE_VOIP_RUNTIME_HPP
#define VOIP_CORE_VOIP_RUNTIME_HPP

#include "AgentRegistry.hpp"
#include "CommandMailbox.hpp"
#include "CoreActor.hpp"
#include "RuntimeAdapter.hpp"
#if defined(CONFIG_VOIP_PJSUA)
#include "../pjsua/PjsuaRuntimeAdapter.hpp"
#elif !defined(__ZEPHYR__) || defined(CONFIG_VOIP_SERVICE_FAKE_ADAPTER)
#include "FakeRuntimeAdapter.hpp"
#endif
#include "OperationTable.hpp"
#include "VoipResourceGuard.hpp"
#include "VoipEventQueue.hpp"
#include "CallScheduler.hpp"

#include <array>
#include <cstdint>

namespace voip {

#if defined(CONFIG_VOIP_PJSUA)
using SelectedRuntimeAdapter = PjsuaRuntimeAdapter;
#elif !defined(__ZEPHYR__) || defined(CONFIG_VOIP_SERVICE_FAKE_ADAPTER)
using SelectedRuntimeAdapter = FakeRuntimeAdapter;
#else
using SelectedRuntimeAdapter = NullRuntimeAdapter;
#endif

class VoipRuntime final : public CommandHandleValidator {
public:
    VoipRuntime() noexcept;
    ~VoipRuntime() noexcept;
    VoipRuntime(const VoipRuntime &) = delete;
    VoipRuntime &operator=(const VoipRuntime &) = delete;

    Error Initialize(const ServiceConfig &) noexcept;
    Error Shutdown() noexcept;
    Error GetAgentHandle(std::uint8_t, AgentHandle *) const noexcept;
    Error Dial(AgentHandle, const DialRequest &, OperationId *) noexcept;
    Error Answer(CallHandle, OperationId *) noexcept;
    Error Reject(CallHandle, std::uint16_t, OperationId *) noexcept;
    Error Cancel(CallHandle, OperationId *) noexcept;
    Error Hangup(CallHandle, OperationId *) noexcept;
    Error SetHeld(CallHandle, bool, OperationId *) noexcept;
    Error TryGetEvent(Event *event) noexcept { return events_.TryPop(event) ? Error::ok : Error::timed_out; }
    Error WaitForEvent(Event *, std::uint32_t) noexcept;
    Error GetAgentSnapshot(AgentHandle, AgentSnapshot *) const noexcept;
    Error GetCallSnapshot(CallHandle, CallSnapshot *) const noexcept;
    ResourceSnapshot GetResourceSnapshot() const noexcept;

    void Step(std::uint64_t now_ms) noexcept;
    // Deterministic host seam; the public VoipService API exposes no actor
    // controls.
    void SetActorPaused(bool paused) noexcept;
    // Deterministic host/fake-adapter seam for copied native notifications.
    Error InjectNotification(const RuntimeNotification &) noexcept;
    void FailNextAdapter(RuntimeRequest::Type, Error) noexcept;
    void SetAdapterCallbacksDeferred(bool) noexcept;
    void DrainAdapterCallbacks() noexcept;
    void SetAdapterInitializationNotificationBurst(std::size_t) noexcept;
    std::size_t PendingAdapterNotifications() const noexcept;
    std::size_t AdapterRequestCount() const noexcept;
    bool GetAdapterRequest(std::size_t, RuntimeRequest *) const noexcept;
    bool Validate(AgentHandle) const noexcept override;
    bool Validate(CallHandle) const noexcept override;

private:
    enum class Lifecycle : std::uint8_t { idle, starting, running, shutting_down, stopped };

    void CompleteStartup(Error) noexcept;
    void RollbackStartup(Error) noexcept;
    Error ReserveOperation(OperationId *) noexcept;
    Error EnqueueControl(VoipCommand &, OperationId *) noexcept;
    void PublishTransition(const ScheduledTransition &) noexcept;
    bool PublishEvent(const Event &) noexcept;
    bool ReserveCallTerminal(VoipEventQueue::Reservation *, std::size_t *) noexcept;
    bool CommitCallTerminal(CallHandle, const Event &) noexcept;
    void PublishAdmission(CallHandle, bool waiting) noexcept;
    void PublishAgent(const AgentContext &) noexcept;
    void ApplyEffects(const SchedulerEffects &) noexcept;
    void ProcessNotification(const RuntimeNotification &) noexcept;
    void ProcessCommand(const VoipCommand &) noexcept;
    CallContext *FindCall(std::uint32_t token) noexcept;
    const CallContext *FindCall(CallHandle) const noexcept;
    void ForgetCall(CallHandle) noexcept;
    void RefreshResources() noexcept;
    void CompleteCallOperation(const CallContext &, Error) noexcept;
    void BindCallOperation(CallContext &, OperationId) noexcept;
    void CompleteCallOperations(CallContext &, Error) noexcept;
    void CompleteOperationIds(OperationId signaling, OperationId current,
                              Error) noexcept;
    void FinalizeTerminal(CallHandle, SchedulerEffects &) noexcept;
    void CancelAllCalls() noexcept;
    void ApplyTimers(std::uint64_t now_ms) noexcept;
    void RetryPendingTeardowns() noexcept;
    void CompleteShutdownIfDrained() noexcept;

    // Serializes the public shutdown wait and actor join. The actor only
    // takes mutex_, so holding this coordination lock cannot invert locks.
    mutable CoreMutex shutdown_mutex_{};
    mutable CoreMutex mutex_{};
    AgentRegistry agents_{};
    CommandMailbox mailbox_{};
    OperationTable operations_{};
    VoipEventQueue events_{};
    CallScheduler scheduler_;
    SelectedRuntimeAdapter adapter_{};
    VoipResourceGuard resources_{};
    CoreActor actor_{};
    std::array<CallHandle, CallScheduler::logical_call_capacity> calls_{};
    std::array<VoipEventQueue::Reservation,
               CallScheduler::logical_call_capacity> call_terminals_{};
    std::array<bool, CallScheduler::logical_call_capacity> call_terminal_live_{};
    std::array<CallHandle, CallScheduler::logical_call_capacity>
        call_terminal_handles_{};
    std::size_t call_count_ = 0;
    Lifecycle lifecycle_ = Lifecycle::idle;
    bool actor_stopped_ = true;
    std::uint64_t now_ms_ = 0;
    std::uint32_t queue_timeout_ms_ = 0;
    std::uint32_t answer_timeout_ms_ = 0;
    SecurityPolicy security_{};
    PcmFormat conference_format_{};
    CoreEventSignal shutdown_signal_{};
    CoreEventSignal startup_signal_{};
    Error startup_error_ = Error::ok;
    bool shutdown_complete_ = false;
    Error shutdown_error_ = Error::ok;
    bool event_publication_failed_ = false;
};

} // namespace voip

#endif
