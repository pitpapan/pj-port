#ifndef VOIP_PJSUA_CALL_RECORDS_HPP
#define VOIP_PJSUA_CALL_RECORDS_HPP

#include "../core/RuntimeAdapter.hpp"

namespace voip {

// Ordered callback-owned value record; zero sequence means not publishable.
struct PjsuaCallNotification {
    RuntimeNotification notification{};
    std::uint64_t sequence = 0;
};

} // namespace voip

#endif
