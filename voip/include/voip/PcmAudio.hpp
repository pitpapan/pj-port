#ifndef VOIP_PCM_AUDIO_HPP
#define VOIP_PCM_AUDIO_HPP

#include <voip/VoipTypes.hpp>

#include <cstddef>
#include <cstdint>

namespace voip {

enum class SampleFormat : std::uint8_t { signed_16 };

struct PcmFormat {
    std::uint32_t sample_rate_hz;
    std::uint16_t samples_per_frame;
    std::uint8_t channels;
    SampleFormat sample_format;
};

// These application-owned objects are borrowed from successful Initialize()
// through completed Shutdown(). Methods are non-blocking and noexcept.
// timestamp is a conference sample tick (at Format().sample_rate_hz), not a
// millisecond time or an encoded RTP timestamp. Each callback covers exactly
// Format().samples_per_frame mono samples. busy/resource_exhausted denotes
// transient capture underrun/playout overflow; another error is fatal to the
// affected call. A failed Read may have written a partial frame: the runtime
// discards the whole frame and transmits silence.
class PcmSource {
public:
    virtual ~PcmSource() noexcept = default;
    virtual PcmFormat Format() const noexcept = 0;
    virtual Error Read(std::int16_t *samples, std::size_t sample_count,
                       std::uint64_t timestamp) noexcept = 0;
};

class PcmSink {
public:
    virtual ~PcmSink() noexcept = default;
    virtual PcmFormat Format() const noexcept = 0;
    virtual Error Write(const std::int16_t *samples, std::size_t sample_count,
                        std::uint64_t timestamp) noexcept = 0;
    // Discard stale playout data without freeing sink-owned storage.
    virtual void Flush() noexcept = 0;
};

} // namespace voip

#endif
