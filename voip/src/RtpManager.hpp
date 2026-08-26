#ifndef VOIP_RTP_MANAGER_HPP
#define VOIP_RTP_MANAGER_HPP

#include "PjHeadlessMedia.hpp"

namespace voip {

/* Owns the PJMEDIA RTP/RTCP session for one call slot. */
class RtpManager final {
public:
    RtpManager(pj_pool_factory *factory, pjmedia_endpt *endpoint) noexcept;
    ~RtpManager();

    RtpManager(const RtpManager &) = delete;
    RtpManager &operator=(const RtpManager &) = delete;

    Error Initialize() noexcept;
    Error Prepare(bool sdes_signaling = false) noexcept;
    Error EncodeSdesOffer(pj_pool_t *, pjmedia_sdp_session *) noexcept;
    Error EncodeSdesAnswer(pj_pool_t *, pjmedia_sdp_session *,
                           const pjmedia_sdp_session *) noexcept;
    Error ActivateSdes(pj_pool_t *, const pjmedia_sdp_session *,
                       const pjmedia_sdp_session *) noexcept;
    unsigned LocalRtpPort() const noexcept;
    unsigned PeerRtpPortForValidation() const noexcept;
    Error StartPrepared(Codec, const char *, unsigned) noexcept;
    Error Start(Codec) noexcept;
    Error SetPaused(bool) noexcept;
    Error StopCall() noexcept;
    Error Stop() noexcept;
    Error InjectTransportFailure() noexcept;
    MediaStats Stats() const noexcept;
    bool Running() const noexcept;
    bool SrtpKeysActiveForValidation() const noexcept;
    bool SrtpKeysClearedForValidation() const noexcept;
    bool SrtpTransportActiveForValidation() const noexcept;
    void Destroy() noexcept;

private:
    PjHeadlessMedia media_;
};

} // namespace voip

#endif
