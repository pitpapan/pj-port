#include "PjRuntime.hpp"

namespace voip {

PjRuntime::PjRuntime() noexcept
    : pj_initialized_(false), pool_initialized_(false), caching_pool_{} {}

PjRuntime::~PjRuntime() {
    ShutdownPool();
    ShutdownPjlib();
}

pj_status_t PjRuntime::InitializePjlib() noexcept {
    if (pj_initialized_) return PJ_EINVALIDOP;
    const pj_status_t status = pj_init();
    if (status == PJ_SUCCESS) pj_initialized_ = true;
    return status;
}

pj_status_t PjRuntime::InitializePool(
    BlockAllocCallback alloc, BlockFreeCallback free) noexcept {
    if (!pj_initialized_ || pool_initialized_) return PJ_EINVALIDOP;
    pj_caching_pool_init(&caching_pool_, nullptr, 1);
    caching_pool_.factory.on_block_alloc = alloc;
    caching_pool_.factory.on_block_free = free;
    pool_initialized_ = true;
    return PJ_SUCCESS;
}

void PjRuntime::ShutdownPool() noexcept {
    if (!pool_initialized_) return;
    pj_caching_pool_destroy(&caching_pool_);
    pool_initialized_ = false;
}

void PjRuntime::ShutdownPjlib() noexcept {
    if (!pj_initialized_) return;
    pj_shutdown();
    pj_initialized_ = false;
}

} // namespace voip
