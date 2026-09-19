#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <cassert>

namespace voip::test {
void RunPjsuaCallApiTests();
void RunPjsuaCallContextTests();
void RunPjsuaIncomingAdmissionTests();
void RunPjsuaOutgoingPromotionTests();
void RunPjsuaCallControlTests();
}

int main() {
    voip::test::RunPjsuaCallApiTests();
    voip::test::RunPjsuaCallContextTests();
    voip::test::RunPjsuaIncomingAdmissionTests();
    voip::test::RunPjsuaOutgoingPromotionTests();
    voip::test::RunPjsuaCallControlTests();
    printk("PJSUA PLAN 4 COMPONENT RESULT: PASSED\n");
    // The harness owns QEMU lifetime; stay quiescent after the verdict.
    k_sleep(K_FOREVER);
}
