#include "PjsuaTransportManager.hpp"
#include <cassert>
namespace voip {
#if defined(CONFIG_VOIP_PJSUA_REGISTRATION_TEST)
// This symbol is supplied solely by the integration-test support target.  It
// is intentionally not declared by a public VoIP header or linked by normal
// PJSUA builds.
extern "C" unsigned voip_pjsua_registration_test_listener_port() noexcept;
#endif
void PjsuaTransportManager::AssertActor() const noexcept {
#ifndef NDEBUG
    assert(actor_thread_ == std::thread::id{} || actor_thread_ == std::this_thread::get_id());
#endif
}
pjsua_transport_id PjsuaTransportManager::Id() const noexcept {
    AssertActor();
    return id_;
}
Error PjsuaTransportManager::Initialize(SignalingTransportPolicy policy) noexcept {
    AssertActor(); if (actor_thread_ == std::thread::id{}) actor_thread_ = std::this_thread::get_id();
    if (policy != SignalingTransportPolicy::tcp_plain) return Error::unsupported_configuration;
    if (id_ != PJSUA_INVALID_ID) return Error::invalid_state;
    pjsua_transport_config config{}; api_.transport_config_default(&config);
#if defined(CONFIG_VOIP_PJSUA_REGISTRATION_TEST)
    config.port = static_cast<pj_uint16_t>(voip_pjsua_registration_test_listener_port());
    config.bound_addr = pj_str(const_cast<char *>("127.0.0.1"));
#endif
    if (PjsuaStatus(api_.transport_create(PJSIP_TRANSPORT_TCP, &config, &id_)) != Error::ok) {
        id_ = PJSUA_INVALID_ID; return Error::internal_failure;
    }
    return Error::ok;
}
Error PjsuaTransportManager::Shutdown() noexcept {
    AssertActor();
    if (id_ != PJSUA_INVALID_ID) (void)api_.transport_close(id_, PJ_FALSE);
    id_ = PJSUA_INVALID_ID; actor_thread_ = {}; return Error::ok;
}
} // namespace voip
