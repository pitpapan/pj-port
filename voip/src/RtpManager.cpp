#include "RtpManager.hpp"

namespace voip {

RtpManager::RtpManager(pj_pool_factory *factory,
                       pjmedia_endpt *endpoint) noexcept
    : media_(factory, endpoint) {}

RtpManager::~RtpManager() { media_.Destroy(); }

Error RtpManager::Initialize() noexcept { return media_.Initialize(); }
Error RtpManager::Prepare(bool sdes) noexcept { return media_.Prepare(sdes); }
Error RtpManager::EncodeSdesOffer(pj_pool_t *pool,
                                   pjmedia_sdp_session *offer) noexcept {
    return media_.EncodeSdesOffer(pool, offer);
}
Error RtpManager::EncodeSdesAnswer(
    pj_pool_t *pool, pjmedia_sdp_session *answer,
    const pjmedia_sdp_session *offer) noexcept {
    return media_.EncodeSdesAnswer(pool, answer, offer);
}
Error RtpManager::ActivateSdes(
    pj_pool_t *pool, const pjmedia_sdp_session *local,
    const pjmedia_sdp_session *remote) noexcept {
    return media_.ActivateSdes(pool, local, remote);
}
unsigned RtpManager::LocalRtpPort() const noexcept {
    return media_.LocalRtpPort();
}
unsigned RtpManager::PeerRtpPortForValidation() const noexcept {
    return media_.PeerRtpPortForValidation();
}
Error RtpManager::StartPrepared(Codec codec, const char *address,
                                unsigned port) noexcept {
    return media_.StartPrepared(codec, address, port);
}
Error RtpManager::Start(Codec codec) noexcept { return media_.Start(codec); }
Error RtpManager::SetPaused(bool paused) noexcept {
    return media_.SetPaused(paused);
}
Error RtpManager::StopCall() noexcept { return media_.StopCall(); }
Error RtpManager::Stop() noexcept { return media_.Stop(); }
Error RtpManager::InjectTransportFailure() noexcept {
    return media_.InjectTransportFailure();
}
MediaStats RtpManager::Stats() const noexcept { return media_.Stats(); }
bool RtpManager::Running() const noexcept { return media_.Running(); }
bool RtpManager::SrtpKeysActiveForValidation() const noexcept {
    return media_.SrtpKeysActiveForValidation();
}
bool RtpManager::SrtpKeysClearedForValidation() const noexcept {
    return media_.SrtpKeysClearedForValidation();
}
bool RtpManager::SrtpTransportActiveForValidation() const noexcept {
    return media_.SrtpTransportActiveForValidation();
}
void RtpManager::Destroy() noexcept { media_.Destroy(); }

} // namespace voip
