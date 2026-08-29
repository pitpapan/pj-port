#ifndef VOIP_VOIP_SERVICE_HPP
#define VOIP_VOIP_SERVICE_HPP

#include <voip/PcmAudio.hpp>
#include <voip/VoipEvents.hpp>

#include <cstddef>
#include <cstdint>

namespace voip {

struct AgentConfig {
    SipAccountConfig sip;
    AgentAudioBinding audio;
    bool register_on_start;
};

struct ServiceConfig {
    const AgentConfig *agents;
    std::uint8_t agent_count;
    std::uint32_t queue_timeout_ms;
    std::uint32_t answer_timeout_ms;
    PcmFormat conference_format;
    SecurityPolicy security;
};

class VoipService final {
public:
    VoipService() noexcept;
    ~VoipService() noexcept;

    VoipService(const VoipService &) = delete;
    VoipService &operator=(const VoipService &) = delete;

    Error Initialize(const ServiceConfig &) noexcept;
    Error Shutdown() noexcept;

    Error GetAgentHandle(std::uint8_t config_index,
                         AgentHandle *) const noexcept;

    Error Dial(AgentHandle, const DialRequest &, CallHandle *,
               OperationId *) noexcept;
    Error Answer(CallHandle, OperationId *) noexcept;
    Error Reject(CallHandle, std::uint16_t sip_status,
                 OperationId *) noexcept;
    Error Cancel(CallHandle, OperationId *) noexcept;
    Error Hangup(CallHandle, OperationId *) noexcept;
    Error SetHeld(CallHandle, bool, OperationId *) noexcept;

    Error TryGetEvent(Event *) noexcept;
    Error WaitForEvent(Event *, std::uint32_t timeout_ms) noexcept;

    Error GetAgentSnapshot(AgentHandle,
                           AgentSnapshot *) const noexcept;
    Error GetCallSnapshot(CallHandle,
                          CallSnapshot *) const noexcept;
    ResourceSnapshot GetResourceSnapshot() const noexcept;

private:
    class Impl;
    static constexpr std::size_t implementation_storage_size = 131072;
    alignas(std::max_align_t) std::uint8_t storage_[implementation_storage_size];
    Impl *impl_;
};

} // namespace voip

#endif
