#include <zephyr/sys/atomic.h>
#include <zephyr/sys/sys_heap.h>

#include <cstddef>

namespace {

// Count allocation attempts, including nested libc/kernel entry points. Zero
// therefore proves that no general heap entry point ran in the tracked window.
atomic_t allocation_count;
atomic_t tracking_enabled;

} // namespace

extern "C" void voip_general_heap_tracking_begin() noexcept {
    atomic_clear(&allocation_count);
    atomic_set(&tracking_enabled, 1);
}

extern "C" std::size_t voip_general_heap_tracking_end() noexcept {
    atomic_clear(&tracking_enabled);
    return static_cast<std::size_t>(atomic_get(&allocation_count));
}

#if defined(VOIP_GENERAL_HEAP_WRAP_ENABLED)

#include <cstdlib>

extern "C" void *__real_malloc(std::size_t);
extern "C" void *__real_calloc(std::size_t, std::size_t);
extern "C" void *__real_realloc(void *, std::size_t);
extern "C" void *__real_aligned_alloc(std::size_t, std::size_t);
extern "C" void *__real_k_malloc(std::size_t);
extern "C" void *__real_k_calloc(std::size_t, std::size_t);
extern "C" void *__real_k_realloc(void *, std::size_t);
extern "C" void *__real_k_aligned_alloc(std::size_t, std::size_t);
extern "C" void *__real_sys_heap_alloc(struct sys_heap *, std::size_t);
extern "C" void *__real_sys_heap_noalign_alloc(struct sys_heap *, std::size_t,
                                                std::size_t);
extern "C" void *__real_sys_heap_realloc(struct sys_heap *, void *,
                                           std::size_t);
extern "C" void *__real_sys_heap_aligned_alloc(struct sys_heap *, std::size_t,
                                                 std::size_t);
extern "C" void *__real_sys_heap_aligned_realloc(struct sys_heap *, void *,
                                                  std::size_t, std::size_t);

namespace {

void RecordAllocation() noexcept {
    if (atomic_get(&tracking_enabled) != 0)
        atomic_inc(&allocation_count);
}

} // namespace

extern "C" void *__wrap_malloc(std::size_t size) {
    RecordAllocation();
    return __real_malloc(size);
}

extern "C" void *__wrap_calloc(std::size_t count, std::size_t size) {
    RecordAllocation();
    return __real_calloc(count, size);
}

extern "C" void *__wrap_realloc(void *memory, std::size_t size) {
    RecordAllocation();
    return __real_realloc(memory, size);
}

extern "C" void *__wrap_aligned_alloc(std::size_t alignment,
                                       std::size_t size) {
    RecordAllocation();
    return __real_aligned_alloc(alignment, size);
}

extern "C" void *__wrap_k_malloc(std::size_t size) {
    RecordAllocation();
    return __real_k_malloc(size);
}

extern "C" void *__wrap_k_calloc(std::size_t count, std::size_t size) {
    RecordAllocation();
    return __real_k_calloc(count, size);
}

extern "C" void *__wrap_k_realloc(void *memory, std::size_t size) {
    RecordAllocation();
    return __real_k_realloc(memory, size);
}

extern "C" void *__wrap_k_aligned_alloc(std::size_t alignment,
                                         std::size_t size) {
    RecordAllocation();
    return __real_k_aligned_alloc(alignment, size);
}

extern "C" void *__wrap_sys_heap_alloc(struct sys_heap *heap,
                                        std::size_t size) {
    RecordAllocation();
    return __real_sys_heap_alloc(heap, size);
}

extern "C" void *__wrap_sys_heap_noalign_alloc(struct sys_heap *heap,
                                                std::size_t alignment,
                                                std::size_t size) {
    RecordAllocation();
    return __real_sys_heap_noalign_alloc(heap, alignment, size);
}

extern "C" void *__wrap_sys_heap_realloc(struct sys_heap *heap, void *memory,
                                          std::size_t size) {
    RecordAllocation();
    return __real_sys_heap_realloc(heap, memory, size);
}

extern "C" void *__wrap_sys_heap_aligned_alloc(struct sys_heap *heap,
                                                std::size_t alignment,
                                                std::size_t size) {
    RecordAllocation();
    return __real_sys_heap_aligned_alloc(heap, alignment, size);
}

extern "C" void *__wrap_sys_heap_aligned_realloc(struct sys_heap *heap,
                                                  void *memory,
                                                  std::size_t alignment,
                                                  std::size_t size) {
    RecordAllocation();
    return __real_sys_heap_aligned_realloc(heap, memory, alignment, size);
}

#endif
