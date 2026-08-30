#include "AgentRegistry.hpp"

#include <cstring>

namespace voip {
namespace {

std::size_t BoundedLength(const char *value, std::size_t maximum) noexcept {
    if (value == nullptr) return maximum + 1;
    for (std::size_t index = 0; index <= maximum; ++index) {
        if (value[index] == '\0') return index;
    }
    return maximum + 1;
}

bool SameString(const char *left, std::size_t left_length,
                const char *right, std::size_t right_length) noexcept {
    return left_length == right_length &&
           std::memcmp(left, right, left_length) == 0;
}

} // namespace

void OwnedSipAccountConfig::EraseCredentials() noexcept {
    volatile char *username = auth_username;
    volatile char *password = auth_password;
    for (std::size_t index = 0; index < sizeof(auth_username); ++index)
        username[index] = '\0';
    for (std::size_t index = 0; index < sizeof(auth_password); ++index)
        password[index] = '\0';
}

bool AgentRegistry::ValidateString(const char *value, std::size_t maximum,
                                   std::size_t *length) noexcept {
    if (length == nullptr) return false;
    const std::size_t measured = BoundedLength(value, maximum);
    if (measured == 0 || measured > maximum) return false;
    *length = measured;
    return true;
}

bool AgentRegistry::SameFormat(const PcmFormat &left,
                               const PcmFormat &right) noexcept {
    return left.sample_rate_hz == right.sample_rate_hz &&
           left.samples_per_frame == right.samples_per_frame &&
           left.channels == right.channels &&
           left.sample_format == right.sample_format;
}

bool AgentRegistry::IsSupportedFormat(const PcmFormat &format) noexcept {
    return format.sample_rate_hz != 0 && format.samples_per_frame != 0 &&
           format.channels == 1 &&
           format.sample_format == SampleFormat::signed_16;
}

Error AgentRegistry::Validate(const ServiceConfig &config) noexcept {
    if (config.agent_count == 0 || config.agent_count > max_agents ||
        config.agents == nullptr) {
        return Error::invalid_argument;
    }

    if (config.security.signaling == SignalingSecurity::tls)
        return Error::unsupported_configuration;
    if (config.security.signaling != SignalingSecurity::none)
        return Error::invalid_argument;
    if (config.security.media == MediaSecurity::srtp_sdes)
        return Error::unsupported_configuration;
    if (config.security.media != MediaSecurity::none)
        return Error::invalid_argument;

    if (!IsSupportedFormat(config.conference_format))
        return Error::invalid_argument;

    for (std::size_t index = 0; index < config.agent_count; ++index) {
        const AgentConfig &agent = config.agents[index];
        std::size_t identity_length = 0;
        std::size_t registrar_length = 0;
        std::size_t username_length = 0;
        std::size_t password_length = 0;
        if (!ValidateString(agent.sip.identity_uri, max_uri_length,
                            &identity_length) ||
            !ValidateString(agent.sip.registrar_uri, max_uri_length,
                            &registrar_length) ||
            !ValidateString(agent.sip.auth_username, max_username_length,
                            &username_length) ||
            !ValidateString(agent.sip.auth_password, max_password_length,
                            &password_length)) {
            return Error::invalid_argument;
        }
        (void)registrar_length;
        (void)username_length;
        (void)password_length;

        if (agent.audio.source == nullptr || agent.audio.sink == nullptr)
            return Error::invalid_argument;
        const PcmFormat source_format = agent.audio.source->Format();
        const PcmFormat sink_format = agent.audio.sink->Format();
        if (!IsSupportedFormat(source_format) ||
            !IsSupportedFormat(sink_format) ||
            !SameFormat(source_format, config.conference_format) ||
            !SameFormat(sink_format, config.conference_format)) {
            return Error::invalid_argument;
        }

        for (std::size_t prior = 0; prior < index; ++prior) {
            const AgentConfig &other = config.agents[prior];
            std::size_t other_identity_length = 0;
            if (!ValidateString(other.sip.identity_uri, max_uri_length,
                                &other_identity_length)) {
                return Error::invalid_argument;
            }
            if (SameString(agent.sip.identity_uri, identity_length,
                           other.sip.identity_uri, other_identity_length) ||
                agent.audio.source == other.audio.source ||
                agent.audio.sink == other.audio.sink) {
                return Error::invalid_argument;
            }
        }
    }
    return Error::ok;
}

void AgentRegistry::CopyString(char *destination, const char *source,
                               std::size_t length) noexcept {
    std::memcpy(destination, source, length);
    destination[length] = '\0';
}

Error AgentRegistry::Initialize(const ServiceConfig &config) noexcept {
    const Error validation = Validate(config);
    if (validation != Error::ok) {
        Reset();
        return validation;
    }

    Reset();
    for (std::size_t index = 0; index < config.agent_count; ++index) {
        AgentHandle handle{};
        AgentContext *context = pool_.Allocate(handle);
        if (context == nullptr) {
            Reset();
            return Error::resource_exhausted;
        }
        const AgentConfig &agent = config.agents[index];
        context->handle = handle;
        const std::size_t identity_length =
            BoundedLength(agent.sip.identity_uri, max_uri_length);
        const std::size_t registrar_length =
            BoundedLength(agent.sip.registrar_uri, max_uri_length);
        const std::size_t username_length =
            BoundedLength(agent.sip.auth_username, max_username_length);
        const std::size_t password_length =
            BoundedLength(agent.sip.auth_password, max_password_length);
        CopyString(context->sip.identity_uri, agent.sip.identity_uri,
                   identity_length);
        CopyString(context->sip.registrar_uri, agent.sip.registrar_uri,
                   registrar_length);
        CopyString(context->sip.auth_username, agent.sip.auth_username,
                   username_length);
        CopyString(context->sip.auth_password, agent.sip.auth_password,
                   password_length);
        context->audio = agent.audio;
        context->registration = agent.register_on_start
                                    ? RegistrationState::registering
                                    : RegistrationState::disabled;
        context->promoted_call = CallHandle{};
        handles_[index] = handle;
        count_ = static_cast<std::uint8_t>(index + 1);
    }
    return Error::ok;
}

void AgentRegistry::Reset() noexcept {
    pool_.InvalidateAll();
    for (AgentHandle &handle : handles_) handle = AgentHandle{};
    count_ = 0;
}

Error AgentRegistry::GetAgentHandle(std::uint8_t config_index,
                                    AgentHandle *handle) const noexcept {
    if (handle == nullptr || config_index >= count_)
        return Error::invalid_argument;
    *handle = handles_[config_index];
    return Error::ok;
}

} // namespace voip
