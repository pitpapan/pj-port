#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <cassert>

namespace voip::test { void RunPjsuaCallApiTests(); void RunPjsuaCallContextTests(); }

int main() {
    voip::test::RunPjsuaCallApiTests();
    voip::test::RunPjsuaCallContextTests();
    printk("PJSUA PLAN 4 COMPONENT RESULT: PASSED\n");
    // The harness owns QEMU lifetime; stay quiescent after the verdict.
    k_sleep(K_FOREVER);
}
