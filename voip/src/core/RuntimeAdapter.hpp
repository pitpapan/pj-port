#ifndef VOIP_CORE_RUNTIME_ADAPTER_HPP
#define VOIP_CORE_RUNTIME_ADAPTER_HPP

#include "AgentContext.hpp"

#include <cstdint>
#include <cstddef>

namespace voip {

class AgentRegistry;

struct RuntimeNotification {
    enum class Type : std::uint8_t {
        registration_state,
        call_accepted,
        call_rejected,
        call_finished,
        call_timeout,
        call_held,
        call_resumed,
        incoming_call,
    };
    Type type = Type::call_finished;
    AgentHandle agent{};
    CallHandle call{};
    std::uint32_t token = 0;
    std::uint16_t sip_status = 0;
    Error error = Error::ok;
    RegistrationState registration = RegistrationState::disabled;
    Status status{};
    char remote_uri[max_uri_length + 1]{};
};

struct RuntimeRequest {
    enum class Type : std::uint8_t {
        initialize,
        promote_outgoing,
        promote_incoming,
        answer,
        reject,
        cancel,
        hangup,
        set_held,
        teardown,
        shutdown,
    };
    Type type = Type::shutdown;
    AgentHandle agent{};
    std::uint32_t token = 0;
    std::uint16_t sip_status = 0;
    bool held = false;
    char remote_uri[max_uri_length + 1]{};
};

class RuntimeAdapter {
public:
    static constexpr std::size_t notification_capacity = 32;
    virtual ~RuntimeAdapter() noexcept = default;
    virtual Error Initialize(const AgentRegistry &, const SecurityPolicy &,
                             const PcmFormat &) noexcept = 0;
    virtual Error Pump(std::uint64_t, std::uint32_t) noexcept = 0;
    virtual Error PromoteOutgoing(AgentHandle, const char *,
                                  std::uint32_t *) noexcept = 0;
    virtual Error PromoteIncoming(AgentHandle, std::uint32_t,
                                  std::uint32_t *) noexcept = 0;
    virtual Error Answer(std::uint32_t) noexcept = 0;
    virtual Error Reject(std::uint32_t, std::uint16_t) noexcept = 0;
    virtual Error Cancel(std::uint32_t) noexcept = 0;
    virtual Error Hangup(std::uint32_t) noexcept = 0;
    virtual Error SetHeld(std::uint32_t, bool) noexcept = 0;
    virtual bool TryGetNotification(RuntimeNotification *) noexcept = 0;
    virtual Error BeginCallTeardown(std::uint32_t) noexcept = 0;
    virtual Error Shutdown() noexcept = 0;
};

// Build fallback used by production configurations until a PJPROJECT adapter
// is selected. It keeps the core linkable without pulling the host fake into
// a target image and never owns dynamic state.
class NullRuntimeAdapter final : public RuntimeAdapter {
public:
    Error Initialize(const AgentRegistry &, const SecurityPolicy &,
                     const PcmFormat &) noexcept override { return Error::ok; }
    Error Pump(std::uint64_t, std::uint32_t) noexcept override { return Error::ok; }
    Error PromoteOutgoing(AgentHandle, const char *, std::uint32_t *) noexcept override { return Error::unsupported_configuration; }
    Error PromoteIncoming(AgentHandle, std::uint32_t, std::uint32_t *) noexcept override { return Error::unsupported_configuration; }
    Error Answer(std::uint32_t) noexcept override { return Error::unsupported_configuration; }
    Error Reject(std::uint32_t, std::uint16_t) noexcept override { return Error::unsupported_configuration; }
    Error Cancel(std::uint32_t) noexcept override { return Error::unsupported_configuration; }
    Error Hangup(std::uint32_t) noexcept override { return Error::unsupported_configuration; }
    Error SetHeld(std::uint32_t, bool) noexcept override { return Error::unsupported_configuration; }
    bool TryGetNotification(RuntimeNotification *) noexcept override { return false; }
    Error BeginCallTeardown(std::uint32_t) noexcept override { return Error::unsupported_configuration; }
    Error Shutdown() noexcept override { return Error::ok; }
};

} // namespace voip

#endif
