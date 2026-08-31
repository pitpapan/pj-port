#ifndef VOIP_PJSUA_API_HPP
#define VOIP_PJSUA_API_HPP

#include <pjsua-lib/pjsua.h>
#include <pj_zephyr_pool_arena.h>
#include <voip/VoipTypes.hpp>

namespace voip {

struct PjsuaApi {
    pj_status_t (*arena_install)() = nullptr;
    pj_status_t (*arena_reset)() = nullptr;
    pj_status_t (*create)() = nullptr;
    void (*config_default)(pjsua_config *) = nullptr;
    void (*logging_config_default)(pjsua_logging_config *) = nullptr;
    void (*media_config_default)(pjsua_media_config *) = nullptr;
    void (*transport_config_default)(pjsua_transport_config *) = nullptr;
    pj_status_t (*init)(const pjsua_config *, const pjsua_logging_config *,
                        const pjsua_media_config *) = nullptr;
    pjmedia_port *(*set_no_sound)() = nullptr;
    pj_status_t (*transport_create)(pjsip_transport_type_e,
                                    const pjsua_transport_config *,
                                    pjsua_transport_id *) = nullptr;
    pj_status_t (*transport_close)(pjsua_transport_id, pj_bool_t) = nullptr;
    pj_status_t (*start)() = nullptr;
    int (*handle_events)(unsigned) = nullptr;
    pj_status_t (*destroy)() = nullptr;
    void (*acc_config_default)(pjsua_acc_config *) = nullptr;
    pj_status_t (*acc_add)(const pjsua_acc_config *, pj_bool_t,
                           pjsua_acc_id *) = nullptr;
    pj_status_t (*acc_set_user_data)(pjsua_acc_id, void *) = nullptr;
    pj_status_t (*acc_del)(pjsua_acc_id) = nullptr;
    pj_status_t (*acc_set_registration)(pjsua_acc_id, pj_bool_t) = nullptr;
    pj_status_t (*call_answer)(pjsua_call_id, unsigned,
                               const pj_str_t *, const pjsua_msg_data *) = nullptr;
    pj_status_t (*call_hangup)(pjsua_call_id, unsigned,
                               const pj_str_t *, const pjsua_msg_data *) = nullptr;
};

const PjsuaApi &NativePjsuaApi() noexcept;
Error PjsuaStatus(pj_status_t status) noexcept;

} // namespace voip

#endif
