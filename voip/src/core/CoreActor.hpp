#ifndef VOIP_CORE_CORE_ACTOR_HPP
#define VOIP_CORE_CORE_ACTOR_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#if defined(__ZEPHYR__)
#include <zephyr/kernel.h>
#else
#include <thread>
#endif

namespace voip {
class VoipRuntime;

class CoreActor final {
public:
    CoreActor() noexcept = default;
    ~CoreActor() noexcept { Stop(); }
    CoreActor(const CoreActor &) = delete;
    CoreActor &operator=(const CoreActor &) = delete;

    bool Start(VoipRuntime &) noexcept;
    void Stop() noexcept;
    void SetPaused(bool paused) noexcept { paused_.store(paused); }
    bool Running() const noexcept { return running_.load(); }
#if defined(CONFIG_VOIP_PJSUA_REGISTRATION_TEST)
    // The registration integration test verifies that a full actor turn can
    // survive a context switch without corrupting the actor TLS record.
    static bool RegistrationTestTlsProbePassed() noexcept;
    static std::size_t RegistrationTestMinimumUnusedStackBytes() noexcept;
#endif

private:
    void Run(VoipRuntime *) noexcept;
#if defined(__ZEPHYR__)
    static void Entry(void *, void *, void *) noexcept;
    struct k_thread thread_data_{};
    K_KERNEL_STACK_MEMBER(stack_, CONFIG_VOIP_CORE_ACTOR_STACK_SIZE);
    k_tid_t thread_ = nullptr;
#else
    std::thread thread_{};
#endif
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_{false};
    std::atomic<bool> paused_{false};
};
} // namespace voip

#endif
