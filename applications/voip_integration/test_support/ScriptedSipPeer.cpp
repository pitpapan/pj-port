#include "ScriptedSipPeer.hpp"

#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/sys/atomic.h>
#include <pjsip.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
using voip_integration::ScriptedSipPeer;
using voip_integration::SipPeerScript;

constexpr int invalid_fd = -1;
constexpr unsigned max_connections = 3;
constexpr unsigned message_bytes = 1536;
K_THREAD_STACK_DEFINE(peer_stack, 4096);

struct Connection {
    int fd{invalid_fd};
    bool incoming{};
    SipPeerScript script{SipPeerScript::answer};
    char call_id[128]{};
    char from[256]{};
    char to[256]{};
    char via[256]{};
    char cseq[64]{};
    char buffer[message_bytes]{};
    unsigned used{};
};
struct PeerState {
    int listener{invalid_fd};
    Connection connections[max_connections]{};
    SipPeerScript next_outgoing{SipPeerScript::answer};
    SipPeerScript next_incoming{SipPeerScript::answer};
    atomic_t incoming_requested{};
    atomic_t incoming_sequence{};
    atomic_t running{};
    k_thread thread{};
    atomic_t invites{}, incoming_invites{}, acks{}, byes{}, cancels{}, parse_failures{};
};
PeerState state;

void Close(Connection &connection) { if (connection.fd != invalid_fd) (void)zsock_close(connection.fd); connection = {}; }
const char *Header(const char *message, const char *name) {
    const std::size_t length = std::strlen(name);
    for (const char *line = message; line != nullptr && *line != '\0'; ) {
        if (std::strncmp(line, name, length) == 0) return line + length;
        line = std::strstr(line, "\r\n");
        if (line != nullptr) line += 2;
    }
    return nullptr;
}
bool CopyHeader(char *out, std::size_t capacity, const char *message, const char *name) {
    const char *value = Header(message, name);
    if (value == nullptr) return false;
    const char *end = std::strstr(value, "\r\n");
    if (end == nullptr || static_cast<std::size_t>(end - value) >= capacity) return false;
    std::memcpy(out, value, static_cast<std::size_t>(end - value));
    out[end - value] = '\0';
    return true;
}
bool ParseInvite(Connection &connection) {
    return CopyHeader(connection.via, sizeof(connection.via), connection.buffer, "Via: ") &&
           CopyHeader(connection.from, sizeof(connection.from), connection.buffer, "From: ") &&
           CopyHeader(connection.to, sizeof(connection.to), connection.buffer, "To: ") &&
           CopyHeader(connection.call_id, sizeof(connection.call_id), connection.buffer, "Call-ID: ") &&
           CopyHeader(connection.cseq, sizeof(connection.cseq), connection.buffer, "CSeq: ");
}
void Send(Connection &connection, const char *message) {
    if (connection.fd == invalid_fd || message == nullptr) return;
    const char *cursor = message;
    std::size_t remaining = std::strlen(message);
    while (remaining != 0) {
        const int sent = zsock_send(connection.fd, cursor, remaining, 0);
        if (sent <= 0) return;
        cursor += sent;
        remaining -= static_cast<std::size_t>(sent);
    }
}
void Reply(Connection &connection, unsigned code, const char *reason, const char *body) {
    char message[message_bytes]{};
    const unsigned body_length = body == nullptr ? 0U : static_cast<unsigned>(std::strlen(body));
    std::snprintf(message, sizeof(message),
                  "SIP/2.0 %u %s\r\nVia: %s\r\nFrom: %s\r\nTo: %s;tag=peer-fixed\r\nCall-ID: %s\r\nCSeq: %s\r\n%sContent-Length: %u\r\n%s%s",
                  code, reason, connection.via, connection.from, connection.to,
                  connection.call_id, connection.cseq,
                  body == nullptr ? "" : "Contact: <sip:remote-a@127.0.0.1:5098;transport=tcp>\r\n", body_length,
                  body_length == 0 ? "\r\n" : "Content-Type: application/sdp\r\n\r\n",
                  body == nullptr ? "" : body);
    Send(connection, message);
}
const char *Sdp(bool valid) {
    return valid ? "v=0\r\no=peer 1 1 IN IP4 127.0.0.1\r\ns=peer\r\nc=IN IP4 127.0.0.1\r\nt=0 0\r\nm=audio 4002 RTP/AVP 0\r\na=rtpmap:0 PCMU/8000\r\na=sendrecv\r\n"
                 : "v=0\r\no=broken\r\nm=audio not-a-port\r\n";
}
void SendBye(Connection &connection) {
    char message[message_bytes]{};
    std::snprintf(message, sizeof(message),
                  "BYE sip:agent@127.0.0.1 SIP/2.0\r\nVia: SIP/2.0/TCP 127.0.0.1;branch=z9hG4bK-peer-bye\r\nFrom: %s;tag=peer-fixed\r\nTo: %s\r\nCall-ID: %s\r\nCSeq: 2 BYE\r\nContent-Length: 0\r\n\r\n",
                  connection.to, connection.from, connection.call_id);
    Send(connection, message);
    atomic_inc(&state.byes);
}
void SendCancel(Connection &connection) {
    char message[message_bytes]{};
    std::snprintf(message, sizeof(message),
                  "CANCEL sip:agent0@127.0.0.1 SIP/2.0\r\nVia: %s\r\nFrom: %s\r\nTo: %s\r\nCall-ID: %s\r\nCSeq: 1 CANCEL\r\nContent-Length: 0\r\n\r\n",
                  connection.via, connection.from, connection.to, connection.call_id);
    Send(connection, message);
    atomic_inc(&state.cancels);
}
void Handle(Connection &connection) {
    if (std::strncmp(connection.buffer, "REGISTER ", 9) == 0) {
        if (!ParseInvite(connection)) { atomic_inc(&state.parse_failures); Close(connection); return; }
        Reply(connection, 200, "OK", nullptr);
        return;
    }
    if (std::strncmp(connection.buffer, "INVITE ", 7) == 0) {
        atomic_inc(&state.invites);
        if (!ParseInvite(connection)) { atomic_inc(&state.parse_failures); Close(connection); return; }
        if (connection.script == SipPeerScript::close_transport) { Close(connection); return; }
        if (connection.script == SipPeerScript::reject_486) { Reply(connection, 486, "Busy Here", nullptr); return; }
        // PJSIP's TCP transport consumes one complete message per poll here.
        // Keep the deterministic peer to a single final answer rather than
        // coalescing provisional and final responses in one TCP receive.
        Reply(connection, 200, "OK", Sdp(connection.script != SipPeerScript::malformed_sdp));
        return;
    }
    if (std::strncmp(connection.buffer, "ACK ", 4) == 0) {
        atomic_inc(&state.acks);
        if (connection.script == SipPeerScript::bye_after_ack) SendBye(connection);
        return;
    }
    if (std::strncmp(connection.buffer, "BYE ", 4) == 0) {
        atomic_inc(&state.byes);
        if (ParseInvite(connection)) Reply(connection, 200, "OK", nullptr);
        return;
    }
    // TCP receives may coalesce provisional and final responses. The peer
    // needs only those deterministic response markers, never SIP internals.
    if (std::strstr(connection.buffer, "SIP/2.0 180") != nullptr && connection.incoming &&
        connection.script == SipPeerScript::cancel_incoming) { SendCancel(connection); return; }
    if (std::strstr(connection.buffer, "SIP/2.0 200") != nullptr && connection.incoming) {
        if (!CopyHeader(connection.to, sizeof(connection.to), connection.buffer, "To: ")) {
            atomic_inc(&state.parse_failures);
            return;
        }
        char ack[message_bytes]{};
        std::snprintf(ack, sizeof(ack), "ACK sip:agent0@127.0.0.1 SIP/2.0\r\nVia: SIP/2.0/TCP 127.0.0.1;branch=z9hG4bK-peer-ack\r\nFrom: %s\r\nTo: %s\r\nCall-ID: %s\r\nCSeq: 1 ACK\r\nContent-Length: 0\r\n\r\n", connection.from, connection.to, connection.call_id);
        Send(connection, ack); atomic_inc(&state.acks); return;
    }
}
unsigned BodyLength(const char *message, const char *header_end) {
    const char *value = Header(message, "Content-Length: ");
    if (value == nullptr || value >= header_end) return 0;
    return static_cast<unsigned>(std::strtoul(value, nullptr, 10));
}
void Consume(Connection &connection) {
    while (connection.used != 0) {
        connection.buffer[connection.used] = '\0';
        char *header_end = std::strstr(connection.buffer, "\r\n\r\n");
        if (header_end == nullptr) return;
        const unsigned header_bytes = static_cast<unsigned>(header_end - connection.buffer) + 4U;
        const unsigned message_size = header_bytes + BodyLength(connection.buffer, header_end);
        if (message_size > connection.used) return;
        const char saved = connection.buffer[message_size];
        connection.buffer[message_size] = '\0';
        Handle(connection);
        if (connection.fd == invalid_fd) return;
        connection.buffer[message_size] = saved;
        const unsigned remaining = connection.used - message_size;
        if (remaining != 0)
            std::memmove(connection.buffer, connection.buffer + message_size, remaining);
        connection.used = remaining;
    }
}
void MakeIncoming() {
    for (Connection &connection : state.connections) {
        if (connection.fd != invalid_fd) continue;
        connection.fd = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (connection.fd < 0) { connection = {}; return; }
        sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(ScriptedSipPeer::pjsua_listener_port); address.sin_addr.s_addr = htonl(0x7f000001U);
        if (zsock_connect(connection.fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) { Close(connection); return; }
        connection.incoming = true; connection.script = state.next_incoming;
        std::snprintf(connection.via, sizeof(connection.via), "SIP/2.0/TCP 127.0.0.1;branch=z9hG4bK-peer-in");
        std::snprintf(connection.from, sizeof(connection.from), "<sip:remote-b@peer.test>;tag=peer-in");
        std::snprintf(connection.to, sizeof(connection.to), "<sip:agent0@127.0.0.1>");
        const unsigned sequence = static_cast<unsigned>(atomic_inc(&state.incoming_sequence)) + 1U;
        std::snprintf(connection.call_id, sizeof(connection.call_id), "peer-incoming-%u", sequence);
        std::snprintf(connection.cseq, sizeof(connection.cseq), "1 INVITE");
        char invite[message_bytes]{};
        const char *sdp = Sdp(true);
        std::snprintf(invite, sizeof(invite), "INVITE sip:agent0@127.0.0.1 SIP/2.0\r\nVia: %s\r\nFrom: %s\r\nTo: %s\r\nCall-ID: %s\r\nCSeq: %s\r\nContact: <sip:remote-b@127.0.0.1:5098;transport=tcp>\r\nContent-Type: application/sdp\r\nContent-Length: %u\r\n\r\n%s", connection.via, connection.from, connection.to, connection.call_id, connection.cseq, static_cast<unsigned>(std::strlen(sdp)), sdp);
        Send(connection, invite); atomic_inc(&state.incoming_invites); return;
    }
}
void PumpPeer() {
    if (atomic_get(&state.running) != 0) {
        if (atomic_cas(&state.incoming_requested, 1, 0)) MakeIncoming();
        zsock_pollfd listener_poll{};
        listener_poll.fd = state.listener;
        listener_poll.events = ZSOCK_POLLIN;
        if (state.listener != invalid_fd && zsock_poll(&listener_poll, 1, 0) > 0 &&
            (listener_poll.revents & ZSOCK_POLLIN) != 0) {
            sockaddr_in address{}; socklen_t length = sizeof(address);
            int fd = zsock_accept(state.listener, reinterpret_cast<sockaddr *>(&address), &length);
            if (fd >= 0) for (Connection &connection : state.connections) if (connection.fd == invalid_fd) { connection.fd = fd; connection.script = state.next_outgoing; break; }
        }
        for (Connection &connection : state.connections) {
            if (connection.fd == invalid_fd) continue;
            const unsigned available = sizeof(connection.buffer) - 1U - connection.used;
            if (available == 0) { atomic_inc(&state.parse_failures); Close(connection); continue; }
            int received = zsock_recv(connection.fd, connection.buffer + connection.used, available, ZSOCK_MSG_DONTWAIT);
            if (received > 0) { connection.used += static_cast<unsigned>(received); Consume(connection); }
        }
    }
}
void PeerEntry(void *, void *, void *) {
    while (atomic_get(&state.running) != 0) {
        PumpPeer();
        k_sleep(K_MSEC(1));
    }
}
} // namespace

namespace voip_integration {
ScriptedSipPeer &ScriptedSipPeer::Instance() noexcept { static ScriptedSipPeer peer; return peer; }
void ScriptedSipPeer::Reset() noexcept { for (Connection &connection : state.connections) Close(connection); state.next_outgoing = SipPeerScript::answer; state.next_incoming = SipPeerScript::answer; atomic_set(&state.incoming_sequence, 0); atomic_set(&state.invites, 0); atomic_set(&state.incoming_invites, 0); atomic_set(&state.acks, 0); atomic_set(&state.byes, 0); atomic_set(&state.cancels, 0); atomic_set(&state.parse_failures, 0); }
bool ScriptedSipPeer::Start() noexcept {
    if (atomic_get(&state.running) != 0) return true;
    state.listener = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (state.listener < 0) return false;
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(listener_port); address.sin_addr.s_addr = htonl(0x7f000001U);
    if (zsock_bind(state.listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 || zsock_listen(state.listener, 3) != 0) { (void)zsock_close(state.listener); state.listener = invalid_fd; return false; }
    atomic_set(&state.running, 1);
    (void)k_thread_create(&state.thread, peer_stack, K_THREAD_STACK_SIZEOF(peer_stack),
                          PeerEntry, nullptr, nullptr, nullptr, 5, 0, K_NO_WAIT);
    return true;
}
void ScriptedSipPeer::Pump() noexcept { PumpPeer(); }
void ScriptedSipPeer::SetNextOutgoing(SipPeerScript script) noexcept { state.next_outgoing = script; for (Connection &connection : state.connections) if (connection.fd != invalid_fd && !connection.incoming) connection.script = script; }
bool ScriptedSipPeer::StartIncoming(SipPeerScript script) noexcept { state.next_incoming = script; return atomic_cas(&state.incoming_requested, 0, 1); }
SipPeerStats ScriptedSipPeer::Stats() const noexcept { return {static_cast<unsigned>(atomic_get(&state.invites)), static_cast<unsigned>(atomic_get(&state.incoming_invites)), static_cast<unsigned>(atomic_get(&state.acks)), static_cast<unsigned>(atomic_get(&state.byes)), static_cast<unsigned>(atomic_get(&state.cancels)), static_cast<unsigned>(atomic_get(&state.parse_failures))}; }
} // namespace voip_integration

extern "C" unsigned voip_pjsua_registration_test_listener_port() noexcept { return ScriptedSipPeer::pjsua_listener_port; }
extern "C" pj_status_t voip_pjsua_registration_test_install(pjsip_endpoint *) noexcept { return PJ_SUCCESS; }
