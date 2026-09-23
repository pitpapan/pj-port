#ifndef VOIP_INTEGRATION_SCRIPTED_SIP_PEER_HPP
#define VOIP_INTEGRATION_SCRIPTED_SIP_PEER_HPP

#include <cstdint>

namespace voip_integration {

enum class SipPeerScript : std::uint8_t { answer, malformed_sdp, reject_486, close_transport, cancel_incoming, bye_after_ack };

struct SipPeerStats {
    unsigned invites;
    unsigned incoming_invites;
    unsigned acks;
    unsigned byes;
    unsigned cancels;
    unsigned parse_failures;
};

// A deliberately small raw TCP SIP peer. It owns no PJSUA object and only
// sees wire messages, so the integration test exercises the public service.
class ScriptedSipPeer final {
public:
    static constexpr unsigned listener_port = 5098;
    static constexpr unsigned pjsua_listener_port = 5097;

    static ScriptedSipPeer &Instance() noexcept;
    void Reset() noexcept;
    bool Start() noexcept;
    void Pump() noexcept;
    void SetNextOutgoing(SipPeerScript script) noexcept;
    bool StartIncoming(SipPeerScript script) noexcept;
    SipPeerStats Stats() const noexcept;

private:
    ScriptedSipPeer() noexcept = default;
};

} // namespace voip_integration

#endif
