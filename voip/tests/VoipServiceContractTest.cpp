#include <voip/VoipService.hpp>

#include <cassert>
#include <type_traits>

static_assert(std::is_trivially_copyable<voip::Event>::value,
              "events must be safe to copy out of PJ callbacks");
static_assert(sizeof(voip::AccountHandle) == 4,
              "account handles must remain compact");
static_assert(sizeof(voip::CallHandle) == 4,
              "call handles must remain compact");

int main() {
    const voip::AccountHandle invalid{0, 0};
    const voip::CallHandle valid{0, 1};
    assert(!invalid.IsValid());
    assert(valid.IsValid());
    return 0;
}
