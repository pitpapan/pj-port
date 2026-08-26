#ifndef VOIP_VOIP_SERVICE_HPP
#define VOIP_VOIP_SERVICE_HPP

#include <voip/VoipFacade.hpp>

#include <cstddef>
#include <cstdint>

struct k_work_q;

namespace voip {

struct AccountHandle {
    std::uint16_t slot;
    std::uint16_t generation;

    constexpr bool IsValid() const noexcept {
        return generation != 0;
    }
};

struct CallHandle {
    std::uint16_t slot;
    std::uint16_t generation;

    constexpr bool IsValid() const noexcept {
        return generation != 0;
    }
};

using OperationId = std::uint32_t;

constexpr std::uint16_t max_sdk_event_capacity = 16;

enum class EventType : std::uint8_t {
    account_state,
    incoming_call,
    call_state,
    media_state,
    operation_failed,
    resource_pressure,
};

struct ResourceUsage {
    std::uint32_t pool_bytes;
    std::uint32_t pool_peak_bytes;
    std::uint32_t heap_bytes;
    std::uint32_t stack_bytes;
    std::uint16_t active_accounts;
    std::uint16_t active_calls;
    std::uint16_t sockets;
    std::uint16_t timers;
};

struct Event {
    EventType type;
    AccountHandle account;
    CallHandle call;
    OperationId operation;
    RegistrationState registration_state;
    CallInfo call_info;
    Status status;
    ResourceUsage resource;
};

struct ServiceConfig {
    k_work_q *event_queue;
    std::uint16_t command_capacity;
    std::uint16_t event_capacity;
};

struct CallConfig {
    const char *remote_uri;
    Codec codec;
    class PcmSource *source;
    class PcmSink *sink;
};

class EventHandler {
public:
    virtual ~EventHandler() = default;
    virtual void OnEvent(const Event &) noexcept = 0;
};

class PcmSource {
public:
    virtual ~PcmSource() = default;
    virtual Error Read(std::int16_t *samples, std::size_t sample_count,
                       std::uint64_t timestamp) noexcept = 0;
};

class PcmSink {
public:
    virtual ~PcmSink() = default;
    virtual Error Write(const std::int16_t *samples, std::size_t sample_count,
                        std::uint64_t timestamp) noexcept = 0;
};

class VoipService final {
public:
    explicit VoipService(Backend &compatibility_backend) noexcept;
    ~VoipService();

    VoipService(const VoipService &) = delete;
    VoipService &operator=(const VoipService &) = delete;

    Error Initialize(const ServiceConfig &, EventHandler *) noexcept;
    Error Shutdown() noexcept;

    Error AddAccount(const AccountConfig &, AccountHandle *) noexcept;
    Error RemoveAccount(AccountHandle) noexcept;
    Error SetRegistration(AccountHandle, bool, OperationId *) noexcept;
    Error Dial(AccountHandle, const CallConfig &, CallHandle *,
               OperationId *) noexcept;
    Error Answer(CallHandle, OperationId *) noexcept;
    Error Reject(CallHandle, std::uint16_t, OperationId *) noexcept;
    Error Hangup(CallHandle, OperationId *) noexcept;
    Error SetHeld(CallHandle, bool, OperationId *) noexcept;

    Error GetAccountState(AccountHandle, RegistrationState *) const noexcept;
    Error GetCallState(CallHandle, CallInfo *) const noexcept;
    ResourceUsage GetResourceUsage() const noexcept;

private:
    class Impl;
    Impl *impl_;
};

} // namespace voip

#endif
