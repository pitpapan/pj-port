#ifndef VOIP_PJSUA_ACCOUNT_MANAGER_HPP
#define VOIP_PJSUA_ACCOUNT_MANAGER_HPP

#include "PjsuaAccountContext.hpp"
#include "PjsuaApi.hpp"
#include "../core/AgentRegistry.hpp"

#include <array>
#include <cstddef>
#include <thread>

namespace voip {

class PjsuaAccountManager final {
public:
    static constexpr std::size_t max_accounts = 5;

    explicit PjsuaAccountManager(const PjsuaApi &api = NativePjsuaApi()) noexcept
        : api_(api) {}

    Error Initialize(const AgentRegistry &, pjsua_transport_id) noexcept;
    Error StartInitialRegistration() noexcept;
    Error Shutdown() noexcept;
    std::size_t Count() const noexcept;
    PjsuaAccountContext *Resolve(pjsua_acc_id) noexcept;
    const PjsuaAccountContext *Resolve(pjsua_acc_id) const noexcept;
    Error NativeId(AgentHandle, pjsua_acc_id *) const noexcept;

private:
    struct NativeLookup { pjsua_acc_id id{PJSUA_INVALID_ID}; PjsuaAccountContext *context{}; };
    static_assert(max_accounts == AgentRegistry::max_agents,
                  "PJSUA account capacity must match the fixed registry");

    const PjsuaApi &api_;
    std::array<PjsuaAccountContext, max_accounts> contexts_{};
    std::array<NativeLookup, max_accounts> lookup_{};
    std::size_t count_{};
    std::thread::id actor_thread_{};

    void AssertActor() const noexcept;
    void Clear() noexcept;
    void Rollback() noexcept;
    bool InsertLookup(pjsua_acc_id, PjsuaAccountContext *) noexcept;
    static pj_str_t NativeString(char *) noexcept;
};

static_assert(PjsuaAccountManager::max_accounts == 5,
              "PjsuaAccountManager owns exactly five contexts");

} // namespace voip

#endif
