#ifndef VOIP_PJSUA_RUNTIME_ADAPTER_HPP
#define VOIP_PJSUA_RUNTIME_ADAPTER_HPP

#include "../core/RuntimeAdapter.hpp"
#include "PjsuaAccountManager.hpp"
#include "PjsuaCallbackRouter.hpp"
#include "PjsuaRuntime.hpp"
#include "PjsuaTransportManager.hpp"

#include <array>
#include <cstddef>

namespace voip {

// Thin actor-owned composition root. Focused PJSUA components retain their
// own runtime, transport, account, retry, and callback responsibilities.
class PjsuaRuntimeAdapter final : public RuntimeAdapter {
public:
    explicit PjsuaRuntimeAdapter(const PjsuaApi &api = NativePjsuaApi()) noexcept;
    Error Initialize(const AgentRegistry &, const SecurityPolicy &,
                     const PcmFormat &) noexcept override;
    Error Pump(std::uint64_t, std::uint32_t) noexcept override;
    Error PromoteOutgoing(AgentHandle, const char *, std::uint32_t *) noexcept override;
    Error PromoteIncoming(AgentHandle, std::uint32_t, std::uint32_t *) noexcept override;
    Error Answer(std::uint32_t) noexcept override;
    Error Reject(std::uint32_t, std::uint16_t) noexcept override;
    Error Cancel(std::uint32_t) noexcept override;
    Error Hangup(std::uint32_t) noexcept override;
    Error SetHeld(std::uint32_t, bool) noexcept override;
    bool TryGetNotification(RuntimeNotification *) noexcept override;
    Error BeginCallTeardown(std::uint32_t) noexcept override;
    Error Shutdown() noexcept override;

private:
    void Rollback() noexcept;
    void DrainAccountNotifications() noexcept;
    bool Enqueue(const RuntimeNotification &) noexcept;
    const PjsuaApi &api_;
    PjsuaAccountManager accounts_;
    PjsuaCallbackRouter router_;
    PjsuaRuntime runtime_;
    PjsuaTransportManager transport_;
    std::array<RuntimeNotification, notification_capacity> notifications_{};
    std::size_t notification_head_{};
    std::size_t notification_size_{};
    bool initialized_{};
    bool shutdown_started_{};
    std::uint64_t now_ms_{};
};
static_assert(sizeof(PjsuaRuntimeAdapter) < 16384,
              "PJSUA adapter must remain bounded service placement storage");
} // namespace voip
#endif
