#ifndef VOIP_PJSUA_CALL_RECORDS_HPP
#define VOIP_PJSUA_CALL_RECORDS_HPP

#include "../core/RuntimeAdapter.hpp"

namespace voip {

// Callback-local PJSUA structures never cross this boundary.  The manager
// places this value record in its fixed ring for the actor to drain.
struct PjsuaCallNotification {
    RuntimeNotification notification{};
};

} // namespace voip

#endif
