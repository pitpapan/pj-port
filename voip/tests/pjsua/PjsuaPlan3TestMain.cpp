#include <zephyr/sys/printk.h>
#include "FakePjsuaApi.hpp"
#include "../../src/pjsua/PjsuaCallbackRouter.hpp"
#include <cassert>

namespace voip::test { void RunPjsuaRuntimeTests(); void RunPjsuaCallbackRouterTests(); void RunPjsuaAccountManagerTests(); void RunPjsuaRegistrationStateTests(); void RunPjsuaRuntimeAdapterTests(); void RunPjsuaPublicServiceTests(); }

namespace {
void AssertDetached() {
    assert(voip::PjsuaCallbackRouter::ActiveForTest() == nullptr);
    assert(voip::test::FakePjsuaApi::ActiveForTest() == nullptr);
}
}

int main() {
    constexpr unsigned test_first = 1;
    constexpr unsigned test_limit = 7;
    if (test_first <= 1 && test_limit >= 1) { voip::test::RunPjsuaPublicServiceTests(); AssertDetached(); }
    if (test_first <= 2 && test_limit >= 2) { voip::test::RunPjsuaRuntimeTests(); AssertDetached(); }
    if (test_first <= 3 && test_limit >= 3) { voip::test::RunPjsuaCallbackRouterTests(); AssertDetached(); }
    if (test_first <= 4 && test_limit >= 4) { voip::test::RunPjsuaAccountManagerTests(); AssertDetached(); }
    if (test_first <= 5 && test_limit >= 5) { voip::test::RunPjsuaRegistrationStateTests(); AssertDetached(); }
    if (test_first <= 6 && test_limit >= 6) { voip::test::RunPjsuaRuntimeAdapterTests(); AssertDetached(); }
    printk("PJSUA PLAN 3 COMPONENT RESULT: PASSED\n");
    return 0;
}
