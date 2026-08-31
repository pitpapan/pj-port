#ifndef VOIP_PJSUA_ACCOUNT_MANAGER_HPP
#define VOIP_PJSUA_ACCOUNT_MANAGER_HPP

#include "PjsuaAccountContext.hpp"
#include "PjsuaApi.hpp"
#include "PjsuaCallbackRouter.hpp"
#include "../core/AgentRegistry.hpp"

#include <array>
#include <cstddef>
#include <thread>

namespace voip {

struct PjsuaRegistrationNotification {
    AgentHandle agent{};
    RegistrationState registration{RegistrationState::disabled};
    Status status{};
};

class PjsuaAccountManager final : public PjsuaCallbackSink {
public:
    static constexpr std::size_t max_accounts = 5;

    explicit PjsuaAccountManager(const PjsuaApi &api = NativePjsuaApi()) noexcept
        : api_(api) {}

    Error Initialize(const AgentRegistry &, pjsua_transport_id) noexcept;
    Error StartInitialRegistration() noexcept;
    Error Pump(std::uint64_t now_ms) noexcept;
    Error Shutdown() noexcept;
    std::size_t Count() const noexcept;
    PjsuaAccountContext *Resolve(pjsua_acc_id) noexcept;
    const PjsuaAccountContext *Resolve(pjsua_acc_id) const noexcept;
    Error NativeId(AgentHandle, pjsua_acc_id *) const noexcept;
    bool TryGetNotification(PjsuaRegistrationNotification *) noexcept;
    void OnRegistrationStarted(const PjsuaRegistrationRecord &) noexcept override;
    void OnRegistrationState(const PjsuaRegistrationRecord &) noexcept override;

private:
    struct NativeLookup { pjsua_acc_id id{PJSUA_INVALID_ID}; PjsuaAccountContext *context{}; };
    static_assert(max_accounts == AgentRegistry::max_agents,
                  "PJSUA account capacity must match the fixed registry");

    const PjsuaApi &api_;
    std::array<PjsuaAccountContext, max_accounts> contexts_{};
    std::array<NativeLookup, max_accounts> lookup_{};
    static constexpr std::size_t notification_capacity = 32;
    std::array<PjsuaRegistrationNotification, notification_capacity> notifications_{};
    std::size_t notification_head_{};
    std::size_t notification_size_{};
    std::size_t count_{};
    std::uint64_t now_ms_{};
    std::thread::id actor_thread_{};

    void AssertActor() const noexcept;
    void Clear() noexcept;
    void Rollback() noexcept;
    static bool IsNativeIdInDomain(pjsua_acc_id) noexcept;
    NativeLookup *FindLookup(pjsua_acc_id) noexcept;
    const NativeLookup *FindLookup(pjsua_acc_id) const noexcept;
    PjsuaAccountContext *OwnedContext(void *) noexcept;
    const PjsuaAccountContext *OwnedContext(const void *) const noexcept;
    bool InsertLookup(pjsua_acc_id, PjsuaAccountContext *) noexcept;
    void Notify(const PjsuaAccountContext &, RegistrationState, Error,
                std::uint16_t, const char *) noexcept;
    void RemoveNotification(std::size_t) noexcept;
    void RemoveRetryWait(AgentHandle) noexcept;
    static bool IsFailure(RegistrationState) noexcept;
    static bool IsGuaranteed(RegistrationState) noexcept;
    void ScheduleRetry(PjsuaAccountContext &, const PjsuaRegistrationRecord &) noexcept;
    static bool Recoverable(const PjsuaRegistrationRecord &) noexcept;
    static std::uint64_t AddSaturated(std::uint64_t, std::uint64_t) noexcept;
    static pj_str_t NativeString(char *) noexcept;
};

static_assert(PjsuaAccountManager::max_accounts == 5,
              "PjsuaAccountManager owns exactly five contexts");

} // namespace voip

#endif
