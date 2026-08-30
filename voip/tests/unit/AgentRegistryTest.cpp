#include "../../src/core/AgentContext.hpp"
#include "../../src/core/AgentRegistry.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>

namespace {

class Source final : public voip::PcmSource {
public:
    explicit Source(voip::PcmFormat format) noexcept : format_(format) {}
    voip::PcmFormat Format() const noexcept override { return format_; }
    voip::Error Read(std::int16_t *, std::size_t,
                     std::uint64_t) noexcept override {
        return voip::Error::ok;
    }

private:
    voip::PcmFormat format_;
};

class Sink final : public voip::PcmSink {
public:
    explicit Sink(voip::PcmFormat format) noexcept : format_(format) {}
    voip::PcmFormat Format() const noexcept override { return format_; }
    voip::Error Write(const std::int16_t *, std::size_t,
                      std::uint64_t) noexcept override {
        return voip::Error::ok;
    }
    void Flush() noexcept override {}

private:
    voip::PcmFormat format_;
};

const voip::PcmFormat kFormat{8000, 160, 1,
                              voip::SampleFormat::signed_16};

struct Fixtures {
    Source source{kFormat};
    Sink sink{kFormat};
    voip::AgentConfig agent{{"sip:a@example.test", "sip:example.test", "a",
                             "secret"},
                            {&source, &sink}, true};
    voip::ServiceConfig service{&agent, 1, 1000, 5000, kFormat,
                                {voip::SignalingSecurity::none,
                                 voip::MediaSecurity::none}};
};

void test_rejects_empty_or_unbounded_configuration() {
    Fixtures fixtures;
    voip::AgentRegistry registry;
    fixtures.service.agent_count = 0;
    assert(registry.Initialize(fixtures.service) == voip::Error::invalid_argument);
    assert(registry.Count() == 0);

    fixtures.service.agent_count = 1;
    fixtures.service.agents = nullptr;
    assert(registry.Initialize(fixtures.service) == voip::Error::invalid_argument);
    assert(registry.Count() == 0);

    fixtures.service.agents = &fixtures.agent;
    fixtures.agent.sip.identity_uri = nullptr;
    assert(registry.Initialize(fixtures.service) == voip::Error::invalid_argument);
    fixtures.agent.sip.identity_uri = "sip:a@example.test";
    fixtures.agent.sip.registrar_uri = nullptr;
    assert(registry.Initialize(fixtures.service) == voip::Error::invalid_argument);
    fixtures.agent.sip.registrar_uri = "sip:example.test";
    fixtures.agent.sip.auth_username = nullptr;
    assert(registry.Initialize(fixtures.service) == voip::Error::invalid_argument);
    fixtures.agent.sip.auth_username = "a";
    fixtures.agent.sip.auth_password = nullptr;
    assert(registry.Initialize(fixtures.service) == voip::Error::invalid_argument);
    fixtures.agent.sip.auth_password = "secret";

    char oversized[voip::max_uri_length + 2]{};
    std::memset(oversized, 'x', sizeof(oversized) - 1);
    fixtures.agent.sip.identity_uri = oversized;
    assert(registry.Initialize(fixtures.service) == voip::Error::invalid_argument);
    fixtures.agent.sip.identity_uri = "sip:a@example.test";

    char oversized_password[voip::max_password_length + 2]{};
    std::memset(oversized_password, 'x', sizeof(oversized_password) - 1);
    fixtures.agent.sip.auth_password = oversized_password;
    assert(registry.Initialize(fixtures.service) == voip::Error::invalid_argument);
}

void test_rejects_duplicate_identity_and_audio_bindings() {
    Fixtures fixtures;
    Source second_source{kFormat};
    Sink second_sink{kFormat};
    voip::AgentConfig agents[2] = {fixtures.agent,
                                   {{"sip:b@example.test", "sip:example.test",
                                     "b", "secret"},
                                    {&second_source, &second_sink}, true}};
    voip::ServiceConfig service = fixtures.service;
    service.agents = agents;
    service.agent_count = 2;
    voip::AgentRegistry registry;
    agents[1].sip.identity_uri = agents[0].sip.identity_uri;
    assert(registry.Initialize(service) == voip::Error::invalid_argument);
    assert(registry.Count() == 0);
    agents[1].sip.identity_uri = "sip:b@example.test";
    agents[1].audio.source = agents[0].audio.source;
    assert(registry.Initialize(service) == voip::Error::invalid_argument);
    agents[1].audio.source = &second_source;
    agents[1].audio.sink = agents[0].audio.sink;
    assert(registry.Initialize(service) == voip::Error::invalid_argument);
}

void test_rejects_null_endpoint_bad_format_and_security() {
    Fixtures fixtures;
    voip::AgentRegistry registry;
    fixtures.agent.audio.source = nullptr;
    assert(registry.Initialize(fixtures.service) == voip::Error::invalid_argument);
    fixtures.agent.audio.source = &fixtures.source;
    fixtures.agent.audio.sink = nullptr;
    assert(registry.Initialize(fixtures.service) == voip::Error::invalid_argument);
    fixtures.agent.audio.sink = &fixtures.sink;

    Source bad_source({8000, 160, 2, voip::SampleFormat::signed_16});
    fixtures.agent.audio.source = &bad_source;
    assert(registry.Initialize(fixtures.service) == voip::Error::invalid_argument);
    fixtures.agent.audio.source = &fixtures.source;

    Source mismatched_source({16000, 320, 1, voip::SampleFormat::signed_16});
    fixtures.agent.audio.source = &mismatched_source;
    assert(registry.Initialize(fixtures.service) == voip::Error::invalid_argument);
    fixtures.agent.audio.source = &fixtures.source;

    fixtures.service.conference_format = {8000, 160, 2,
                                          voip::SampleFormat::signed_16};
    assert(registry.Initialize(fixtures.service) == voip::Error::invalid_argument);
    fixtures.service.conference_format = kFormat;

    fixtures.service.security.signaling = voip::SignalingSecurity::tls;
    assert(registry.Initialize(fixtures.service) ==
           voip::Error::unsupported_configuration);
    fixtures.service.security.signaling = voip::SignalingSecurity::none;
    fixtures.service.security.media = voip::MediaSecurity::srtp_sdes;
    assert(registry.Initialize(fixtures.service) ==
           voip::Error::unsupported_configuration);
}

void test_copies_strings_and_maps_configuration_indexes_to_handles() {
    Fixtures fixtures;
    char caller_identity[] = "sip:a@example.test";
    fixtures.agent.sip.identity_uri = caller_identity;
    voip::AgentRegistry registry;
    assert(registry.Initialize(fixtures.service) == voip::Error::ok);
    voip::AgentHandle handle{};
    assert(registry.GetAgentHandle(0, &handle) == voip::Error::ok);
    const voip::AgentContext *context = registry.Resolve(handle);
    assert(context != nullptr);
    assert(std::strcmp(context->sip.identity_uri, "sip:a@example.test") == 0);
    assert(std::strcmp(context->sip.auth_password, "secret") == 0);

    std::strcpy(caller_identity, "bad");
    assert(std::strcmp(context->sip.identity_uri, "sip:a@example.test") == 0);
    voip::AgentHandle invalid{};
    assert(registry.GetAgentHandle(1, &invalid) == voip::Error::invalid_argument);
    assert(registry.GetAgentHandle(0, nullptr) == voip::Error::invalid_argument);
}

void test_failed_reinitialize_resets_and_invalidates_old_handle() {
    Fixtures fixtures;
    voip::AgentRegistry registry;
    assert(registry.Initialize(fixtures.service) == voip::Error::ok);
    voip::AgentHandle old_handle{};
    assert(registry.GetAgentHandle(0, &old_handle) == voip::Error::ok);
    fixtures.agent.sip.auth_password = nullptr;
    assert(registry.Initialize(fixtures.service) == voip::Error::invalid_argument);
    assert(registry.Count() == 0);
    assert(registry.Resolve(old_handle) == nullptr);
    fixtures.agent.sip.auth_password = "secret";
    assert(registry.Initialize(fixtures.service) == voip::Error::ok);
    voip::AgentHandle replacement{};
    assert(registry.GetAgentHandle(0, &replacement) == voip::Error::ok);
    assert(replacement.generation != old_handle.generation);
}

void test_rejects_more_than_five_agents() {
    Fixtures fixtures;
    voip::AgentConfig agents[6] = {fixtures.agent, fixtures.agent, fixtures.agent,
                                   fixtures.agent, fixtures.agent, fixtures.agent};
    for (unsigned index = 0; index < 6; ++index) {
        agents[index].sip.identity_uri = index == 0 ? "sip:a@example.test"
                                                    : "sip:b@example.test";
    }
    fixtures.service.agents = agents;
    fixtures.service.agent_count = 6;
    voip::AgentRegistry registry;
    assert(registry.Initialize(fixtures.service) == voip::Error::invalid_argument);
    assert(registry.Count() == 0);
}

} // namespace

int main() {
    test_rejects_empty_or_unbounded_configuration();
    test_rejects_duplicate_identity_and_audio_bindings();
    test_rejects_null_endpoint_bad_format_and_security();
    test_copies_strings_and_maps_configuration_indexes_to_handles();
    test_failed_reinitialize_resets_and_invalidates_old_handle();
    test_rejects_more_than_five_agents();
    std::puts("AgentRegistryTest PASSED");
    return 0;
}
