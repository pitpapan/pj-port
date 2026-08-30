#include "PjsuaTransportManager.hpp"
#include <cassert>
namespace voip {
void PjsuaTransportManager::AssertActor() const noexcept {
#ifndef NDEBUG
    assert(actor_thread_ == std::thread::id{} || actor_thread_ == std::this_thread::get_id());
#endif
}
Error PjsuaTransportManager::Initialize(SignalingTransportPolicy policy) noexcept {
    AssertActor(); if (actor_thread_ == std::thread::id{}) actor_thread_ = std::this_thread::get_id();
    if (policy != SignalingTransportPolicy::tcp_plain) return Error::unsupported_configuration;
    if (id_ != PJSUA_INVALID_ID) return Error::invalid_state;
    pjsua_transport_config config{}; api_.transport_config_default(&config);
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
