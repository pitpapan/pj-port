#ifndef VOIP_VOIP_FACADE_HPP
#define VOIP_VOIP_FACADE_HPP

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
    value_too_long,
    invalid_state,
    busy,
    queue_full,
    not_initialized,
    shutting_down,
    transport_failure,
    authentication_failure,
    negotiation_failure,
    media_failure,
    internal_failure,
};

enum class RegistrationState : std::uint8_t {
    disabled,
    registering,
    registered,
    unregistering,
    failed,
    connection_lost,
};

enum class CallState : std::uint8_t {
    idle,
    outgoing,
    incoming,
    early,
    established,
    held,
    disconnecting,
    disconnected,
    failed,
};

enum class MediaDirection : std::uint8_t {
    inactive,
    send_only,
    receive_only,
    send_receive,
};

enum class Codec : std::uint8_t {
    pcmu,
    pcma,
};

struct AccountConfig {
    const char *account_uri;
    const char *registrar_uri;
    const char *username;
    const char *password;
    std::uint32_t registration_expires_seconds;
};

struct CallInfo {
    CallState state;
    Codec codec;
    MediaDirection direction;
    char remote_uri[max_uri_length + 1];
    char remote_address[max_address_length + 1];
    std::uint16_t local_rtp_port;
    std::uint16_t local_rtcp_port;
    std::uint16_t remote_rtp_port;
    std::uint16_t remote_rtcp_port;
};

struct Status {
    Error error;
    std::uint16_t sip_status;
    char reason[max_reason_length + 1];
};

class Observer {
public:
    virtual ~Observer() = default;
    virtual void OnRegistrationState(RegistrationState, const Status &) {}
    virtual void OnIncomingCall(const CallInfo &) {}
    virtual void OnCallState(const CallInfo &, const Status &) {}
    virtual void OnMediaState(const CallInfo &, const Status &) {}
};

class Backend {
public:
    virtual ~Backend() = default;
    virtual Error Initialize(Observer *) = 0;
    virtual Error Shutdown() = 0;
    virtual Error ConfigureAccount(const AccountConfig &) = 0;
    virtual Error RegisterAccount() = 0;
    virtual Error UnregisterAccount() = 0;
    virtual Error StartOutgoingCall(const char *) = 0;
    virtual Error AcceptCall() = 0;
    virtual Error RejectCall(std::uint16_t) = 0;
    virtual Error EndCall() = 0;
    virtual Error SetHeld(bool) = 0;
    virtual RegistrationState GetRegistrationState() const = 0;
    virtual CallInfo GetCallInfo() const = 0;
};

class VoipManager final {
public:
    explicit VoipManager(Backend &backend) noexcept;
    ~VoipManager();

    VoipManager(const VoipManager &) = delete;
    VoipManager &operator=(const VoipManager &) = delete;

    Error Initialize(Observer *observer) noexcept;
    Error Shutdown() noexcept;
    Error ConfigureAccount(const AccountConfig &config) noexcept;
    Error RegisterAccount() noexcept;
    Error UnregisterAccount() noexcept;
    Error StartOutgoingCall(const char *remote_uri) noexcept;
    Error AcceptCall() noexcept;
    Error RejectCall(std::uint16_t sip_status = 486) noexcept;
    Error EndCall() noexcept;
    Error SetHeld(bool held) noexcept;
    RegistrationState GetRegistrationState() const noexcept;
    CallInfo GetCallInfo() const noexcept;

private:
    Backend &backend_;
    bool initialized_;
};

} // namespace voip

#endif
