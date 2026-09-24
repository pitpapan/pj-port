#ifndef VOIP_INTEGRATION_SCRIPTED_SIP_PEER_HPP
#define VOIP_INTEGRATION_SCRIPTED_SIP_PEER_HPP

#include <cstdint>

namespace voip_integration {

enum class SipPeerScript : std::uint8_t { answer, malformed_sdp, reject_486, close_transport, cancel_incoming, bye_after_ack, no_final, shared_stream };

struct SipPeerStats {
    unsigned invites;
    unsigned incoming_invites;
    unsigned acks;
    unsigned byes;
    unsigned cancels;
    unsigned trying;
    unsigned ringing;
    unsigned reinvites;
    unsigned reinvite_responses;
    unsigned reinvite_acks;
    unsigned registrations;
    unsigned incoming_trying;
    unsigned incoming_ringing;
    unsigned cancel_responses;
    unsigned cancelled_invites;
    unsigned minimum_stack_unused;
    unsigned parse_failures;
};

// A deliberately small raw TCP SIP peer. It owns no PJSUA object and only
// sees wire messages, so the integration test exercises the public service.
class ScriptedSipPeer final {
public:
    static constexpr unsigned listener_port = 5098;
    static constexpr unsigned pjsua_listener_port = 5097;

    static ScriptedSipPeer &Instance() noexcept;
    bool Start() noexcept;
    static bool FramingChecks() noexcept;
    static bool ZeroWorkerConfiguration() noexcept;
    void SetNextOutgoing(SipPeerScript script) noexcept;
    bool StartIncoming(SipPeerScript script, unsigned agent = 0) noexcept;
    SipPeerStats Stats() const noexcept;
    unsigned IncomingResponseOrder(unsigned sequence, unsigned code) const noexcept;
    unsigned OutgoingInviteOrder(unsigned sequence) const noexcept;
    unsigned IncomingPort(unsigned sequence) const noexcept;

private:
    ScriptedSipPeer() noexcept = default;
};

} // namespace voip_integration

#endif
