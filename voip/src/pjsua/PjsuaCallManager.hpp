#ifndef VOIP_PJSUA_CALL_MANAGER_HPP
#define VOIP_PJSUA_CALL_MANAGER_HPP

#include "PjsuaAccountManager.hpp"
#include "PjsuaCallContext.hpp"
#include "PjsuaCallRecords.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace voip {

class PjsuaCallManager final : public PjsuaCallCallbackSink {
public:
    static constexpr std::size_t max_calls = 7;

    explicit PjsuaCallManager(PjsuaAccountManager &accounts,
                              const PjsuaApi &api = NativePjsuaApi()) noexcept
        : accounts_(accounts), api_(api) {}

    void OnIncomingCall(pjsua_acc_id, pjsua_call_id) noexcept override;
    void OnCallState(pjsua_call_id) noexcept override;
    void OnCallMediaState(pjsua_call_id) noexcept override;
    Error AdmitIncoming(CallHandle, AgentHandle, std::uint32_t) noexcept;
    Error PromoteOutgoing(CallHandle, AgentHandle, const char *,
                          std::uint32_t *) noexcept;
    Error PromoteIncoming(CallHandle, AgentHandle, std::uint32_t) noexcept;
    Error Answer(std::uint32_t) noexcept;
    Error Reject(std::uint32_t, std::uint16_t) noexcept;
    Error Cancel(std::uint32_t) noexcept;
    Error Hangup(std::uint32_t) noexcept;
    Error SetHeld(std::uint32_t, bool) noexcept;
    Error RejectProvisional(std::uint32_t, std::uint16_t) noexcept;
    bool TryGetNotification(RuntimeNotification *) noexcept;
    Error BeginShutdown() noexcept;
    void Reset() noexcept;
    std::size_t Count() const noexcept { return count_; }

private:
    PjsuaCallContext *Find(pjsua_call_id) noexcept;
    PjsuaCallContext *FindToken(std::uint32_t) noexcept;
    PjsuaCallContext *Reserve(AgentHandle, pjsua_call_id) noexcept;
    PjsuaCallContext *ReserveOutgoing(CallHandle, AgentHandle) noexcept;
    bool OwnsNativeUserData(const PjsuaCallContext &) const noexcept;
    bool EnqueueIncoming(const PjsuaCallContext &, const pjsua_call_info &) noexcept;
    bool EnqueueCall(PjsuaCallContext &, RuntimeNotification::Type,
                     const pjsua_call_info &, Error = Error::ok) noexcept;
    RuntimeNotification CopyCall(const PjsuaCallContext &, RuntimeNotification::Type,
                                 const pjsua_call_info &, Error) const noexcept;
    bool EnqueueDeferred(const PjsuaCallContext &, RuntimeNotification::Type,
                         const pjsua_call_info &, Error) noexcept;
    void FlushDeferred() noexcept;
    void FinishCallback(PjsuaCallContext &) noexcept;
    void TryRelease(PjsuaCallContext &) noexcept;
    void Release(PjsuaCallContext &) noexcept;
    void Decline(pjsua_call_id, unsigned) noexcept;
    Error Native(pjsua_call_id, pj_status_t) const noexcept;
    std::uint32_t NextToken() noexcept;

    PjsuaAccountManager &accounts_;
    const PjsuaApi &api_;
    std::array<PjsuaCallContext, max_calls> contexts_{};
    std::array<PjsuaCallNotification, RuntimeAdapter::notification_capacity> notifications_{};
    std::array<PjsuaDeferredCallNotification, max_calls * 4> deferred_{};
    std::size_t notification_head_{};
    std::size_t notification_size_{};
    std::size_t deferred_head_{};
    std::size_t deferred_size_{};
    std::size_t count_{};
    std::uint32_t next_token_{};
};

static_assert(PjsuaCallManager::max_calls == 7,
              "PJSUA call manager owns exactly seven native contexts");
static_assert(RuntimeAdapter::notification_capacity >= PjsuaCallManager::max_calls * 4,
              "public call ring must retain accepted/held/resumed/finished for every context");

} // namespace voip

#endif
