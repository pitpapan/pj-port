#ifndef VOIP_PJSUA_ACCOUNT_CONTEXT_HPP
#define VOIP_PJSUA_ACCOUNT_CONTEXT_HPP

#include <pjsua-lib/pjsua.h>
#include <voip/VoipTypes.hpp>

#include <cstdint>
#include <type_traits>

namespace voip {

struct RetryState {
    std::uint8_t attempt{};
    std::uint64_t due_ms{};
    bool scheduled{};
};

struct PjsuaAccountContext {
    pjsua_acc_id account_id{PJSUA_INVALID_ID};
    AgentHandle agent{};
    RegistrationState registration{RegistrationState::disabled};
    RetryState retry{};
    std::uint64_t refresh_due_ms{};
    bool register_on_start{};
    bool occupied{};
};

static_assert(std::is_standard_layout<PjsuaAccountContext>::value,
              "account context must remain a simple fixed record");
static_assert(sizeof(PjsuaAccountContext) < 128,
              "account context must not retain credentials or audio bindings");

} // namespace voip

#endif
