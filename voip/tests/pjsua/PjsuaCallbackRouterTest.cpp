#include "../../src/pjsua/PjsuaCallbackRouter.hpp"

#include <cassert>

namespace voip::test {
class RecordingSink final : public PjsuaCallbackSink {
public:
    void OnRegistrationStarted(const PjsuaRegistrationRecord &) noexcept override { ++started; }
    void OnRegistrationState(const PjsuaRegistrationRecord &) noexcept override { ++state; }
    unsigned started = 0;
    unsigned state = 0;
};

void RunPjsuaCallbackRouterTests() {
    RecordingSink sink;
    PjsuaCallbackRouter first(sink);
    PjsuaCallbackRouter second(sink);
    pjsua_callback callbacks{};
    assert(first.Attach(&callbacks) == Error::ok);
    assert(second.Attach(&callbacks) == Error::invalid_state);
    pjsua_reg_info info{};
    callbacks.on_reg_started2(0, &info);
    callbacks.on_reg_state2(0, &info);
    assert(sink.started == 1 && sink.state == 1);
    first.BeginQuiescence();
    callbacks.on_reg_state2(0, &info);
    assert(sink.state == 2);
    first.Detach();
    assert(second.Attach(&callbacks) == Error::ok);
    second.Detach();
}
} // namespace voip::test
