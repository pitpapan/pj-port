#include "FakePjsuaApi.hpp"
#include "../../src/core/AgentRegistry.hpp"
#include "../../src/pjsua/PjsuaAccountManager.hpp"
#include "../../src/pjsua/PjsuaCallManager.hpp"

#include <cassert>

namespace voip::test {
namespace {
class Source final : public PcmSource { public: PcmFormat Format() const noexcept override { return {8000, 160, 1, SampleFormat::signed_16}; } Error Read(std::int16_t *, std::size_t, std::uint64_t) noexcept override { return Error::ok; } };
class Sink final : public PcmSink { public: PcmFormat Format() const noexcept override { return {8000, 160, 1, SampleFormat::signed_16}; } Error Write(const std::int16_t *, std::size_t, std::uint64_t) noexcept override { return Error::ok; } void Flush() noexcept override {} };
struct Fixture {
    Source source; Sink sink;
    AgentConfig agent{{"sip:zero@example.test", "sip:registrar.example.test", "user", "password"}, {&source, &sink}, true};
    ServiceConfig config{&agent, 1, 1000, 5000, {8000, 160, 1, SampleFormat::signed_16}, {SignalingSecurity::none, MediaSecurity::none}};
    AgentRegistry registry;
    Fixture() noexcept { assert(registry.Initialize(config) == Error::ok); }
};
}

void RunPjsuaCallControlTests() {
    Fixture fixture; FakePjsuaApi fake; fake.Reset();
    PjsuaAccountManager accounts(fake.Api());
    assert(accounts.Initialize(fixture.registry, 4) == Error::ok);
    PjsuaCallManager calls(accounts, fake.Api());
    AgentHandle agent{}; assert(fixture.registry.GetAgentHandle(0, &agent) == Error::ok);

    CallHandle outgoing{0, 1}; std::uint32_t token = 0;
    assert(calls.PromoteOutgoing(outgoing, agent, "sip:peer@example.test", &token) == Error::ok);
    // Removing the 487 compatibility status must fail this command/state mapping.
    assert(calls.Cancel(token) == Error::ok);
    assert(fake.HangupCount() == 1 && fake.HangupStatus() == 487);
    assert(calls.Hangup(token) == Error::ok);
    assert(fake.HangupCount() == 2 && fake.HangupStatus() == 0);
    assert(calls.SetHeld(token, true) == Error::ok);
    assert(calls.SetHeld(token, false) == Error::ok);

    // Stale tokens and out-of-range SIP reject statuses must make no native call.
    assert(calls.Answer(token + 1) == Error::invalid_handle);
    assert(calls.Reject(token, 399) == Error::invalid_argument);
    assert(calls.Reject(token, 700) == Error::invalid_argument);
    assert(calls.Reject(token, 486) == Error::ok);
    assert(fake.AnswerCount() == 1 && fake.AnswerStatus() == 486);
}
} // namespace voip::test
