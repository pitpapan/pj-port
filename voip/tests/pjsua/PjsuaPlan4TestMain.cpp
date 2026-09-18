#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <cassert>

namespace voip::test { void RunPjsuaCallApiTests(); }

int main() {
    voip::test::RunPjsuaCallApiTests();
    printk("PJSUA PLAN 4 COMPONENT RESULT: PASSED\n");
    // The harness owns QEMU lifetime; stay quiescent after the verdict.
    k_sleep(K_FOREVER);
}
