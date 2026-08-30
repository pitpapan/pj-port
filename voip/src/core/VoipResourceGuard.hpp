#ifndef VOIP_CORE_VOIP_RESOURCE_GUARD_HPP
#define VOIP_CORE_VOIP_RESOURCE_GUARD_HPP

#include <voip/VoipTypes.hpp>

namespace voip {

class VoipResourceGuard final {
public:
    VoipResourceGuard() noexcept = default;
    void Update(const ResourceSnapshot &snapshot) noexcept { snapshot_ = snapshot; }
    ResourceSnapshot Snapshot() const noexcept { return snapshot_; }
private:
    ResourceSnapshot snapshot_{};
};

} // namespace voip

#endif
