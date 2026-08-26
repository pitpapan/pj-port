#ifndef VOIP_PJ_RUNTIME_HPP
#define VOIP_PJ_RUNTIME_HPP

#include <pjlib.h>

namespace voip {

class PjRuntime final {
public:
    using BlockAllocCallback = pj_bool_t (*)(pj_pool_factory *, pj_size_t);
    using BlockFreeCallback = void (*)(pj_pool_factory *, pj_size_t);

    PjRuntime() noexcept;
    ~PjRuntime();

    PjRuntime(const PjRuntime &) = delete;
    PjRuntime &operator=(const PjRuntime &) = delete;

    pj_status_t InitializePjlib() noexcept;
    pj_status_t InitializePool(BlockAllocCallback alloc,
                               BlockFreeCallback free) noexcept;
    void ShutdownPool() noexcept;
    void ShutdownPjlib() noexcept;

    bool PjlibInitialized() const noexcept { return pj_initialized_; }
    bool PoolInitialized() const noexcept { return pool_initialized_; }
    pj_pool_factory *Factory() noexcept { return &caching_pool_.factory; }
    const pj_pool_factory *Factory() const noexcept {
        return &caching_pool_.factory;
    }

private:
    bool pj_initialized_;
    bool pool_initialized_;
    pj_caching_pool caching_pool_;
};

} // namespace voip

#endif
