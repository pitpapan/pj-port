#include <zephyr/sys/printk.h>

namespace voip::test { void RunPjsuaRuntimeTests(); void RunPjsuaCallbackRouterTests(); }

int main() {
    voip::test::RunPjsuaRuntimeTests();
    voip::test::RunPjsuaCallbackRouterTests();
    printk("PJSUA PLAN 3 COMPONENT RESULT: PASSED\n");
    return 0;
}
