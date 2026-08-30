#ifndef VOIP_PJSUA_RUNTIME_HPP
#define VOIP_PJSUA_RUNTIME_HPP

#include "PjsuaApi.hpp"
#include <voip/PcmAudio.hpp>
#include <cstdint>
#include <thread>

namespace voip {
class PjsuaRuntime final {
public:
    explicit PjsuaRuntime(const PjsuaApi &api = NativePjsuaApi()) noexcept : api_(api) {}
    Error CreateAndInitialize(const pjsua_callback &, const PcmFormat &) noexcept;
    Error Start() noexcept;
    Error Pump(std::uint32_t timeout_ms) noexcept;
    Error Destroy() noexcept;
private:
    const PjsuaApi &api_;
    bool arena_ = false, created_ = false, initialized_ = false, started_ = false;
    std::thread::id actor_thread_{};
    void AssertActor() const noexcept;
};
} // namespace voip
#endif
