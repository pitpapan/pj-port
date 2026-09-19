#include "FakePjsuaApi.hpp"
#include "../../src/core/AgentRegistry.hpp"
#include "../../src/pjsua/PjsuaAccountManager.hpp"
#include "../../src/pjsua/PjsuaCallManager.hpp"

#include <cassert>
#include <zephyr/sys/printk.h>

namespace voip::test {
namespace {
class Source final : public PcmSource { public: PcmFormat Format() const noexcept override { return {8000, 160, 1, SampleFormat::signed_16}; } Error Read(std::int16_t *, std::size_t, std::uint64_t) noexcept override { return Error::ok; } };
class Sink final : public PcmSink { public: PcmFormat Format() const noexcept override { return {8000, 160, 1, SampleFormat::signed_16}; } Error Write(const std::int16_t *, std::size_t, std::uint64_t) noexcept override { return Error::ok; } void Flush() noexcept override {} };
struct Fixture {
    Source source{}; Sink sink{};
    AgentConfig agent{{"sip:outgoing@example.test", "sip:registrar.example.test", "a", "p"}, {&source, &sink}, true};
    ServiceConfig config{&agent, 1, 1000, 5000, {8000, 160, 1, SampleFormat::signed_16}, {SignalingSecurity::none, MediaSecurity::none}};
    AgentRegistry registry{};
    Fixture() noexcept { assert(registry.Initialize(config) == Error::ok); }
};
}

void RunPjsuaOutgoingPromotionTests() {
    Fixture fixture; FakePjsuaApi fake; fake.Reset();
    PjsuaAccountManager accounts(fake.Api());
    assert(accounts.Initialize(fixture.registry, 4) == Error::ok);
    PjsuaCallManager calls(accounts, fake.Api());
    AgentHandle agent{}; assert(fixture.registry.GetAgentHandle(0, &agent) == Error::ok);
    std::uint32_t token = 0;

    // Removing account resolution would allocate a native call for an
    // unregistered logical agent instead of rejecting it before allocation.
    assert(calls.PromoteOutgoing({0, 99}, {0, 99}, "sip:invalid@example.test", &token) == Error::invalid_handle);
    assert(fake.ActiveCallCount() == 0 && calls.Count() == 0);

    // Replacing promotion-time allocation with Dial-time allocation makes
    // either the seventh call fail early or the eighth call succeed.
    for (std::uint16_t i = 0; i < 7; ++i) {
        token = 0;
        assert(calls.PromoteOutgoing({static_cast<std::uint8_t>(i), 1}, agent, "sip:remote@example.test", &token) == Error::ok);
        assert(token != 0);
    }
    assert(fake.ActiveCallCount() == 7 && calls.Count() == 7);
    assert(calls.PromoteOutgoing({7, 1}, agent, "sip:eighth@example.test", &token) == Error::resource_exhausted);
    assert(fake.ActiveCallCount() == 7 && calls.Count() == 7);
    printk("PjsuaOutgoingPromotionTest PASSED\n");
}
} // namespace voip::test
