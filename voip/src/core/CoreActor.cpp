#include "CoreActor.hpp"
#include "VoipRuntime.hpp"

#include <chrono>

namespace voip {

bool CoreActor::Start(VoipRuntime &runtime) noexcept {
    if (running_.load()) return false;
    stop_.store(false);
#if defined(__ZEPHYR__)
    running_.store(false);
    thread_ = k_thread_create(&thread_data_, stack_, K_THREAD_STACK_SIZEOF(stack_),
                              &CoreActor::Entry, &runtime, nullptr, nullptr,
                              K_PRIO_PREEMPT(5), 0, K_NO_WAIT);
    if (thread_ != nullptr) running_.store(true);
    return thread_ != nullptr;
#else
    try {
        thread_ = std::thread(&CoreActor::Run, this, &runtime);
    } catch (...) {
        return false;
    }
    return true;
#endif
}

void CoreActor::Stop() noexcept {
    stop_.store(true);
#if defined(__ZEPHYR__)
    if (thread_ != nullptr) {
        k_thread_abort(thread_);
        thread_ = nullptr;
    }
#else
    if (thread_.joinable()) thread_.join();
#endif
    running_.store(false);
}

#if defined(__ZEPHYR__)
void CoreActor::Entry(void *first, void *, void *) noexcept {
    static_cast<VoipRuntime *>(first)->Step(
        static_cast<std::uint64_t>(k_uptime_get()));
    // The actor remains in the runtime-owned loop until shutdown.  Keeping
    // this loop here avoids exposing k_thread to the public service contract.
    while (true) {
        static_cast<VoipRuntime *>(first)->Step(
            static_cast<std::uint64_t>(k_uptime_get()));
        k_sleep(K_MSEC(1));
    }
}
#endif

void CoreActor::Run(VoipRuntime *runtime) noexcept {
#if !defined(__ZEPHYR__)
    running_.store(true);
    // Bootstrap is actor-owned and must run even when the deterministic host
    // seam pauses normal iterations before Start().
    const auto bootstrap_now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    runtime->Step(static_cast<std::uint64_t>(bootstrap_now));
    while (!stop_.load()) {
        if (paused_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        runtime->Step(static_cast<std::uint64_t>(now));
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    running_.store(false);
#else
    (void)runtime;
#endif
}

} // namespace voip
