#include "PjSrtpKeyMaterial.hpp"

#include <zephyr/random/random.h>

namespace voip {

PjSrtpKeyMaterial::~PjSrtpKeyMaterial() {
    Clear();
}

bool PjSrtpKeyMaterial::Generate() noexcept {
    Clear();
    if (sys_csrand_get(bytes_, sizeof(bytes_)) != 0) return false;
    active_ = true;
    return true;
}

void PjSrtpKeyMaterial::Clear() noexcept {
    volatile std::uint8_t *byte = bytes_;
    for (std::size_t i = 0; i < sizeof(bytes_); ++i) byte[i] = 0;
    active_ = false;
}

bool PjSrtpKeyMaterial::ClearedForValidation() const noexcept {
    if (active_) return false;
    std::uint8_t aggregate = 0;
    for (std::size_t i = 0; i < sizeof(bytes_); ++i) aggregate |= bytes_[i];
    return aggregate == 0;
}

} // namespace voip
