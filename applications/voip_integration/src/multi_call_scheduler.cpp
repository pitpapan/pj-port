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
class Source final : public voip::PcmSource { public: voip::PcmFormat Format() const noexcept override { return format; } voip::Error Read(std::int16_t *, std::size_t, std::uint64_t) noexcept override { return voip::Error::ok; } };
class Sink final : public voip::PcmSink { public: voip::PcmFormat Format() const noexcept override { return format; } voip::Error Write(const std::int16_t *, std::size_t, std::uint64_t) noexcept override { return voip::Error::ok; } void Flush() noexcept override {} };
struct Seen { voip::CallHandle calls[12]{}; voip::AgentHandle agents[12]{}; voip::Error errors[12]{}; char remote[12][256]{}; unsigned incoming{}, accepted{}, finished{}; };
void Record(const voip::Event &event, Seen &seen) {
    if (event.type == voip::EventType::incoming_call) { ++seen.incoming; seen.calls[seen.incoming - 1] = event.call; }
    if (event.type == voip::EventType::call_state && event.destination_state == voip::CallState::established && seen.accepted < 12) { seen.calls[seen.accepted] = event.call; seen.agents[seen.accepted] = event.agent; std::snprintf(seen.remote[seen.accepted], sizeof(seen.remote[0]), "%s", event.call_snapshot.remote_uri); ++seen.accepted; }
    if (event.type == voip::EventType::call_state && event.destination_state == voip::CallState::terminated && seen.finished < 12) seen.errors[seen.finished++] = event.status.error;
}
bool Wait(voip::VoipService &service, Seen &seen, unsigned *value, unsigned want) { for (unsigned ms = 0; ms < 12000 && *value < want; ms += 50) { voip::Event event{}; if (service.WaitForEvent(&event, 50) == voip::Error::ok) Record(event, seen); } return *value >= want; }
bool Has(const Seen &seen, voip::Error error) { for (unsigned i = 0; i < seen.finished; ++i) if (seen.errors[i] == error) return true; return false; }
}
int main() {
    using voip_integration::ScriptedSipPeer; using voip_integration::SipPeerScript;
    auto &peer = ScriptedSipPeer::Instance(); peer.Reset(); CHECK(peer.Start());
    Source sources[5]; Sink sinks[5]; char identities[5][64]{}; voip::AgentConfig agents[5]{};
    for (unsigned i = 0; i < 5; ++i) { std::snprintf(identities[i], sizeof(identities[i]), "<sip:agent%u@127.0.0.1>", i); agents[i] = {{identities[i], "sip:127.0.0.1:5098;transport=tcp", "agent", "password"}, {&sources[i], &sinks[i]}, false}; }
    voip::VoipService service; CHECK(service.Initialize({agents, 5, 1000, 1000, format, {voip::SignalingSecurity::none, voip::MediaSecurity::none}}) == voip::Error::ok);
    voip::AgentHandle a{}, b{}; CHECK(service.GetAgentHandle(0, &a) == voip::Error::ok); CHECK(service.GetAgentHandle(1, &b) == voip::Error::ok);
    Seen seen{}; voip::OperationId operation{};
    CHECK(service.Dial(a, {"sip:remote-a@127.0.0.1:5098;transport=tcp"}, &operation) == voip::Error::ok);
    CHECK(peer.StartIncoming(SipPeerScript::answer)); CHECK(Wait(service, seen, &seen.incoming, 1)); CHECK(service.Answer(seen.calls[0], &operation) == voip::Error::ok); CHECK(Wait(service, seen, &seen.accepted, 2));
    const auto resources = service.GetResourceSnapshot(); CHECK(resources.promoted_calls == 2); CHECK(seen.agents[0].slot != seen.agents[1].slot); CHECK(std::strstr(seen.remote[0], "remote-") != nullptr); CHECK(std::strstr(seen.remote[1], "remote-") != nullptr);
    // Each fault is a separate, reusable promoted lease; outcomes are public events.
    const SipPeerScript fault_scripts[] = {SipPeerScript::malformed_sdp, SipPeerScript::reject_486, SipPeerScript::close_transport, SipPeerScript::bye_after_ack};
    for (SipPeerScript script : fault_scripts) { peer.SetNextOutgoing(script); CHECK(service.Dial(a, {"sip:fault@127.0.0.1:5098;transport=tcp"}, &operation) == voip::Error::ok); (void)Wait(service, seen, &seen.finished, seen.finished + 1); }
    CHECK(peer.StartIncoming(SipPeerScript::cancel_incoming)); CHECK(Wait(service, seen, &seen.incoming, 2)); CHECK(Wait(service, seen, &seen.finished, 5));
    CHECK(Has(seen, voip::Error::negotiation_failed)); CHECK(Has(seen, voip::Error::remote_rejected)); CHECK(Has(seen, voip::Error::signaling_failed)); CHECK(Has(seen, voip::Error::cancelled));
    CHECK(peer.Stats().parse_failures == 0); CHECK(service.GetResourceSnapshot().promoted_calls == 2);
    for (unsigned i = 0; i < seen.accepted; ++i) (void)service.Hangup(seen.calls[i], &operation);
    (void)Wait(service, seen, &seen.finished, 7); CHECK(service.Shutdown() == voip::Error::ok || service.Shutdown() == voip::Error::busy);
    if (failures == 0) printk("VOIP PJSUA TWO CALL SMOKE: PASSED (2 promoted, 2 agents)\n");
    return failures == 0 ? 0 : 1;
}
