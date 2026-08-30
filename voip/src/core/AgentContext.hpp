#ifndef VOIP_CORE_AGENT_CONTEXT_HPP
#define VOIP_CORE_AGENT_CONTEXT_HPP

#include <voip/VoipService.hpp>

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace voip {

// This is the registry-owned counterpart of SipAccountConfig.  The public
// configuration contains borrowed pointers; these arrays never borrow them.
struct OwnedSipAccountConfig {
    char identity_uri[max_uri_length + 1]{};
    char registrar_uri[max_uri_length + 1]{};
    char auth_username[max_username_length + 1]{};
    char auth_password[max_password_length + 1]{};

    OwnedSipAccountConfig() noexcept
        : identity_uri{}, registrar_uri{}, auth_username{}, auth_password{} {}

    ~OwnedSipAccountConfig() noexcept { EraseCredentials(); }

    OwnedSipAccountConfig(const OwnedSipAccountConfig &) = delete;
    OwnedSipAccountConfig &operator=(const OwnedSipAccountConfig &) = delete;

    void EraseCredentials() noexcept;
};

struct AgentContext {
    AgentHandle handle{};
    OwnedSipAccountConfig sip{};
    AgentAudioBinding audio{nullptr, nullptr};
    RegistrationState registration = RegistrationState::disabled;
    CallHandle promoted_call{};

    AgentContext() noexcept = default;
    ~AgentContext() noexcept = default;

    AgentContext(const AgentContext &) = delete;
    AgentContext &operator=(const AgentContext &) = delete;
};

static_assert(std::is_nothrow_destructible<AgentContext>::value,
              "agent contexts must be safe for fixed-pool destruction");

} // namespace voip

#endif
