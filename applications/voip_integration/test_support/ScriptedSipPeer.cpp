#include "ScriptedSipPeer.hpp"

#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>
#include <pjsip.h>
#include <pjsua-lib/pjsua.h>

#include <cstdio>
#include <cstring>

namespace {
using voip_integration::ScriptedSipPeer;
using voip_integration::SipPeerScript;

constexpr int invalid_fd = -1;
constexpr unsigned max_connections = 12;
constexpr unsigned message_bytes = 1536;
constexpr unsigned max_observed_calls = 64;
K_THREAD_STACK_DEFINE(peer_stack, 8192);

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
    unsigned response_stage{};
    int64_t response_at{};
    bool reinvite_sent{};
    bool bye_sent{};
    bool cancel_sent{};
    unsigned sequence{};
};
struct PeerState {
    // Only PeerEntry/PumpPeer touches sockets or connection/script state.
    // Public calls exchange scalar requests and observations through atomics.
    int listener{invalid_fd};
    Connection connections[max_connections]{};
    atomic_t next_outgoing{static_cast<atomic_val_t>(SipPeerScript::answer)};
    atomic_t incoming_requested{};
    atomic_t incoming_sequence{};
    atomic_t running{};
    atomic_t ready{};
    atomic_t zero_workers{};
    atomic_t media_transport_creations{};
    atomic_t minimum_stack_unused{8192};
    k_thread thread{};
    atomic_t invites{}, incoming_invites{}, acks{}, byes{}, cancels{}, trying{}, ringing{}, reinvites{}, reinvite_responses{}, parse_failures{};
    atomic_t reinvite_acks{}, registrations{}, incoming_trying{}, incoming_ringing{}, cancel_responses{}, cancelled_invites{};
    atomic_t wire_order{};
    atomic_t incoming_180_order[max_observed_calls]{}, incoming_200_order[max_observed_calls]{};
    atomic_t incoming_480_order[max_observed_calls]{}, incoming_486_order[max_observed_calls]{};
    atomic_t outgoing_invite_order[max_observed_calls]{};
    atomic_t incoming_port[max_observed_calls]{};
};
PeerState state;

void Close(Connection &connection) { if (connection.fd != invalid_fd) (void)zsock_close(connection.fd); connection = {}; }
const char *Header(const char *message, const char *name) {
    const std::size_t length = std::strlen(name);
    for (const char *line = message; line != nullptr && *line != '\0'; ) {
        if (line[0] == '\r' && line[1] == '\n') break;
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
        const int sent = zsock_send(connection.fd, cursor, remaining, ZSOCK_MSG_DONTWAIT);
        if (sent <= 0) { atomic_inc(&state.parse_failures); return; }
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
        atomic_inc(&state.registrations);
        return;
    }
    if (std::strncmp(connection.buffer, "INVITE ", 7) == 0) {
        // The FIFO fixture sends the next outgoing dialog over the stream on
        // which its incoming 200/ACK handshake has just been observed. Later
        // requests carry their own dialog headers (ParseInvite below).
        if (connection.script == SipPeerScript::shared_stream)
            connection.incoming = false;
        const unsigned invite = static_cast<unsigned>(atomic_inc(&state.invites)) + 1U;
        if (invite < max_observed_calls)
            atomic_set(&state.outgoing_invite_order[invite], atomic_inc(&state.wire_order) + 1);
        if (!ParseInvite(connection)) { atomic_inc(&state.parse_failures); Close(connection); return; }
        connection.script = static_cast<SipPeerScript>(atomic_get(&state.next_outgoing));
        connection.bye_sent = false;
        if (connection.script == SipPeerScript::close_transport) { Close(connection); return; }
        if (connection.script == SipPeerScript::reject_486) { Reply(connection, 486, "Busy Here", nullptr); return; }
        Reply(connection, 100, "Trying", nullptr); atomic_inc(&state.trying);
        connection.response_stage = 1;
        connection.response_at = k_uptime_get() + 50;
        return;
    }
    if (std::strncmp(connection.buffer, "ACK ", 4) == 0) {
        atomic_inc(&state.acks);
        if (connection.script == SipPeerScript::bye_after_ack && !connection.bye_sent) {
            connection.bye_sent = true; SendBye(connection);
        }
        return;
    }
    if (std::strncmp(connection.buffer, "BYE ", 4) == 0) {
        connection.response_stage = 0;
        atomic_inc(&state.byes);
        if (ParseInvite(connection)) Reply(connection, 200, "OK", nullptr);
        if (connection.incoming) { connection.response_stage = 4; connection.response_at = k_uptime_get() + 100; }
        return;
    }
    if (std::strncmp(connection.buffer, "CANCEL ", 7) == 0 && !connection.incoming) {
        if (!ParseInvite(connection)) { atomic_inc(&state.parse_failures); Close(connection); return; }
        Reply(connection, 200, "OK", nullptr);
        // CANCEL carries the cancelled INVITE's sequence number. Do not use
        // whichever dialog last happened to use this shared TCP connection.
        char *method = std::strchr(connection.cseq, ' ');
        if (method == nullptr) { atomic_inc(&state.parse_failures); return; }
        std::snprintf(method, sizeof(connection.cseq) - static_cast<std::size_t>(method - connection.cseq), " INVITE");
        Reply(connection, 487, "Request Terminated", nullptr);
        // Outgoing calls share this TCP connection with registration. Ending
        // the INVITE transaction must leave the transport available.
        connection.response_stage = 0;
        return;
    }
    if (!connection.incoming) return;
    char cseq[64]{};
    char call_id[128]{};
    if (!CopyHeader(call_id, sizeof(call_id), connection.buffer, "Call-ID: ") ||
        std::strcmp(call_id, connection.call_id) != 0) { atomic_inc(&state.parse_failures); return; }
    if (!CopyHeader(cseq, sizeof(cseq), connection.buffer, "CSeq: ")) return;
    if (std::strncmp(connection.buffer, "SIP/2.0 100 ", 12) == 0) atomic_inc(&state.incoming_trying);
    if (std::strncmp(connection.buffer, "SIP/2.0 180 ", 12) == 0) {
        atomic_inc(&state.incoming_ringing);
        if (connection.sequence < max_observed_calls)
            atomic_set(&state.incoming_180_order[connection.sequence], atomic_inc(&state.wire_order) + 1);
        if (connection.script == SipPeerScript::cancel_incoming && !connection.cancel_sent) {
            connection.cancel_sent = true; SendCancel(connection);
        }
    }
    const bool success = std::strncmp(connection.buffer, "SIP/2.0 200 ", 12) == 0;
    const bool cancelled = std::strncmp(connection.buffer, "SIP/2.0 487 ", 12) == 0;
    const bool timed_out = std::strncmp(connection.buffer, "SIP/2.0 480 ", 12) == 0;
    const bool busy = std::strncmp(connection.buffer, "SIP/2.0 486 ", 12) == 0;
    if (std::strcmp(cseq, "1 INVITE") == 0 && connection.sequence < max_observed_calls) {
        atomic_t *order = success ? state.incoming_200_order : timed_out ? state.incoming_480_order :
            busy ? state.incoming_486_order : nullptr;
        if (order != nullptr) atomic_set(&order[connection.sequence], atomic_inc(&state.wire_order) + 1);
    }
    if (success && std::strcmp(cseq, "1 CANCEL") == 0) atomic_inc(&state.cancel_responses);
    if ((success || cancelled || timed_out || busy) && std::strstr(cseq, " INVITE") != nullptr) {
        if (!CopyHeader(connection.to, sizeof(connection.to), connection.buffer, "To: ")) {
            atomic_inc(&state.parse_failures);
            return;
        }
        char ack[message_bytes]{};
        const unsigned sequence = std::strcmp(cseq, "2 INVITE") == 0 ? 2U : 1U;
        const char *via = (cancelled || timed_out || busy) ? connection.via : "SIP/2.0/TCP 127.0.0.1;branch=z9hG4bK-peer-ack";
        const char *answer_sdp = success && sequence == 1 &&
            connection.script == SipPeerScript::offerless ? Sdp(true) : "";
        std::snprintf(ack, sizeof(ack), "ACK sip:agent0@127.0.0.1 SIP/2.0\r\nVia: %s\r\nFrom: %s\r\nTo: %s\r\nCall-ID: %s\r\nCSeq: %u ACK\r\nContent-Length: %u\r\n%s%s", via, connection.from, connection.to, connection.call_id, sequence,
            static_cast<unsigned>(std::strlen(answer_sdp)), *answer_sdp ?
            "Content-Type: application/sdp\r\n\r\n" : "\r\n", answer_sdp);
        Send(connection, ack); atomic_inc(&state.acks);
        if (cancelled || timed_out || busy) {
            atomic_inc(&state.cancelled_invites);
            connection.response_stage = 4; connection.response_at = k_uptime_get() + 100;
        }
        else if (sequence == 2) { atomic_inc(&state.reinvite_responses); atomic_inc(&state.reinvite_acks); }
        else if (!connection.reinvite_sent && connection.script != SipPeerScript::shared_stream) {
            connection.reinvite_sent = true;
            connection.response_stage = 3;
            connection.response_at = k_uptime_get() + 100;
        }
        return;
    }
}
bool BodyLength(const char *message, const char *header_end, unsigned *length) {
    const char *value = Header(message, "Content-Length:");
    if (length == nullptr) return false;
    if (value == nullptr) { *length = 0; return true; }
    while (value < header_end && (*value == ' ' || *value == '\t')) ++value;
    if (value >= header_end || *value < '0' || *value > '9') return false;
    unsigned result = 0;
    while (value < header_end && *value >= '0' && *value <= '9') {
        const unsigned digit = static_cast<unsigned>(*value++ - '0');
        if (result > (message_bytes - 1U - digit) / 10U) return false;
        result = result * 10U + digit;
    }
    while (value < header_end && (*value == ' ' || *value == '\t')) ++value;
    if (*value != '\r') return false;
    *length = result;
    return true;
}
bool FrameSize(const char *message, const char *header_end, unsigned *size) {
    const unsigned header_bytes = static_cast<unsigned>(header_end - message) + 4U;
    unsigned body_bytes{};
    if (header_bytes > message_bytes - 1U ||
        !BodyLength(message, header_end, &body_bytes) ||
        body_bytes > message_bytes - 1U - header_bytes) return false;
    *size = header_bytes + body_bytes;
    return true;
}
void Consume(Connection &connection) {
    while (connection.used != 0) {
        connection.buffer[connection.used] = '\0';
        char *header_end = std::strstr(connection.buffer, "\r\n\r\n");
        if (header_end == nullptr) return;
        unsigned message_size{};
        if (!FrameSize(connection.buffer, header_end, &message_size)) {
            atomic_inc(&state.parse_failures); Close(connection); return;
        }
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
void MakeIncoming(SipPeerScript script, unsigned agent) {
    for (Connection &connection : state.connections) {
        if (connection.fd != invalid_fd) continue;
        connection.fd = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (connection.fd < 0) { connection = {}; return; }
        sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(ScriptedSipPeer::pjsua_listener_port); address.sin_addr.s_addr = htonl(0x7f000001U);
        if (zsock_connect(connection.fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) { Close(connection); return; }
        connection.incoming = true; connection.script = script;
        const unsigned sequence = static_cast<unsigned>(atomic_inc(&state.incoming_sequence)) + 1U;
        connection.sequence = sequence;
        sockaddr_in local{}; socklen_t local_size = sizeof(local);
        if (zsock_getsockname(connection.fd, reinterpret_cast<sockaddr *>(&local), &local_size) == 0 && sequence < max_observed_calls)
            atomic_set(&state.incoming_port[sequence], ntohs(local.sin_port));
        std::snprintf(connection.via, sizeof(connection.via), "SIP/2.0/TCP 127.0.0.1;branch=z9hG4bK-peer-in-%u", sequence);
        std::snprintf(connection.from, sizeof(connection.from), "<sip:remote-in-%u@peer.test>;tag=peer-in-%u", sequence, sequence);
        std::snprintf(connection.to, sizeof(connection.to), "<sip:agent%u@127.0.0.1>", agent);
        std::snprintf(connection.call_id, sizeof(connection.call_id), "peer-incoming-%u", sequence);
        std::snprintf(connection.cseq, sizeof(connection.cseq), "1 INVITE");
        char invite[message_bytes]{};
        const char *sdp = script == SipPeerScript::offerless ? "" : Sdp(true);
        std::snprintf(invite, sizeof(invite), "INVITE sip:agent%u@127.0.0.1 SIP/2.0\r\nVia: %s\r\nFrom: %s\r\nTo: %s\r\nCall-ID: %s\r\nCSeq: %s\r\nContact: <sip:remote-in-%u@127.0.0.1:5098;transport=tcp>\r\nContent-Type: application/sdp\r\nContent-Length: %u\r\n\r\n%s", agent, connection.via, connection.from, connection.to, connection.call_id, connection.cseq, sequence, static_cast<unsigned>(std::strlen(sdp)), sdp);
        Send(connection, invite); atomic_inc(&state.incoming_invites); return;
    }
}
void PumpPeer() {
    if (atomic_get(&state.running) != 0) {
        const auto requested = atomic_set(&state.incoming_requested, 0);
        if (requested != 0) MakeIncoming(static_cast<SipPeerScript>((requested & 0xff) - 1), static_cast<unsigned>(requested >> 8));
        zsock_pollfd listener_poll{};
        listener_poll.fd = state.listener;
        listener_poll.events = ZSOCK_POLLIN;
        if (state.listener != invalid_fd && zsock_poll(&listener_poll, 1, 0) > 0 &&
            (listener_poll.revents & ZSOCK_POLLIN) != 0) {
            sockaddr_in address{}; socklen_t length = sizeof(address);
            int fd = zsock_accept(state.listener, reinterpret_cast<sockaddr *>(&address), &length);
            if (fd >= 0) {
                bool accepted = false;
                for (Connection &connection : state.connections) if (connection.fd == invalid_fd) { connection.fd = fd; accepted = true; break; }
                if (!accepted) (void)zsock_close(fd);
            }
        }
        for (Connection &connection : state.connections) {
            if (connection.fd == invalid_fd) continue;
            if (connection.response_stage != 0 && k_uptime_get() >= connection.response_at) {
                if (connection.response_stage == 4) { Close(connection); continue; }
                if (connection.response_stage == 1) {
                    Reply(connection, 180, "Ringing", nullptr); atomic_inc(&state.ringing);
                    connection.response_stage = 2; connection.response_at = k_uptime_get() + 50;
                } else if (connection.response_stage == 2) {
                    if (connection.script == SipPeerScript::no_final) {
                        connection.response_stage = 0;
                        continue;
                    }
                    Reply(connection, 200, "OK", Sdp(connection.script != SipPeerScript::malformed_sdp));
                    connection.response_stage = 0;
                } else {
                    char invite[message_bytes]{};
                    const char *sdp = Sdp(true);
                    std::snprintf(invite, sizeof(invite), "INVITE sip:agent0@127.0.0.1:5097;transport=tcp SIP/2.0\r\nVia: SIP/2.0/TCP 127.0.0.1;branch=z9hG4bK-peer-reinvite\r\nFrom: %s\r\nTo: %s\r\nCall-ID: %s\r\nCSeq: 2 INVITE\r\nContact: <sip:remote-b@127.0.0.1:5098;transport=tcp>\r\nContent-Type: application/sdp\r\nContent-Length: %u\r\n\r\n%s", connection.from, connection.to, connection.call_id, static_cast<unsigned>(std::strlen(sdp)), sdp);
                    Send(connection, invite); atomic_inc(&state.reinvites); connection.response_stage = 0;
                }
            }
            const unsigned available = sizeof(connection.buffer) - 1U - connection.used;
            if (available == 0) { atomic_inc(&state.parse_failures); Close(connection); continue; }
            int received = zsock_recv(connection.fd, connection.buffer + connection.used, available, ZSOCK_MSG_DONTWAIT);
            if (received > 0) { connection.used += static_cast<unsigned>(received); Consume(connection); }
            else if (received == 0) Close(connection);
        }
    }
}
void PeerEntry(void *, void *, void *) {
    state.listener = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(ScriptedSipPeer::listener_port); address.sin_addr.s_addr = htonl(0x7f000001U);
    if (state.listener < 0 || zsock_bind(state.listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 || zsock_listen(state.listener, 3) != 0) {
        if (state.listener >= 0) (void)zsock_close(state.listener);
        atomic_set(&state.ready, -1); return;
    }
    atomic_set(&state.ready, 1);
    printk("[peer] ready priority=%d listener=%d\n", k_thread_priority_get(k_current_get()), state.listener);
    while (atomic_get(&state.running) != 0) {
        PumpPeer();
        std::size_t unused{};
        if (k_thread_stack_space_get(k_current_get(), &unused) == 0 &&
            unused < static_cast<std::size_t>(atomic_get(&state.minimum_stack_unused)))
            atomic_set(&state.minimum_stack_unused, static_cast<atomic_val_t>(unused));
        k_sleep(K_MSEC(1));
    }
}
} // namespace

namespace voip_integration {
ScriptedSipPeer &ScriptedSipPeer::Instance() noexcept { static ScriptedSipPeer peer; return peer; }
bool ScriptedSipPeer::Start() noexcept {
    if (atomic_cas(&state.running, 0, 1))
        (void)k_thread_create(&state.thread, peer_stack, K_THREAD_STACK_SIZEOF(peer_stack),
                             PeerEntry, nullptr, nullptr, nullptr, K_PRIO_PREEMPT(5), 0, K_NO_WAIT);
    const int64_t deadline = k_uptime_get() + 1000;
    while (atomic_get(&state.ready) == 0 && k_uptime_get() < deadline) k_sleep(K_MSEC(1));
    return atomic_get(&state.ready) == 1;
}
void ScriptedSipPeer::SetNextOutgoing(SipPeerScript script) noexcept { atomic_set(&state.next_outgoing, static_cast<atomic_val_t>(script)); }
bool ScriptedSipPeer::StartIncoming(SipPeerScript script, unsigned agent) noexcept {
    return agent < 5 && atomic_cas(&state.incoming_requested, 0,
        static_cast<atomic_val_t>((agent << 8) | (static_cast<unsigned>(script) + 1)));
}
SipPeerStats ScriptedSipPeer::Stats() const noexcept {
    return {static_cast<unsigned>(atomic_get(&state.invites)), static_cast<unsigned>(atomic_get(&state.incoming_invites)),
        static_cast<unsigned>(atomic_get(&state.acks)), static_cast<unsigned>(atomic_get(&state.byes)),
        static_cast<unsigned>(atomic_get(&state.cancels)), static_cast<unsigned>(atomic_get(&state.trying)),
        static_cast<unsigned>(atomic_get(&state.ringing)), static_cast<unsigned>(atomic_get(&state.reinvites)),
        static_cast<unsigned>(atomic_get(&state.reinvite_responses)), static_cast<unsigned>(atomic_get(&state.reinvite_acks)),
        static_cast<unsigned>(atomic_get(&state.registrations)), static_cast<unsigned>(atomic_get(&state.incoming_trying)),
        static_cast<unsigned>(atomic_get(&state.incoming_ringing)), static_cast<unsigned>(atomic_get(&state.cancel_responses)),
        static_cast<unsigned>(atomic_get(&state.cancelled_invites)), static_cast<unsigned>(atomic_get(&state.minimum_stack_unused)),
        static_cast<unsigned>(atomic_get(&state.parse_failures))};
}
unsigned ScriptedSipPeer::IncomingResponseOrder(unsigned sequence, unsigned code) const noexcept {
    if (sequence >= max_observed_calls) return 0;
    atomic_t *orders = code == 180 ? state.incoming_180_order :
        code == 200 ? state.incoming_200_order :
        code == 480 ? state.incoming_480_order :
        code == 486 ? state.incoming_486_order : nullptr;
    return orders == nullptr ? 0 : static_cast<unsigned>(atomic_get(&orders[sequence]));
}
unsigned ScriptedSipPeer::OutgoingInviteOrder(unsigned sequence) const noexcept {
    return sequence < max_observed_calls ? static_cast<unsigned>(atomic_get(&state.outgoing_invite_order[sequence])) : 0;
}
unsigned ScriptedSipPeer::IncomingPort(unsigned sequence) const noexcept {
    return sequence < max_observed_calls ? static_cast<unsigned>(atomic_get(&state.incoming_port[sequence])) : 0;
}
bool ScriptedSipPeer::ZeroWorkerConfiguration() noexcept { return atomic_get(&state.zero_workers) == 1; }
bool ScriptedSipPeer::FramingChecks() noexcept {
    struct Case { const char *value; bool valid; unsigned length; };
    const Case cases[] = {{" 0", true, 0}, {"\t12 ", true, 12}, {"1535", true, 1535},
        {"1536", false, 0}, {"4294967296", false, 0}, {"99999999999999999999", false, 0},
        {"-1", false, 0}, {"+1", false, 0}, {"1x", false, 0}, {"1 2", false, 0}, {"", false, 0}};
    for (const auto &test : cases) {
        char message[128]{};
        std::snprintf(message, sizeof(message), "SIP/2.0 200 OK\r\nContent-Length: %s\r\n\r\n", test.value);
        unsigned length{};
        const bool valid = BodyLength(message, std::strstr(message, "\r\n\r\n"), &length);
        if (valid != test.valid || (valid && length != test.length)) return false;
    }
    // A numerically valid length must still fit alongside the headers.
    Connection oversized{};
    std::snprintf(oversized.buffer, sizeof(oversized.buffer), "SIP/2.0 200 OK\r\nContent-Length: 1535\r\n\r\n");
    unsigned length{};
    const char *end = std::strstr(oversized.buffer, "\r\n\r\n");
    return !FrameSize(oversized.buffer, end, &length);
}
} // namespace voip_integration

extern "C" pj_status_t __real_pjsua_init(const pjsua_config *, const pjsua_logging_config *, const pjsua_media_config *);
namespace {
pjsua_med_tp_state_cb native_media_transport_callback;
pj_status_t ObserveMediaTransport(pjsua_call_id call, const pjsua_med_tp_state_info *info) {
    if (info != nullptr && info->state == PJSUA_MED_TP_CREATING)
        atomic_inc(&state.media_transport_creations);
    if (native_media_transport_callback != nullptr)
        return native_media_transport_callback(call, info);
    return PJ_SUCCESS;
}
}
unsigned voip_integration::ScriptedSipPeer::MediaTransportCreations() const noexcept {
    return static_cast<unsigned>(atomic_get(&state.media_transport_creations));
}
extern "C" pj_status_t __wrap_pjsua_init(const pjsua_config *ua, const pjsua_logging_config *log, const pjsua_media_config *media) {
    atomic_set(&state.zero_workers, ua && media && ua->thread_cnt == 0 && media->thread_cnt == 0 && !media->has_ioqueue ? 1 : -1);
    if (ua == nullptr) return __real_pjsua_init(ua, log, media);
    pjsua_config observed = *ua;
    native_media_transport_callback = ua->cb.on_call_media_transport_state;
    observed.cb.on_call_media_transport_state = ObserveMediaTransport;
    return __real_pjsua_init(&observed, log, media);
}

extern "C" unsigned voip_pjsua_registration_test_listener_port() noexcept { return ScriptedSipPeer::pjsua_listener_port; }
extern "C" pj_status_t voip_pjsua_registration_test_install(pjsip_endpoint *) noexcept { return PJ_SUCCESS; }
