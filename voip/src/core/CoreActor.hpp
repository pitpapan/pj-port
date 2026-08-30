#ifndef VOIP_CORE_CORE_ACTOR_HPP
#define VOIP_CORE_CORE_ACTOR_HPP

#include <atomic>
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
    bool Running() const noexcept { return running_.load(); }

private:
    void Run(VoipRuntime *) noexcept;
#if defined(__ZEPHYR__)
    static void Entry(void *, void *, void *) noexcept;
    struct k_thread thread_data_{};
    K_KERNEL_STACK_MEMBER(stack_, 4096);
    k_tid_t thread_ = nullptr;
#else
    std::thread thread_{};
#endif
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_{false};
};
} // namespace voip

#endif
