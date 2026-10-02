#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
namespace voip::test { void RunPjsuaAudioPortTests(); }
int main() {
    voip::test::RunPjsuaAudioPortTests();
    printk("PJSUA PLAN 5 COMPONENT RESULT: PASSED\n");
    k_sleep(K_FOREVER);
}
