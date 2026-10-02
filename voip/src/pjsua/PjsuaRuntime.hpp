#ifndef VOIP_PJSUA_RUNTIME_HPP
#define VOIP_PJSUA_RUNTIME_HPP

#include "PjsuaApi.hpp"
#include "PjsuaMediaClock.hpp"
#include <voip/PcmAudio.hpp>
#include <cstdint>
#include <thread>

namespace voip {
class PjsuaRuntime final {
public:
    explicit PjsuaRuntime(const PjsuaApi &api = NativePjsuaApi()) noexcept : api_(api), clock_(api) {}
    Error CreateAndInitialize(const pjsua_callback &, const PcmFormat &) noexcept;
    Error EnsureActorRegistered() noexcept;
    Error Start() noexcept;
    Error Pump(std::uint32_t timeout_ms) noexcept;
    Error PumpMedia(std::uint64_t now_ms) noexcept;
    const PjsuaMediaClock &Clock() const noexcept { return clock_; }
    Error Destroy() noexcept;
    bool Created() const noexcept { return created_; }
private:
    const PjsuaApi &api_;
    PjsuaMediaClock clock_;
    pjmedia_port *master_port_=nullptr;
    bool arena_ = false, created_ = false, initialized_ = false, started_ = false;
    pj_thread_desc actor_thread_desc_{};
    pj_thread_t *actor_pj_thread_ = nullptr;
    std::thread::id actor_thread_{};
    void AssertActor() const noexcept;
};
} // namespace voip
#endif
