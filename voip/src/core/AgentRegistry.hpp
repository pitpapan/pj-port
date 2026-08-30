#ifndef VOIP_CORE_AGENT_REGISTRY_HPP
#define VOIP_CORE_AGENT_REGISTRY_HPP

#include "AgentContext.hpp"
#include "../HandlePool.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace voip {

class AgentRegistry final {
public:
    static constexpr std::size_t max_agents = 5;

    AgentRegistry() noexcept = default;
    ~AgentRegistry() noexcept = default;

    AgentRegistry(const AgentRegistry &) = delete;
    AgentRegistry &operator=(const AgentRegistry &) = delete;

    // Validation is completed before this registry is changed.  A failed
    // attempt leaves no live contexts and invalidates the prior handles.
    Error Initialize(const ServiceConfig &config) noexcept;
    void Reset() noexcept;

    std::size_t Count() const noexcept { return count_; }
    bool Empty() const noexcept { return count_ == 0; }

    Error GetAgentHandle(std::uint8_t config_index,
                         AgentHandle *handle) const noexcept;

    AgentContext *Resolve(const AgentHandle &handle) noexcept {
        return pool_.Resolve(handle);
    }
    const AgentContext *Resolve(const AgentHandle &handle) const noexcept {
        return pool_.Resolve(handle);
    }

private:
    static constexpr std::size_t kAgentSlots = 5;
    static_assert(kAgentSlots == max_agents && max_agents == 5,
                  "production registry must contain exactly five agent slots");

    using Pool = HandlePool<AgentHandle, kAgentSlots, AgentContext>;

    static Error Validate(const ServiceConfig &config) noexcept;
    static bool ValidateString(const char *value,
                               std::size_t maximum,
                               std::size_t *length) noexcept;
    static bool IsSupportedFormat(const PcmFormat &format) noexcept;
    static bool SameFormat(const PcmFormat &left,
                           const PcmFormat &right) noexcept;
    static void CopyString(char *destination, const char *source,
                           std::size_t length) noexcept;

    Pool pool_{};
    std::array<AgentHandle, max_agents> handles_{};
    std::uint8_t count_ = 0;
};

static_assert(AgentRegistry::max_agents == 5,
              "AgentRegistry capacity is a fixed product limit");

} // namespace voip

#endif
