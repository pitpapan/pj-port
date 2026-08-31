#include <zephyr/sys/printk.h>

namespace voip::test { void RunPjsuaRuntimeTests(); void RunPjsuaCallbackRouterTests(); void RunPjsuaAccountManagerTests(); void RunPjsuaRegistrationStateTests(); }

int main() {
    voip::test::RunPjsuaRuntimeTests();
    voip::test::RunPjsuaCallbackRouterTests();
    voip::test::RunPjsuaAccountManagerTests();
    voip::test::RunPjsuaRegistrationStateTests();
    printk("PJSUA PLAN 3 COMPONENT RESULT: PASSED\n");
    return 0;
}
