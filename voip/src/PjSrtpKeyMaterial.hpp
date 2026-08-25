#ifndef VOIP_PJ_SRTP_KEY_MATERIAL_HPP
#define VOIP_PJ_SRTP_KEY_MATERIAL_HPP

#include <cstddef>
#include <cstdint>

namespace voip {

class PjSrtpKeyMaterial final {
public:
    static constexpr std::size_t size = 30;

    PjSrtpKeyMaterial() noexcept = default;
    ~PjSrtpKeyMaterial();

    PjSrtpKeyMaterial(const PjSrtpKeyMaterial &) = delete;
    PjSrtpKeyMaterial &operator=(const PjSrtpKeyMaterial &) = delete;
    PjSrtpKeyMaterial(PjSrtpKeyMaterial &&) = delete;
    PjSrtpKeyMaterial &operator=(PjSrtpKeyMaterial &&) = delete;

    bool Generate() noexcept;
    void Clear() noexcept;
    const std::uint8_t *Data() const noexcept { return bytes_; }
    bool Active() const noexcept { return active_; }
    bool ClearedForValidation() const noexcept;

private:
    std::uint8_t bytes_[size]{};
    bool active_{};
};

} // namespace voip

#endif
