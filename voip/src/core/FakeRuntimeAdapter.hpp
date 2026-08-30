#ifndef VOIP_CORE_FAKE_RUNTIME_ADAPTER_HPP
#define VOIP_CORE_FAKE_RUNTIME_ADAPTER_HPP

#include "RuntimeAdapter.hpp"

#include <array>
#include <cstddef>

namespace voip {

class FakeRuntimeAdapter final : public RuntimeAdapter {
public:
    static constexpr std::size_t capacity = 32;
    FakeRuntimeAdapter() noexcept = default;
    ~FakeRuntimeAdapter() noexcept override = default;
    FakeRuntimeAdapter(const FakeRuntimeAdapter &) = delete;
    FakeRuntimeAdapter &operator=(const FakeRuntimeAdapter &) = delete;

    Error InitializeAccount(const AgentContext &) noexcept override;
    Error PromoteOutgoing(AgentHandle, const char *, std::uint32_t *) noexcept override;
    Error PromoteIncoming(AgentHandle, std::uint32_t, std::uint32_t *) noexcept override;
    Error Answer(std::uint32_t) noexcept override;
    Error Reject(std::uint32_t, std::uint16_t) noexcept override;
    Error Cancel(std::uint32_t) noexcept override;
    Error Hangup(std::uint32_t) noexcept override;
    Error SetHeld(std::uint32_t, bool) noexcept override;
    bool Poll(RuntimeNotification *) noexcept override;
    Error BeginCallTeardown(std::uint32_t) noexcept override;
    Error Shutdown() noexcept override;

    // Host and fake-only integration seam for deterministic native callback
    // delivery. The production adapter supplies the same copied records from
    // its poller; this method never mutates core state.
    Error Inject(const RuntimeNotification &) noexcept;
    void SetCallbacksDeferred(bool) noexcept;
    void DrainDeferredCallbacks() noexcept;
    void FailNext(RuntimeRequest::Type, Error) noexcept;
    std::size_t RequestCount() const noexcept { return request_count_; }
    bool GetRequest(std::size_t index, RuntimeRequest *request) const noexcept;

private:
    void Record(const RuntimeRequest &) noexcept;
    Error ConsumeFailure(RuntimeRequest::Type) noexcept;
    Error Enqueue(const RuntimeNotification &) noexcept;
    std::array<RuntimeNotification, capacity> notifications_{};
    std::array<RuntimeNotification, capacity> deferred_notifications_{};
    std::size_t read_ = 0;
    std::size_t write_ = 0;
    std::size_t count_ = 0;
    std::size_t deferred_count_ = 0;
    std::uint32_t next_token_ = 1;
    bool stopped_ = false;
    bool callbacks_deferred_ = false;
    std::array<RuntimeRequest, capacity> requests_{};
    std::size_t request_count_ = 0;
    std::array<Error, 10> failures_{};
    static_assert(static_cast<std::size_t>(RuntimeRequest::Type::shutdown) + 1 == 10,
                  "failure table must cover every adapter request");
};

} // namespace voip

#endif
