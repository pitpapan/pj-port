#include "../src/HandlePool.hpp"

#include <cassert>
#include <cstdint>

namespace {

struct Handle {
    std::uint16_t slot;
    std::uint16_t generation;
};

void test_generation_invalidates_released_slot() {
    voip::HandlePool<Handle, 1> pool;
    Handle first{};
    assert(pool.Acquire(&first));
    assert(pool.IsLive(first));
    Handle unavailable{};
    assert(!pool.Acquire(&unavailable));

    pool.Release(first);
    assert(!pool.IsLive(first));

    Handle second{};
    assert(pool.Acquire(&second));
    assert(second.slot == first.slot);
    assert(second.generation != first.generation);
    assert(!pool.IsLive(first));
    assert(pool.IsLive(second));
}

void test_capacity_and_clear() {
    voip::HandlePool<Handle, 2> pool;
    Handle first{};
    Handle second{};
    assert(pool.Acquire(&first));
    assert(pool.Acquire(&second));
    Handle unavailable{};
    assert(!pool.Acquire(&unavailable));
    pool.Clear();
    assert(!pool.IsLive(first));
    assert(!pool.IsLive(second));

    Handle replacement{};
    assert(pool.Acquire(&replacement));
    assert(replacement.generation != first.generation);
    assert(!pool.IsLive(first));
}

} // namespace

int main() {
    test_generation_invalidates_released_slot();
    test_capacity_and_clear();
    return 0;
}
