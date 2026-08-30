#ifndef VOIP_CORE_SYNCHRONIZATION_HPP
#define VOIP_CORE_SYNCHRONIZATION_HPP

#include <cstdint>
#include <atomic>

#if defined(__ZEPHYR__)
#include <zephyr/kernel.h>
#else
#include <chrono>
#include <condition_variable>
#include <mutex>
#endif

namespace voip {

// Private synchronization wrappers keep the platform primitives out of all
// public service and event types.  They own their storage and never allocate.
class CoreMutex final {
public:
    CoreMutex() noexcept {
#if defined(__ZEPHYR__)
        k_mutex_init(&mutex_);
#endif
    }

    CoreMutex(const CoreMutex &) = delete;
    CoreMutex &operator=(const CoreMutex &) = delete;

    void Lock() noexcept {
#if defined(__ZEPHYR__)
        (void)k_mutex_lock(&mutex_, K_FOREVER);
#else
        mutex_.lock();
#endif
    }

    void Unlock() noexcept {
#if defined(__ZEPHYR__)
        (void)k_mutex_unlock(&mutex_);
#else
        mutex_.unlock();
#endif
    }

private:
    friend class CoreLockGuard;
#if defined(__ZEPHYR__)
    struct k_mutex mutex_{};
#else
    std::mutex mutex_{};
#endif
};

class CoreLockGuard final {
public:
    explicit CoreLockGuard(CoreMutex &mutex) noexcept : mutex_(mutex) {
        mutex_.Lock();
    }

    ~CoreLockGuard() noexcept { mutex_.Unlock(); }

    CoreLockGuard(const CoreLockGuard &) = delete;
    CoreLockGuard &operator=(const CoreLockGuard &) = delete;

private:
    CoreMutex &mutex_;
};

// A counting signal is used rather than a naked condition-variable notify:
// notifications that occur between the queue check and the wait are retained
// and therefore cannot cause a lost wakeup.
class CoreEventSignal final {
public:
    CoreEventSignal() noexcept {
#if defined(__ZEPHYR__)
        k_sem_init(&signal_, 0, 1);
#endif
    }

    CoreEventSignal(const CoreEventSignal &) = delete;
    CoreEventSignal &operator=(const CoreEventSignal &) = delete;

    void Notify() noexcept {
#if defined(__ZEPHYR__)
        (void)k_sem_give(&signal_);
#else
        signaled_.store(true, std::memory_order_release);
        condition_.notify_one();
#endif
    }

    // Clear is called by the queue while holding its queue mutex, making the
    // empty check and signal reset one ordered operation against producers.
    void Clear() noexcept {
#if defined(__ZEPHYR__)
        k_sem_reset(&signal_);
#else
        signaled_.store(false, std::memory_order_release);
#endif
    }

    bool Wait(std::uint32_t timeout_ms) noexcept {
#if defined(__ZEPHYR__)
        return k_sem_take(&signal_, K_MSEC(timeout_ms)) == 0;
#else
        std::unique_lock<std::mutex> lock(mutex_);
        const auto ready = [this]() noexcept {
            return signaled_.load(std::memory_order_acquire);
        };
        if (timeout_ms == 0) {
            if (!ready()) return false;
        } else if (!condition_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                        ready)) {
            return false;
        }
        signaled_.store(false, std::memory_order_release);
        return true;
#endif
    }

private:
#if defined(__ZEPHYR__)
    struct k_sem signal_{};
#else
    std::mutex mutex_{};
    std::condition_variable condition_{};
    std::atomic<bool> signaled_{false};
#endif
};

} // namespace voip

#endif
