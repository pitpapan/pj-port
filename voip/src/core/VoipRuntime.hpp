#ifndef VOIP_CORE_VOIP_RUNTIME_HPP
#define VOIP_CORE_VOIP_RUNTIME_HPP

#include "AgentRegistry.hpp"
#include "CommandMailbox.hpp"
#include "CoreActor.hpp"
#include "FakeRuntimeAdapter.hpp"
#include "OperationTable.hpp"
#include "VoipResourceGuard.hpp"
#include "VoipEventQueue.hpp"
#include "CallScheduler.hpp"

#include <array>
#include <cstdint>

namespace voip {

class VoipRuntime final : public CommandHandleValidator {
public:
    VoipRuntime() noexcept;
    ~VoipRuntime() noexcept;
    VoipRuntime(const VoipRuntime &) = delete;
    VoipRuntime &operator=(const VoipRuntime &) = delete;

    Error Initialize(const ServiceConfig &) noexcept;
    Error Shutdown() noexcept;
    Error GetAgentHandle(std::uint8_t, AgentHandle *) const noexcept;
    Error Dial(AgentHandle, const DialRequest &, CallHandle *, OperationId *) noexcept;
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
    // Deterministic host/fake-adapter seam for copied native notifications.
    Error InjectNotification(const RuntimeNotification &) noexcept;
    bool Validate(AgentHandle) const noexcept override;
    bool Validate(CallHandle) const noexcept override;

private:
    Error ReserveOperation(OperationId *) noexcept;
    void PublishTransition(const ScheduledTransition &) noexcept;
    void PublishAdmission(CallHandle, bool waiting) noexcept;
    void PublishAgent(const AgentContext &) noexcept;
    void ApplyEffects(const SchedulerEffects &) noexcept;
    void ProcessNotification(const RuntimeNotification &) noexcept;
    CallContext *FindCall(std::uint32_t token) noexcept;
    const CallContext *FindCall(CallHandle) const noexcept;
    void ForgetCall(CallHandle) noexcept;
    void RefreshResources() noexcept;
    void CompleteCallOperation(const CallContext &, Error) noexcept;
    void BindCallOperation(CallContext &, OperationId) noexcept;
    void CompleteCallOperations(CallContext &, Error) noexcept;
    void CompleteOperationIds(OperationId signaling, OperationId current,
                              Error) noexcept;
    void CancelAllCalls() noexcept;
    void ApplyTimers(std::uint64_t now_ms) noexcept;

    mutable CoreMutex mutex_{};
    AgentRegistry agents_{};
    CommandMailbox mailbox_{};
    OperationTable operations_{};
    VoipEventQueue events_{};
    CallScheduler scheduler_;
    FakeRuntimeAdapter adapter_{};
    VoipResourceGuard resources_{};
    CoreActor actor_{};
    std::array<CallHandle, CallScheduler::logical_call_capacity> calls_{};
    std::size_t call_count_ = 0;
    bool initialized_ = false;
    bool shutting_down_ = false;
    bool stopped_ = false;
    std::uint64_t now_ms_ = 0;
    std::uint32_t queue_timeout_ms_ = 0;
    std::uint32_t answer_timeout_ms_ = 0;
};

} // namespace voip

#endif
