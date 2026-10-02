#ifndef VOIP_PJSUA_MEDIA_CLOCK_HPP
#define VOIP_PJSUA_MEDIA_CLOCK_HPP
#include "PjsuaApi.hpp"
#include <voip/PcmAudio.hpp>
#include <cstdint>
namespace voip {
class PjsuaMediaClock final {
public:
    explicit PjsuaMediaClock(const PjsuaApi &api=NativePjsuaApi()) noexcept : api_(api) {}
    ~PjsuaMediaClock() noexcept { Stop(); }
    Error Initialize(pjmedia_port *, PcmFormat) noexcept;
    Error Pump(std::uint64_t now_ms) noexcept;
    void Stop() noexcept;
    std::uint64_t Ticks() const noexcept { return ticks_; }
    std::uint64_t Skipped() const noexcept { return skipped_; }
    std::uint64_t MaxLatenessMs() const noexcept { return max_lateness_; }
private:
    const PjsuaApi &api_;
    pj_pool_t *pool_=nullptr;
    pjmedia_port *master_=nullptr;
    void *buffer_=nullptr;
    std::size_t bytes_=0;
    std::uint64_t anchor_=0, timestamp_=0, ticks_=0, skipped_=0, max_lateness_=0;
    std::uint32_t period_ms_=0, samples_=0;
    bool armed_=false;
};
}
#endif
