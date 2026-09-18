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
    bool TryGetNotification(RuntimeNotification *) noexcept;
    void Reset() noexcept;
    std::size_t Count() const noexcept { return count_; }

private:
    PjsuaCallContext *Find(pjsua_call_id) noexcept;
    PjsuaCallContext *Reserve(AgentHandle, pjsua_call_id) noexcept;
    bool OwnsNativeUserData(const PjsuaCallContext &) const noexcept;
    bool EnqueueIncoming(const PjsuaCallContext &, const pjsua_call_info &) noexcept;
    void Release(PjsuaCallContext &) noexcept;
    void Decline(pjsua_call_id, unsigned) noexcept;
    std::uint32_t NextToken() noexcept;

    PjsuaAccountManager &accounts_;
    const PjsuaApi &api_;
    std::array<PjsuaCallContext, max_calls> contexts_{};
    std::array<PjsuaCallNotification, max_calls> notifications_{};
    std::size_t notification_head_{};
    std::size_t notification_size_{};
    std::size_t count_{};
    std::uint32_t next_token_{};
};

static_assert(PjsuaCallManager::max_calls == 7,
              "PJSUA call manager owns exactly seven native contexts");

} // namespace voip

#endif
