#ifndef VOIP_HANDLE_POOL_HPP
#define VOIP_HANDLE_POOL_HPP

#include <array>
#include <cstddef>
#include <cstdint>

namespace voip {

template <typename Handle, std::size_t Capacity>
class HandlePool final {
public:
    HandlePool() noexcept { Clear(); }

    bool Acquire(Handle *handle) noexcept {
        if (handle == nullptr) return false;
        for (std::size_t index = 0; index < Capacity; ++index) {
            Slot &slot = slots_[index];
            if (slot.live) continue;
            slot.live = true;
            if (slot.generation == 0) slot.generation = 1;
            handle->slot = static_cast<std::uint16_t>(index);
            handle->generation = slot.generation;
            return true;
        }
        return false;
    }

    bool IsLive(const Handle &handle) const noexcept {
        if (handle.slot >= Capacity || handle.generation == 0) return false;
        const Slot &slot = slots_[handle.slot];
        return slot.live && slot.generation == handle.generation;
    }

    bool Release(const Handle &handle) noexcept {
        if (!IsLive(handle)) return false;
        Slot &slot = slots_[handle.slot];
        slot.live = false;
        ++slot.generation;
        if (slot.generation == 0) slot.generation = 1;
        return true;
    }

    void Clear() noexcept {
        for (Slot &slot : slots_) {
            slot.live = false;
            // Clearing a manager is a lifecycle boundary.  Advance every
            // generation so handles from a previous service instance cannot
            // become valid again after reinitialization.
            ++slot.generation;
            if (slot.generation == 0) slot.generation = 1;
        }
    }

private:
    struct Slot {
        bool live;
        std::uint16_t generation;
    };

    std::array<Slot, Capacity> slots_{};
};

} // namespace voip

#endif
