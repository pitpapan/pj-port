#ifndef VOIP_PJSUA_AUDIO_PORT_HPP
#define VOIP_PJSUA_AUDIO_PORT_HPP
#include <voip/PcmAudio.hpp>
#include <pjmedia/port.h>
#include <atomic>

namespace voip {
static_assert(std::atomic<unsigned>::is_always_lock_free &&
              std::atomic<bool>::is_always_lock_free &&
              std::atomic<Error>::is_always_lock_free,
              "PCM callback counters and mailbox must not take locks");
// Stable, allocation-free callbacks. The bridge owns native reachability;
// stopping and an empty callback count alone do not authorize native reuse.
class PjsuaAudioPort final {
public:
    Error Initialize(AgentAudioBinding, PcmFormat) noexcept;
    pjmedia_port *Port() noexcept { return &port_; }
    void BeginStop() noexcept { stopping_.store(true, std::memory_order_release); }
    bool IsQuiescent() const noexcept {
        return stopping_.load(std::memory_order_acquire) && entries_.load(std::memory_order_acquire)==0;
    }
    Error TakeError() noexcept { return error_.exchange(Error::ok, std::memory_order_acq_rel); }
private:
    static pj_status_t Get(pjmedia_port *, pjmedia_frame *) noexcept;
    static pj_status_t Put(pjmedia_port *, pjmedia_frame *) noexcept;
    void Latch(Error) noexcept;
    pjmedia_port port_{};
    AgentAudioBinding binding_{};
    PcmFormat format_{};
    std::size_t bytes_=0;
    std::atomic<unsigned> entries_{0};
    std::atomic<bool> stopping_{true};
    std::atomic<Error> error_{Error::ok};
};
} // namespace voip
#endif
