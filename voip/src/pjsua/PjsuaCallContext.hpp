#ifndef VOIP_PJSUA_CALL_CONTEXT_HPP
#define VOIP_PJSUA_CALL_CONTEXT_HPP

#include <pjsua-lib/pjsua.h>
#include "../core/RuntimeAdapter.hpp"
#include "PjsuaCallRecords.hpp"
#include <voip/VoipTypes.hpp>

#include <cstdint>
#include <type_traits>

namespace voip {

enum class PjsuaCallPhase : std::uint8_t {
    free, provisional_incoming, queued_incoming, promoting,
    admitted_incoming, calling, early, confirmed, local_hold, disconnecting, disconnected
};

struct PjsuaCallContext {
    std::uint32_t token = 0;
    CallHandle logical{};
    AgentHandle agent{};
    pjsua_call_id native_id = PJSUA_INVALID_ID;
    PjsuaCallPhase phase = PjsuaCallPhase::free;
    bool incoming = false, logical_bound = false, user_data_bound = false;
    bool promoted = false;
    CopiedMediaState media{};
    Error media_failure=Error::ok;
    bool media_hangup_sent=false;
    bool media_quiescent = true, terminal_reported = false;
    bool confirmed_once = false, terminal_seen = false;
    bool future_security_clear = true;
    enum class MediaControl : std::uint8_t { none, hold, resume, result };
    MediaControl media_control = MediaControl::none;
    pj_int32_t last_local_invite_cseq = 0;
    pj_int32_t control_cseq = 0;
    // A context owns four independent obligations, not a best-effort ring:
    // admission; confirmation OR rejection; one pending media result; finish.
    // Confirmation and rejection are mutually exclusive. A new media control
    // is refused until its previous result has been consumed. Context reuse
    // waits for finish consumption. Thus no callback sequence can overwrite
    // a required record, even when the consumer stops indefinitely.
    PjsuaCallNotification admission{}, outcome{}, media_result{}, finish{};
    unsigned callback_depth = 0;
};

static_assert(std::is_standard_layout<PjsuaCallContext>::value &&
              std::is_trivially_copyable<PjsuaCallContext>::value,
              "PJSUA call contexts must remain fixed allocation-free records");

} // namespace voip

#endif
