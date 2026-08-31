#include "PjsuaApi.hpp"

namespace voip {
static_assert(PJSUA_MAX_ACC == 5, "PJSUA account capacity must remain five");
static_assert(PJSUA_MAX_CALLS == 7, "PJSUA call capacity must remain seven");
static_assert(PJSUA_MAX_CONF_PORTS == 12, "PJSUA conference capacity must remain twelve");
static_assert(PJSIP_HAS_TLS_TRANSPORT == 0, "TLS is outside the initial profile");
static_assert(PJMEDIA_HAS_SRTP == 0, "SRTP is outside the initial profile");
#if defined(CONFIG_VOIP_PJSUA_PLAN3_COMPONENT_TEST)
namespace { const PjsuaApi *component_test_api = nullptr; }
void SetNativePjsuaApiForComponentTest(const PjsuaApi *api) noexcept {
    component_test_api = api;
}
#endif

const PjsuaApi &NativePjsuaApi() noexcept {
    static const PjsuaApi api{pj_zephyr_pool_arena_install,
                              pj_zephyr_pool_arena_reset,
                              pjsua_create,
                              pjsua_config_default,
                              pjsua_logging_config_default,
                              pjsua_media_config_default,
                              pjsua_transport_config_default,
                              pjsua_init,
                              pjsua_set_no_snd_dev,
                              pjsua_transport_create,
                              pjsua_transport_close,
                              pjsua_start,
                              pjsua_handle_events,
                              pjsua_destroy,
                              pjsua_acc_config_default,
                              pjsua_acc_add,
                              pjsua_acc_set_user_data,
                              pjsua_acc_get_user_data,
                              pjsua_acc_del,
                              pjsua_acc_set_registration,
                              pjsua_call_answer,
                              pjsua_call_hangup};
#if defined(CONFIG_VOIP_PJSUA_PLAN3_COMPONENT_TEST)
    return component_test_api != nullptr ? *component_test_api : api;
#else
    return api;
#endif
}

Error PjsuaStatus(pj_status_t status) noexcept {
    return status == PJ_SUCCESS ? Error::ok : Error::internal_failure;
}
} // namespace voip
