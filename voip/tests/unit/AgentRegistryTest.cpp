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
    char equal_identity_a[] = "sip:equal@example.test";
    char equal_identity_b[] = "sip:equal@example.test";
    voip::AgentConfig agents[2] = {fixtures.agent,
                                   {{"sip:b@example.test", "sip:example.test",
                                     "b", "secret"},
                                    {&second_source, &second_sink}, true}};
    voip::ServiceConfig service = fixtures.service;
    service.agents = agents;
    service.agent_count = 2;
    voip::AgentRegistry registry;
    agents[0].sip.identity_uri = equal_identity_a;
    agents[1].sip.identity_uri = equal_identity_b;
    assert(registry.Initialize(service) == voip::Error::invalid_argument);
    assert(registry.Count() == 0);
    agents[1].sip.identity_uri = "sip:b@example.test";
    agents[1].audio.source = agents[0].audio.source;
    assert(registry.Initialize(service) == voip::Error::invalid_argument);
    agents[1].audio.source = &second_source;
    agents[1].audio.sink = agents[0].audio.sink;
    assert(registry.Initialize(service) == voip::Error::invalid_argument);
}

void test_accepts_every_agent_count_and_maps_each_index() {
    Source sources[5] = {Source{kFormat}, Source{kFormat}, Source{kFormat},
                         Source{kFormat}, Source{kFormat}};
    Sink sinks[5] = {Sink{kFormat}, Sink{kFormat}, Sink{kFormat}, Sink{kFormat},
                     Sink{kFormat}};
    const char *identities[5] = {"sip:zero@example.test", "sip:one@example.test",
                                 "sip:two@example.test", "sip:three@example.test",
                                 "sip:four@example.test"};
    voip::AgentConfig agents[5]{};
    for (std::size_t index = 0; index < 5; ++index) {
        agents[index] = {{identities[index], "sip:example.test", "user",
                          "password"},
                         {&sources[index], &sinks[index]},
                         (index % 2) == 0};
    }
    const voip::ServiceConfig service_template{
        agents, 1, 1000, 5000, kFormat,
        {voip::SignalingSecurity::none, voip::MediaSecurity::none}};
    voip::AgentRegistry registry;
    for (std::uint8_t count = 1; count <= 5; ++count) {
        voip::ServiceConfig service = service_template;
        service.agent_count = count;
        assert(registry.Initialize(service) == voip::Error::ok);
        assert(registry.Count() == count);
        for (std::uint8_t index = 0; index < count; ++index) {
            voip::AgentHandle handle{};
            assert(registry.GetAgentHandle(index, &handle) == voip::Error::ok);
            const voip::AgentContext *context = registry.Resolve(handle);
            assert(context != nullptr);
            assert(context->handle.slot == index);
            assert(std::strcmp(context->sip.identity_uri, identities[index]) == 0);
            assert(context->audio.source == &sources[index]);
            assert(context->audio.sink == &sinks[index]);
            const voip::RegistrationState expected =
                agents[index].register_on_start
                    ? voip::RegistrationState::registering
                    : voip::RegistrationState::disabled;
            assert(context->registration == expected);
        }
    }
}

void test_accepts_exact_maximum_for_each_sip_field() {
    Fixtures fixtures;
    char identity[voip::max_uri_length + 1];
    char registrar[voip::max_uri_length + 1];
    char username[voip::max_username_length + 1];
    char password[voip::max_password_length + 1];
    std::memset(identity, 'i', voip::max_uri_length);
    std::memset(registrar, 'r', voip::max_uri_length);
    std::memset(username, 'u', voip::max_username_length);
    std::memset(password, 'p', voip::max_password_length);
    identity[voip::max_uri_length] = '\0';
    registrar[voip::max_uri_length] = '\0';
    username[voip::max_username_length] = '\0';
    password[voip::max_password_length] = '\0';
    fixtures.agent.sip = {identity, registrar, username, password};
    voip::AgentRegistry registry;
    assert(registry.Initialize(fixtures.service) == voip::Error::ok);
    voip::AgentHandle handle{};
    assert(registry.GetAgentHandle(0, &handle) == voip::Error::ok);
    const voip::AgentContext *context = registry.Resolve(handle);
    assert(context != nullptr);
    assert(std::strlen(context->sip.identity_uri) == voip::max_uri_length);
    assert(std::strlen(context->sip.registrar_uri) == voip::max_uri_length);
    assert(std::strlen(context->sip.auth_username) == voip::max_username_length);
    assert(std::strlen(context->sip.auth_password) == voip::max_password_length);
}

void test_rejects_empty_and_maximum_plus_one_for_each_sip_field() {
    Fixtures fixtures;
    voip::AgentRegistry registry;
    const char *valid[] = {"sip:a@example.test", "sip:example.test", "user",
                           "password"};
    const char *voip::SipAccountConfig::*fields[] = {
        &voip::SipAccountConfig::identity_uri,
        &voip::SipAccountConfig::registrar_uri,
        &voip::SipAccountConfig::auth_username,
        &voip::SipAccountConfig::auth_password};
    for (std::size_t field = 0; field < 4; ++field) {
        fixtures.agent.sip.identity_uri = valid[0];
        fixtures.agent.sip.registrar_uri = valid[1];
        fixtures.agent.sip.auth_username = valid[2];
        fixtures.agent.sip.auth_password = valid[3];
        fixtures.agent.sip.*fields[field] = "";
        assert(registry.Initialize(fixtures.service) == voip::Error::invalid_argument);
    }

    char too_long_uri[voip::max_uri_length + 2];
    char too_long_username[voip::max_username_length + 2];
    char too_long_password[voip::max_password_length + 2];
    std::memset(too_long_uri, 'i', voip::max_uri_length + 1);
    std::memset(too_long_username, 'u', voip::max_username_length + 1);
    std::memset(too_long_password, 'p', voip::max_password_length + 1);
    too_long_uri[voip::max_uri_length + 1] = '\0';
    too_long_username[voip::max_username_length + 1] = '\0';
    too_long_password[voip::max_password_length + 1] = '\0';
    const char *oversized[] = {too_long_uri, too_long_uri, too_long_username,
                               too_long_password};
    for (std::size_t field = 0; field < 4; ++field) {
        fixtures.agent.sip.identity_uri = valid[0];
        fixtures.agent.sip.registrar_uri = valid[1];
        fixtures.agent.sip.auth_username = valid[2];
        fixtures.agent.sip.auth_password = valid[3];
        fixtures.agent.sip.*fields[field] = oversized[field];
        assert(registry.Initialize(fixtures.service) == voip::Error::invalid_argument);
    }
}

void test_copies_all_four_sip_fields_independently() {
    Fixtures fixtures;
    char identity[voip::max_uri_length + 1] = "sip:owned@example.test";
    char registrar[voip::max_uri_length + 1] = "sip:owned.test";
    char username[voip::max_username_length + 1] = "owned-user";
    char password[voip::max_password_length + 1] = "owned-password";
    fixtures.agent.sip = {identity, registrar, username, password};
    voip::AgentRegistry registry;
    assert(registry.Initialize(fixtures.service) == voip::Error::ok);
    voip::AgentHandle handle{};
    assert(registry.GetAgentHandle(0, &handle) == voip::Error::ok);
    const voip::AgentContext *context = registry.Resolve(handle);
    assert(context != nullptr);
    std::strcpy(identity, "changed-identity");
    std::strcpy(registrar, "changed-registrar");
    std::strcpy(username, "changed-user");
    std::strcpy(password, "changed-password");
    assert(std::strcmp(context->sip.identity_uri, "sip:owned@example.test") == 0);
    assert(std::strcmp(context->sip.registrar_uri, "sip:owned.test") == 0);
    assert(std::strcmp(context->sip.auth_username, "owned-user") == 0);
    assert(std::strcmp(context->sip.auth_password, "owned-password") == 0);
}

void test_owned_credentials_can_be_erased_explicitly() {
    voip::OwnedSipAccountConfig owned{};
    std::strcpy(owned.auth_username, "secret-user");
    std::strcpy(owned.auth_password, "secret-password");
    owned.EraseCredentials();
    for (char value : owned.auth_username) assert(value == '\0');
    for (char value : owned.auth_password) assert(value == '\0');
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
    test_accepts_every_agent_count_and_maps_each_index();
    test_accepts_exact_maximum_for_each_sip_field();
    test_rejects_empty_and_maximum_plus_one_for_each_sip_field();
    test_copies_all_four_sip_fields_independently();
    test_owned_credentials_can_be_erased_explicitly();
    test_rejects_null_endpoint_bad_format_and_security();
    test_copies_strings_and_maps_configuration_indexes_to_handles();
    test_failed_reinitialize_resets_and_invalidates_old_handle();
    test_rejects_more_than_five_agents();
    std::puts("AgentRegistryTest PASSED");
    return 0;
}
