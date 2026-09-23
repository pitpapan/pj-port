#include "../test_support/ScriptedSipPeer.hpp"
#include <voip/VoipService.hpp>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <cstdio>
#include <cstring>

namespace {
constexpr voip::PcmFormat format{8000, 80, 1, voip::SampleFormat::signed_16};
int failures;
#define CHECK(x) do { if (!(x)) { printk("[two-call] FAIL %d: %s\n", __LINE__, #x); ++failures; } } while (false)
class Source final : public voip::PcmSource { public:
    voip::PcmFormat Format() const noexcept override { return format; }
    voip::Error Read(std::int16_t *samples, std::size_t count, std::uint64_t) noexcept override {
        std::memset(samples, 0, count * sizeof(*samples)); return voip::Error::ok;
    }
};
class Sink final : public voip::PcmSink { public:
    voip::PcmFormat Format() const noexcept override { return format; }
    voip::Error Write(const std::int16_t *, std::size_t, std::uint64_t) noexcept override { return voip::Error::ok; }
    void Flush() noexcept override {}
};
bool Same(voip::CallHandle a, voip::CallHandle b) { return a.slot == b.slot && a.generation == b.generation; }
struct CallEvidence {
    voip::CallHandle handle{}; voip::AgentHandle agent{}; char remote[256]{};
    unsigned established{}, terminals{}; voip::Error error{}; unsigned sip{};
};
struct Seen {
    CallEvidence calls[16]{}; unsigned count{}, incoming{}, accepted{}, finished{};
    bool registered[5]{}; voip::CallHandle last_incoming{};
};
void Record(const voip::Event &event, Seen &seen) {
    if (event.type == voip::EventType::agent_snapshot && event.agent.slot < 5)
        seen.registered[event.agent.slot] = event.agent_snapshot.registration == voip::RegistrationState::registered;
    if (event.type == voip::EventType::incoming_call) { ++seen.incoming; seen.last_incoming = event.call; }
    if (event.type != voip::EventType::call_state) return;
    CHECK(event.call.IsValid()); CHECK(Same(event.call, event.call_snapshot.handle));
    CallEvidence *call = nullptr;
    for (unsigned i = 0; i < seen.count; ++i) if (Same(seen.calls[i].handle, event.call)) call = &seen.calls[i];
    if (!call) {
        CHECK(seen.count < 16); if (seen.count == 16) return;
        call = &seen.calls[seen.count++]; call->handle = event.call;
        std::snprintf(call->remote, sizeof(call->remote), "%s", event.call_snapshot.remote_uri);
    }
    CHECK(std::strcmp(call->remote, event.call_snapshot.remote_uri) == 0);
    if (event.destination_state == voip::CallState::established) {
        call->agent = event.call_snapshot.agent;
        if (call->established++ == 0) ++seen.accepted;
    }
    if (event.destination_state == voip::CallState::terminated) {
        ++call->terminals; ++seen.finished; call->error = event.status.error; call->sip = event.status.sip_status;
        printk("[two-call] terminal call=%u/%u remote=%s error=%u sip=%u reason=%s\n",
               event.call.slot, event.call.generation, call->remote, static_cast<unsigned>(call->error), call->sip, event.status.reason);
    }
}
void Poll(voip::VoipService &service, Seen &seen) {
    voip::Event event{}; if (service.WaitForEvent(&event, 5) == voip::Error::ok) Record(event, seen);
    k_sleep(K_MSEC(5));
}
bool Wait(voip::VoipService &service, Seen &seen, const unsigned &value, unsigned want) {
    const int64_t deadline = k_uptime_get() + 3000;
    while (k_uptime_get() < deadline && value < want) Poll(service, seen);
    return value >= want;
}
void Drain(voip::VoipService &service, Seen &seen) {
    const int64_t deadline = k_uptime_get() + 600;
    while (k_uptime_get() < deadline) Poll(service, seen);
}
bool Registered(const Seen &seen) { for (bool value : seen.registered) if (!value) return false; return true; }
CallEvidence *Find(Seen &seen, const char *remote) {
    for (unsigned i = 0; i < seen.count; ++i) if (std::strcmp(seen.calls[i].remote, remote) == 0) return &seen.calls[i];
    return nullptr;
}
void FreeLeases(voip::VoipService &service) {
    const auto r = service.GetResourceSnapshot();
    CHECK(r.promoted_calls == 0); CHECK(r.active_calls == 0);
    CHECK(r.available_promoted_calls == 2); CHECK(r.available_media_bridges == 2);
}
}
int main() {
    using voip_integration::ScriptedSipPeer; using voip_integration::SipPeerScript;
    auto &peer = ScriptedSipPeer::Instance(); CHECK(peer.FramingChecks()); CHECK(peer.Start());
    printk("[two-call] framing numeric/overflow/body-bound checks complete\n");
    Source sources[5]; Sink sinks[5]; char identities[5][64]{}; voip::AgentConfig agents[5]{};
    for (unsigned i = 0; i < 5; ++i) {
        std::snprintf(identities[i], sizeof(identities[i]), "<sip:agent%u@127.0.0.1>", i);
        agents[i] = {{identities[i], "sip:127.0.0.1:5098;transport=tcp", "agent", "password"}, {&sources[i], &sinks[i]}, true};
    }
    static voip::VoipService service;
    CHECK(service.Initialize({agents, 5, 1000, 1000, format, {voip::SignalingSecurity::none, voip::MediaSecurity::none}}) == voip::Error::ok);
    voip::AgentHandle a{}, b{}; CHECK(service.GetAgentHandle(0, &a) == voip::Error::ok); CHECK(service.GetAgentHandle(1, &b) == voip::Error::ok);
    Seen seen{}; voip::OperationId operation{};
    const int64_t deadline = k_uptime_get() + 3000;
    while (k_uptime_get() < deadline && !Registered(seen)) Poll(service, seen);
    CHECK(Registered(seen)); CHECK(peer.Stats().registrations == 5); CHECK(peer.ZeroWorkerConfiguration());
    printk("[two-call] pjsua_init observed SIP workers=0 media workers=0 media ioqueue=0\n");
    CHECK(service.Dial(b, {"sip:remote-a@127.0.0.1:5098;transport=tcp"}, &operation) == voip::Error::ok);
    CHECK(peer.StartIncoming(SipPeerScript::answer)); CHECK(Wait(service, seen, seen.incoming, 1));
    CHECK(service.Answer(seen.last_incoming, &operation) == voip::Error::ok); CHECK(Wait(service, seen, seen.accepted, 2));
    Drain(service, seen);
    CHECK(service.GetResourceSnapshot().promoted_calls == 2); CHECK(seen.count == 2);
    CHECK(seen.calls[0].handle.IsValid()); CHECK(seen.calls[1].handle.IsValid()); CHECK(!Same(seen.calls[0].handle, seen.calls[1].handle));
    CHECK(seen.calls[0].agent.slot != seen.calls[1].agent.slot);
    CHECK(std::strstr(seen.calls[0].remote, "remote-") != nullptr); CHECK(std::strstr(seen.calls[1].remote, "remote-") != nullptr);
    const auto flows = peer.Stats();
    CHECK(flows.invites == 1); CHECK(flows.trying == 1); CHECK(flows.ringing == 1);
    CHECK(flows.incoming_invites == 1); CHECK(flows.incoming_trying >= 1); CHECK(flows.incoming_ringing >= 1);
    CHECK(flows.reinvites == 1); CHECK(flows.reinvite_responses == 1); CHECK(flows.reinvite_acks == 1); CHECK(flows.acks >= 3);
    printk("[two-call] INVITE/100/180/200/ACK and re-INVITE/200/ACK checked\n");
    CHECK(service.Hangup(seen.calls[0].handle, &operation) == voip::Error::ok); CHECK(service.Hangup(seen.calls[1].handle, &operation) == voip::Error::ok);
    CHECK(Wait(service, seen, seen.finished, 2)); Drain(service, seen); FreeLeases(service);
    CHECK(seen.calls[0].terminals == 1); CHECK(seen.calls[1].terminals == 1);
    struct Fault { SipPeerScript script; const char *uri; voip::Error error; unsigned sip; };
    const Fault faults[] = {
        {SipPeerScript::malformed_sdp, "sip:malformed@127.0.0.1:5098;transport=tcp", voip::Error::negotiation_failed, 415},
        {SipPeerScript::reject_486, "sip:busy@127.0.0.1:5098;transport=tcp", voip::Error::remote_rejected, 486},
        {SipPeerScript::close_transport, "sip:eof@127.0.0.1:5098;transport=tcp", voip::Error::signaling_failed, 503},
        {SipPeerScript::bye_after_ack, "sip:bye@127.0.0.1:5098;transport=tcp", voip::Error::ok, 200}};
    for (const auto &fault : faults) {
        const unsigned want = seen.finished + 1; peer.SetNextOutgoing(fault.script);
        CHECK(service.Dial(a, {fault.uri}, &operation) == voip::Error::ok); CHECK(Wait(service, seen, seen.finished, want)); Drain(service, seen);
        const auto *call = Find(seen, fault.uri); CHECK(call != nullptr);
        if (call) { CHECK(call->handle.IsValid()); CHECK(call->terminals == 1); CHECK(call->error == fault.error); CHECK(call->sip == fault.sip); }
        CHECK(seen.finished == want); FreeLeases(service);
    }
    CHECK(peer.StartIncoming(SipPeerScript::cancel_incoming)); CHECK(Wait(service, seen, seen.incoming, 2));
    const auto cancel_handle = seen.last_incoming;
    CHECK(Wait(service, seen, seen.finished, 7)); Drain(service, seen);
    bool cancelled = false;
    for (const auto &call : seen.calls) if (Same(call.handle, cancel_handle)) {
        CHECK(call.terminals == 1); CHECK(call.error == voip::Error::cancelled); CHECK(call.sip == 487); cancelled = true;
    }
    CHECK(cancelled); CHECK(peer.Stats().cancels == 1); CHECK(peer.Stats().cancel_responses == 1); CHECK(peer.Stats().cancelled_invites == 1); FreeLeases(service);
    // Both reservations must support two new simultaneous calls after all faults.
    peer.SetNextOutgoing(SipPeerScript::answer); const unsigned accepted = seen.accepted; const unsigned old_count = seen.count;
    CHECK(service.Dial(b, {"sip:reuse-a@127.0.0.1:5098;transport=tcp"}, &operation) == voip::Error::ok); CHECK(Wait(service, seen, seen.accepted, accepted + 1));
    CHECK(peer.StartIncoming(SipPeerScript::answer)); CHECK(Wait(service, seen, seen.incoming, 3));
    CHECK(service.Answer(seen.last_incoming, &operation) == voip::Error::ok); CHECK(Wait(service, seen, seen.accepted, accepted + 2));
    CHECK(service.GetResourceSnapshot().promoted_calls == 2); CHECK(seen.calls[old_count].agent.slot != seen.calls[old_count + 1].agent.slot);
    CHECK(service.Hangup(seen.calls[old_count].handle, &operation) == voip::Error::ok); CHECK(service.Hangup(seen.calls[old_count + 1].handle, &operation) == voip::Error::ok);
    CHECK(Wait(service, seen, seen.finished, 9)); Drain(service, seen); FreeLeases(service);
    CHECK(peer.Stats().parse_failures == 0); for (unsigned i = 0; i < seen.count; ++i) CHECK(seen.calls[i].terminals == 1);
    CHECK(peer.Stats().minimum_stack_unused >= 512);
    printk("[two-call] peer minimum unused stack=%u bytes\n", peer.Stats().minimum_stack_unused);
    const auto shutdown = service.Shutdown(); CHECK(shutdown == voip::Error::ok || shutdown == voip::Error::busy);
    if (failures == 0) printk("VOIP PJSUA TWO CALL SMOKE: PASSED (2 promoted, 2 agents)\n");
    return failures == 0 ? 0 : 1;
}
