#ifndef VOIP_PJSUA_TRANSPORT_MANAGER_HPP
#define VOIP_PJSUA_TRANSPORT_MANAGER_HPP

#include "PjsuaApi.hpp"
#include "SignalingTransportPolicy.hpp"
#include <thread>
namespace voip {
class PjsuaTransportManager final {
public:
    explicit PjsuaTransportManager(const PjsuaApi &api = NativePjsuaApi()) noexcept : api_(api) {}
    Error Initialize(SignalingTransportPolicy) noexcept;
    pjsua_transport_id Id() const noexcept;
    Error Shutdown() noexcept;
private:
    const PjsuaApi &api_; pjsua_transport_id id_ = PJSUA_INVALID_ID;
    std::thread::id actor_thread_{};
    void AssertActor() const noexcept;
};
} // namespace voip
#endif
