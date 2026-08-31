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

// C++17 cannot reflect on absent members. These exact member-type and layout
// checks make the allowed fixed record explicit: adding audio pointers,
// OwnedSipAccountConfig, or copied credential arrays changes this contract.
struct PjsuaAccountContextLayoutContract {
    pjsua_acc_id account_id;
    AgentHandle agent;
    RegistrationState registration;
    RetryState retry;
    std::uint64_t refresh_due_ms;
    bool register_on_start;
    bool occupied;
};

static_assert(std::is_standard_layout<PjsuaAccountContext>::value,
              "account context must remain a simple fixed record");
static_assert(std::is_trivially_copyable<PjsuaAccountContext>::value,
              "account context must remain an allocation-free value record");
static_assert(std::is_same<decltype(PjsuaAccountContext::account_id), pjsua_acc_id>::value &&
              std::is_same<decltype(PjsuaAccountContext::agent), AgentHandle>::value &&
              std::is_same<decltype(PjsuaAccountContext::registration), RegistrationState>::value &&
              std::is_same<decltype(PjsuaAccountContext::retry), RetryState>::value &&
              std::is_same<decltype(PjsuaAccountContext::refresh_due_ms), std::uint64_t>::value &&
              std::is_same<decltype(PjsuaAccountContext::register_on_start), bool>::value &&
              std::is_same<decltype(PjsuaAccountContext::occupied), bool>::value,
              "account context may contain only its declared PJ registration state");
static_assert(sizeof(PjsuaAccountContext) == sizeof(PjsuaAccountContextLayoutContract) &&
              alignof(PjsuaAccountContext) == alignof(PjsuaAccountContextLayoutContract),
              "account context must not acquire audio or credential storage");

} // namespace voip

#endif
