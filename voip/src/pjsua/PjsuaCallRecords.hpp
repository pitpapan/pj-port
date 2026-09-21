#ifndef VOIP_PJSUA_CALL_RECORDS_HPP
#define VOIP_PJSUA_CALL_RECORDS_HPP

#include "../core/RuntimeAdapter.hpp"

namespace voip {

// Callback-local PJSUA structures never cross this boundary.  The manager
// places this value record in its fixed ring for the actor to drain.
struct PjsuaCallNotification {
    RuntimeNotification notification{};
};

// A compact callback-reachable overflow record. It deliberately contains no
// PJ pointers/views and is large enough to retain every semantic field core
// consumes while the public ring is saturated.
struct PjsuaDeferredCallNotification {
    RuntimeNotification::Type type = RuntimeNotification::Type::call_finished;
    AgentHandle agent{};
    std::uint32_t token = 0;
    std::uint16_t sip_status = 0;
    Error error = Error::ok;
};

} // namespace voip

#endif
