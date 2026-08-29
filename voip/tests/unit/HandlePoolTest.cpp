#include "../../src/HandlePool.hpp"

#include <cassert>
#include <cstdio>
#include <cstdint>
#include <type_traits>

#include <voip/VoipTypes.hpp>

namespace {

struct TestHandle {
    std::uint8_t slot = 0;
    std::uint16_t generation = 0;
};

struct NonDefaultObject {
    explicit NonDefaultObject(int value) noexcept : value(value) { ++live_count; }
    ~NonDefaultObject() noexcept { --live_count; ++destroy_count; }

    NonDefaultObject(const NonDefaultObject &) = delete;
    NonDefaultObject &operator=(const NonDefaultObject &) = delete;

    int value;
    static int live_count;
    static int destroy_count;
};

int NonDefaultObject::live_count = 0;
int NonDefaultObject::destroy_count = 0;

using ObjectPool = voip::HandlePool<TestHandle, 3, NonDefaultObject>;
using AgentPool = voip::HandlePool<voip::AgentHandle, 5>;
using CallPool = voip::HandlePool<voip::CallHandle, 7>;

static_assert(AgentPool::capacity() == 5, "agent pool capacity must be five");
static_assert(CallPool::capacity() == 7, "call pool capacity must be seven");

void test_allocate_resolve_and_stable_lowest_slot() {
    NonDefaultObject::live_count = 0;
    NonDefaultObject::destroy_count = 0;
    ObjectPool pool;
    TestHandle first{};
    TestHandle second{};

    NonDefaultObject *first_object = pool.Allocate(&first, 11);
    NonDefaultObject *second_object = pool.Allocate(&second, 22);
    assert(first_object != nullptr);
    assert(second_object != nullptr);
    assert(first.slot == 0);
    assert(second.slot == 1);
    assert(first.generation != 0);
    assert(pool.Resolve(first) == first_object);
    assert(pool.Resolve(second)->value == 22);
    assert(NonDefaultObject::live_count == 2);

    TestHandle third{};
    assert(pool.Allocate(&third, 33) != nullptr);
    TestHandle unavailable{};
    assert(pool.Allocate(&unavailable, 44) == nullptr);
    assert(pool.Resolve(unavailable) == nullptr);
}

void test_release_destroys_once_and_rejects_stale_handle() {
    NonDefaultObject::live_count = 0;
    NonDefaultObject::destroy_count = 0;
    ObjectPool pool;
    TestHandle handle{};
    assert(pool.Allocate(&handle, 7) != nullptr);
    assert(pool.Release(handle));
    assert(NonDefaultObject::live_count == 0);
    assert(NonDefaultObject::destroy_count == 1);
    assert(pool.Resolve(handle) == nullptr);
    assert(!pool.Release(handle));
    assert(NonDefaultObject::destroy_count == 1);

    TestHandle replacement{};
    assert(pool.Allocate(&replacement, 8) != nullptr);
    assert(replacement.slot == handle.slot);
    assert(replacement.generation != handle.generation);
    assert(pool.Resolve(handle) == nullptr);
    assert(pool.Resolve(replacement)->value == 8);
}

void test_pool_destructor_destroys_each_live_object_once() {
    NonDefaultObject::live_count = 0;
    NonDefaultObject::destroy_count = 0;
    {
        ObjectPool pool;
        TestHandle handle{};
        assert(pool.Allocate(&handle, 9) != nullptr);
        assert(NonDefaultObject::live_count == 1);
    }
    assert(NonDefaultObject::live_count == 0);
    assert(NonDefaultObject::destroy_count == 1);
}

void test_invalidate_all_destroys_live_objects_and_invalidates_handles() {
    NonDefaultObject::live_count = 0;
    NonDefaultObject::destroy_count = 0;
    ObjectPool pool;
    TestHandle first{};
    TestHandle second{};
    assert(pool.Allocate(&first, 1) != nullptr);
    assert(pool.Allocate(&second, 2) != nullptr);
    pool.InvalidateAll();
    assert(NonDefaultObject::live_count == 0);
    assert(NonDefaultObject::destroy_count == 2);
    assert(pool.Resolve(first) == nullptr);
    assert(pool.Resolve(second) == nullptr);

    TestHandle reinitialized{};
    assert(pool.Allocate(&reinitialized, 3) != nullptr);
    assert(!pool.Resolve(first));
    assert(pool.Resolve(reinitialized)->value == 3);
    assert(reinitialized.generation != first.generation);
}

void test_invalidate_all_advances_released_slot_generation() {
    ObjectPool pool;
    TestHandle old_handle{};
    assert(pool.Allocate(&old_handle, 1) != nullptr);
    assert(pool.Release(old_handle));
    pool.InvalidateAll();

    TestHandle replacement{};
    assert(pool.Allocate(&replacement, 2) != nullptr);
    assert(replacement.slot == old_handle.slot);
    assert(replacement.generation != old_handle.generation);
}

void test_generation_wrap_skips_zero() {
    ObjectPool pool;
    TestHandle handle{};
    assert(pool.Allocate(&handle, 0) != nullptr);
    for (unsigned count = 0; count < 65535U; ++count) {
        assert(pool.Release(handle));
        assert(pool.Allocate(&handle, 0) != nullptr);
    }
    assert(handle.generation == 1);
    assert(handle.generation != 0);
    assert(pool.Resolve(handle) != nullptr);
    assert(NonDefaultObject::live_count == 1);
}

} // namespace

int main() {
    test_allocate_resolve_and_stable_lowest_slot();
    test_release_destroys_once_and_rejects_stale_handle();
    test_pool_destructor_destroys_each_live_object_once();
    test_invalidate_all_destroys_live_objects_and_invalidates_handles();
    test_invalidate_all_advances_released_slot_generation();
    test_generation_wrap_skips_zero();
    assert(std::is_nothrow_destructible<NonDefaultObject>::value);
    std::puts("HandlePoolTest PASSED");
    return 0;
}
