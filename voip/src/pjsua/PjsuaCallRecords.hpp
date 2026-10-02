#ifndef VOIP_PJSUA_CALL_RECORDS_HPP
#define VOIP_PJSUA_CALL_RECORDS_HPP

#include "../core/RuntimeAdapter.hpp"
#include <pjsua-lib/pjsua.h>

namespace voip {

struct CopiedMediaState {
    pjsua_call_id native_id=PJSUA_INVALID_ID;
    pjmedia_dir direction=PJMEDIA_DIR_NONE;
    pjsua_call_media_status status=PJSUA_CALL_MEDIA_NONE;
    pjsua_conf_port_id slot=PJSUA_INVALID_ID;
};
struct PjsuaOwnedMediaObservation {
    CallHandle call{};
    AgentHandle agent{};
    std::uint32_t token=0;
    bool promoted=false, terminal=false, native_inactive=true;
    CopiedMediaState media{};
};

struct PjsuaCallTransactionRecord {
    pjsua_call_id native_id = PJSUA_INVALID_ID;
    pjsip_method_e method = PJSIP_OTHER_METHOD;
    pjsip_role_e role = PJSIP_ROLE_UAS;
    pj_int32_t cseq = 0;
    pjsip_tsx_state_e state = PJSIP_TSX_STATE_NULL;
    int sip_status = 0;
    pj_status_t transport_result = PJ_SUCCESS;
    bool timeout = false, auth_retry = false;
    char reason[max_reason_length + 1]{};
};

// Ordered callback-owned value record; zero sequence means not publishable.
struct PjsuaCallNotification {
    RuntimeNotification notification{};
    std::uint64_t sequence = 0;
};

} // namespace voip

#endif
