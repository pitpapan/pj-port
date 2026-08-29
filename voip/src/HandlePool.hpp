#ifndef VOIP_HANDLE_POOL_HPP
#define VOIP_HANDLE_POOL_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <type_traits>
#include <utility>

namespace voip {

// A fixed-capacity owner for objects addressed by a generation-safe handle.
// Object defaults to Handle for compatibility with the legacy slot-only use
// of this class; production registries should provide their context type.
template <typename Handle, std::size_t Capacity, typename Object = Handle>
class HandlePool final {
private:
    using SlotIndex = typename std::remove_cv<decltype(Handle{}.slot)>::type;

    static_assert(Capacity > 0, "a handle pool must have at least one slot");
    static_assert(Capacity <=
                      static_cast<std::size_t>(std::numeric_limits<SlotIndex>::max()) +
                          1U,
                  "handle slot field cannot represent the pool capacity");
    static_assert(std::is_nothrow_destructible<Object>::value,
                  "pool objects must have non-throwing destructors");

    struct Slot {
        bool occupied = false;
        std::uint16_t generation = 0;
        typename std::aligned_storage<sizeof(Object), alignof(Object)>::type
            storage;
    };

public:
    static constexpr std::size_t capacity() noexcept { return Capacity; }

    HandlePool() noexcept = default;

    ~HandlePool() noexcept { InvalidateAll(); }

    HandlePool(const HandlePool &) = delete;
    HandlePool &operator=(const HandlePool &) = delete;

    template <typename... Args>
    Object *Allocate(Handle *handle, Args &&...args) noexcept {
        static_assert(std::is_nothrow_constructible<Object, Args &&...>::value,
                      "pool object construction must not throw");
        if (handle == nullptr) return nullptr;

        for (std::size_t index = 0; index < Capacity; ++index) {
            Slot &slot = slots_[index];
            if (slot.occupied) continue;
            if (slot.generation == 0) slot.generation = 1;

            Object *object = Construct(slot, std::forward<Args>(args)...);
            slot.occupied = true;
            handle->slot = static_cast<SlotIndex>(index);
            handle->generation = slot.generation;
            return object;
        }
        return nullptr;
    }

    template <typename... Args>
    Object *Allocate(Handle &handle, Args &&...args) noexcept {
        return Allocate(&handle, std::forward<Args>(args)...);
    }

    Object *Resolve(const Handle &handle) noexcept {
        const std::size_t index = static_cast<std::size_t>(handle.slot);
        if (index >= Capacity || handle.generation == 0) return nullptr;
        Slot &slot = slots_[index];
        if (!slot.occupied || slot.generation != handle.generation)
            return nullptr;
        return ObjectAt(slot);
    }

    const Object *Resolve(const Handle &handle) const noexcept {
        const std::size_t index = static_cast<std::size_t>(handle.slot);
        if (index >= Capacity || handle.generation == 0) return nullptr;
        const Slot &slot = slots_[index];
        if (!slot.occupied || slot.generation != handle.generation)
            return nullptr;
        return ObjectAt(slot);
    }

    Object *Resolve(const Handle *handle) noexcept {
        return handle == nullptr ? nullptr : Resolve(*handle);
    }

    const Object *Resolve(const Handle *handle) const noexcept {
        return handle == nullptr ? nullptr : Resolve(*handle);
    }

    bool Release(const Handle &handle) noexcept {
        Object *object = Resolve(handle);
        if (object == nullptr) return false;
        Slot &slot = slots_[static_cast<std::size_t>(handle.slot)];
        object->~Object();
        slot.occupied = false;
        AdvanceGeneration(slot);
        return true;
    }

    bool Release(const Handle *handle) noexcept {
        return handle != nullptr && Release(*handle);
    }

    void InvalidateAll() noexcept {
        for (Slot &slot : slots_) {
            if (slot.occupied) {
                ObjectAt(slot)->~Object();
                slot.occupied = false;
            }
            AdvanceGeneration(slot);
        }
    }

    // Legacy names remain as thin wrappers so older facade code can migrate
    // without changing handle lifetime semantics.
    bool Acquire(Handle *handle) noexcept {
        return Allocate(handle) != nullptr;
    }

    bool IsLive(const Handle &handle) const noexcept {
        return Resolve(handle) != nullptr;
    }

    void Clear() noexcept { InvalidateAll(); }

private:
    template <typename... Args>
    static Object *Construct(Slot &slot, Args &&...args) noexcept {
        return ::new (static_cast<void *>(&slot.storage))
            Object(std::forward<Args>(args)...);
    }

    static Object *ObjectAt(Slot &slot) noexcept {
        return reinterpret_cast<Object *>(&slot.storage);
    }

    static const Object *ObjectAt(const Slot &slot) noexcept {
        return reinterpret_cast<const Object *>(&slot.storage);
    }

    static void AdvanceGeneration(Slot &slot) noexcept {
        ++slot.generation;
        if (slot.generation == 0) slot.generation = 1;
    }

    std::array<Slot, Capacity> slots_{};
};

} // namespace voip

#endif
