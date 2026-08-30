#ifndef VOIP_CORE_CALL_SCHEDULER_HPP
#define VOIP_CORE_CALL_SCHEDULER_HPP

#include "AgentRegistry.hpp"
#include "CallContext.hpp"
#include "../HandlePool.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace voip {

struct ScheduledTransition {
    CallHandle handle{};
    AppliedCallTransition transition{};
    CallSnapshot snapshot{};
    bool handle_invalidated = false;
};

class CallScheduler final {
public:
    static constexpr std::size_t logical_call_capacity = 7;
    static constexpr std::size_t promoted_capacity = 2;
    static constexpr std::size_t fifo_capacity = 5;

    explicit CallScheduler(AgentRegistry &agents) noexcept : agents_(&agents) {}
    CallScheduler() noexcept = default;
    ~CallScheduler() noexcept = default;
    CallScheduler(const CallScheduler &) = delete;
    CallScheduler &operator=(const CallScheduler &) = delete;

    void Bind(AgentRegistry &agents) noexcept { agents_ = &agents; }

    Error AdmitOutgoing(AgentHandle agent, const char *remote_uri,
                        CallHandle *handle, bool *promoted = nullptr) noexcept;
    Error AdmitOutgoing(AgentHandle agent, const DialRequest &request,
                        CallHandle *handle, bool *promoted = nullptr) noexcept {
        return AdmitOutgoing(agent, request.remote_uri, handle, promoted);
    }
    Error AdmitIncoming(AgentHandle agent, std::uint32_t runtime_token,
                        const char *remote_uri, CallHandle *handle,
                        bool *promoted = nullptr) noexcept;
    Error AdmitIncoming(AgentHandle agent, std::uint32_t runtime_token,
                        CallHandle *handle, bool *promoted = nullptr) noexcept {
        return AdmitIncoming(agent, runtime_token, "", handle, promoted);
    }

    Error Answer(CallHandle handle, ScheduledTransition *result = nullptr) noexcept;
    Error OnAcceptance(CallHandle handle,
                       ScheduledTransition *result = nullptr) noexcept;
    Error Reject(CallHandle handle,
                 ScheduledTransition *result = nullptr) noexcept;
    Error Cancel(CallHandle handle,
                 ScheduledTransition *result = nullptr) noexcept;
    Error Hangup(CallHandle handle,
                 ScheduledTransition *result = nullptr) noexcept;
    Error OnTimeout(CallHandle handle,
                    ScheduledTransition *result = nullptr) noexcept;
    Error SetHeld(CallHandle handle, bool held,
                  ScheduledTransition *result = nullptr) noexcept;
    Error OnTeardownComplete(CallHandle handle,
                             ScheduledTransition *result = nullptr) noexcept;

    Error Accept(CallHandle handle,
                 ScheduledTransition *result = nullptr) noexcept {
        return OnAcceptance(handle, result);
    }
    Error Finish(CallHandle handle,
                 ScheduledTransition *result = nullptr) noexcept {
        return Hangup(handle, result);
    }
    Error Timeout(CallHandle handle,
                  ScheduledTransition *result = nullptr) noexcept {
        return OnTimeout(handle, result);
    }
    Error TeardownComplete(CallHandle handle,
                           ScheduledTransition *result = nullptr) noexcept {
        return OnTeardownComplete(handle, result);
    }

    // Looks at only the FIFO head. It can promote repeatedly until no head is
    // eligible, the queue is empty, or both promoted slots are occupied.
    bool OnCapacityChanged(ScheduledTransition *result = nullptr) noexcept;

    bool IsLive(CallHandle handle) const noexcept;
    bool IsPromoted(CallHandle handle) const noexcept;
    std::size_t PromotedCount() const noexcept { return promoted_count_; }
    std::size_t QueuedCount() const noexcept { return fifo_count_; }
    std::size_t LiveCount() const noexcept { return live_count_; }
    std::size_t AvailableLogicalCalls() const noexcept {
        return logical_call_capacity - live_count_;
    }
    std::size_t AvailablePromotedCalls() const noexcept {
        return promoted_capacity - promoted_count_;
    }
    std::size_t AvailableFifoEntries() const noexcept {
        return fifo_capacity - fifo_count_;
    }
    CallSnapshot Snapshot(CallHandle handle) const noexcept;
    Error GetSnapshot(CallHandle handle, CallSnapshot *snapshot) const noexcept;
    bool QueuePosition(CallHandle handle, std::size_t *position) const noexcept;
    const CallContext *Resolve(CallHandle handle) const noexcept {
        return calls_.Resolve(handle);
    }

private:
    using Pool = HandlePool<CallHandle, logical_call_capacity, CallContext>;

    static bool CopyUri(char (&destination)[max_uri_length + 1],
                        const char *source) noexcept;
    bool CanPromote(const AgentHandle &agent) const noexcept;
    bool PromoteContext(CallContext &context,
                        ScheduledTransition *result) noexcept;
    bool PromoteHead(ScheduledTransition *result) noexcept;
    Error Admit(AgentHandle agent, CallDirection direction,
                std::uint32_t runtime_token, const char *remote_uri,
                CallHandle *handle, bool *promoted) noexcept;
    Error Apply(CallContext &context, CallTransition cause,
                ScheduledTransition *result) noexcept;
    void FillTransition(const CallContext &context,
                        const AppliedCallTransition &transition,
                        ScheduledTransition *result) const noexcept;
    bool RemoveFromFifo(CallHandle handle) noexcept;
    bool ReleaseContext(CallContext &context,
                        ScheduledTransition *result) noexcept;
    void SetPhaseAfterAcceptance(CallContext &context) noexcept;
    void ClearAgentLease(const CallContext &context) noexcept;
    bool IsQueued(const CallContext &context) const noexcept;

    AgentRegistry *agents_ = nullptr;
    Pool calls_{};
    std::array<CallHandle, fifo_capacity> fifo_{};
    std::size_t fifo_count_ = 0;
    std::size_t promoted_count_ = 0;
    std::size_t live_count_ = 0;
};

static_assert(CallScheduler::logical_call_capacity == 7,
              "production logical call capacity must be seven");
static_assert(CallScheduler::promoted_capacity == 2,
              "production promoted capacity must be two");
static_assert(CallScheduler::fifo_capacity == 5,
              "production FIFO capacity must be five");

} // namespace voip

#endif
