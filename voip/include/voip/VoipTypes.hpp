#ifndef VOIP_VOIP_TYPES_HPP
#define VOIP_VOIP_TYPES_HPP

#include <cstddef>
#include <cstdint>

namespace voip {

constexpr std::size_t max_uri_length = 255;
constexpr std::size_t max_username_length = 63;
constexpr std::size_t max_password_length = 127;
constexpr std::size_t max_reason_length = 95;
constexpr std::size_t max_address_length = 63;

enum class Error : std::uint8_t {
    ok,
    invalid_argument,
    invalid_handle,
    invalid_state,
    unsupported_configuration,
    agent_unavailable,
    busy,
    queue_full,
    resource_exhausted,
    authentication_failed,
    signaling_failed,
    remote_rejected,
    negotiation_failed,
    media_failed,
    cancelled,
    timed_out,
    shutting_down,
    shutdown_timeout,
    internal_failure,
};

enum class RegistrationState : std::uint8_t {
    disabled,
    registering,
    registered,
    refreshing,
    retry_wait,
    unregistering,
    authentication_failed,
    transport_failed,
};

struct AgentHandle {
    std::uint8_t slot;
    std::uint16_t generation;
    constexpr bool IsValid() const noexcept { return generation != 0; }
};

struct CallHandle {
    std::uint8_t slot;
    std::uint16_t generation;
    constexpr bool IsValid() const noexcept { return generation != 0; }
};

using OperationId = std::uint32_t;

enum class CallState : std::uint8_t {
    idle,
    initiated,
    established,
    hold,
    terminated,
};

enum class HoldReason : std::uint8_t {
    none,
    waiting,
    media,
};

enum class CallTransition : std::uint8_t {
    initiation,
    acceptance,
    rejection,
    wait,
    timeout,
    hold,
    resume,
    finish,
    cleanup,
};

enum class SignalingSecurity : std::uint8_t { none, tls };
enum class MediaSecurity : std::uint8_t { none, srtp_sdes };

struct SecurityPolicy {
    SignalingSecurity signaling;
    MediaSecurity media;
};

class PcmSource;
class PcmSink;

struct AgentAudioBinding {
    PcmSource *source;
    PcmSink *sink;
};

// These pointers are borrowed only for the duration of Initialize(). Inputs
// must be non-null, NUL-terminated, and no longer than their corresponding
// fixed bound; initialization copies every string before it returns.
struct SipAccountConfig {
    // identity_uri and registrar_uri are each limited to max_uri_length.
    const char *identity_uri;
    const char *registrar_uri;
    // auth_username and auth_password are limited to max_username_length and
    // max_password_length respectively and are copied into private storage.
    const char *auth_username;
    const char *auth_password;
};

struct Status {
    Error error;
    std::uint16_t sip_status;
    char reason[max_reason_length + 1];
};

// remote_uri is borrowed synchronously by Dial(). It must be non-null,
// NUL-terminated, and no longer than max_uri_length. An accepted request is
// copied into service-owned storage before Dial() returns, including queued
// requests; callers need not retain the input afterwards.
struct DialRequest {
    const char *remote_uri;
};

struct AgentSnapshot {
    AgentHandle handle;
    RegistrationState registration;
    char identity_uri[max_uri_length + 1];
};

struct CallSnapshot {
    CallHandle handle;
    AgentHandle agent;
    CallState state;
    HoldReason hold_reason;
    char remote_uri[max_uri_length + 1];
    char remote_address[max_address_length + 1];
    std::uint16_t sip_status;
};

struct ResourceSnapshot {
    std::uint8_t active_agents;
    std::uint8_t active_calls;
    std::uint8_t promoted_calls;
    std::uint8_t queued_calls;
    std::uint16_t available_commands;
    std::uint16_t available_operations;
    std::uint16_t available_events;
    std::uint16_t available_fifo_entries;
    std::uint16_t available_logical_calls;
    std::uint16_t available_promoted_calls;
    std::uint16_t available_media_bridges;
    std::uint16_t available_call_slots;
    std::uint32_t audio_callback_failures;
};

} // namespace voip

#endif
