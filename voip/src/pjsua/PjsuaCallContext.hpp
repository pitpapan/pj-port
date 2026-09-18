#ifndef VOIP_PJSUA_CALL_CONTEXT_HPP
#define VOIP_PJSUA_CALL_CONTEXT_HPP

#include <pjsua-lib/pjsua.h>
#include <voip/VoipTypes.hpp>

#include <cstdint>
#include <type_traits>

namespace voip {

enum class PjsuaCallPhase : std::uint8_t {
    free, provisional_incoming, queued_incoming, promoting,
    calling, early, confirmed, local_hold, disconnecting, disconnected
};

struct PjsuaCallContext {
    std::uint32_t token = 0;
    CallHandle logical{};
    AgentHandle agent{};
    pjsua_call_id native_id = PJSUA_INVALID_ID;
    PjsuaCallPhase phase = PjsuaCallPhase::free;
    bool incoming = false, logical_bound = false, user_data_bound = false;
    bool media_quiescent = true, terminal_reported = false;
    unsigned callback_depth = 0;
};

static_assert(std::is_standard_layout<PjsuaCallContext>::value &&
              std::is_trivially_copyable<PjsuaCallContext>::value,
              "PJSUA call contexts must remain fixed allocation-free records");

} // namespace voip

#endif
