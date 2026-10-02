#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
namespace voip::test { void RunPjsuaAudioPortTests(); void RunPjsuaMediaClockTests(); }
namespace voip::test { void RunPjsuaConferenceClockTests(); }
int main() {
    voip::test::RunPjsuaAudioPortTests();
    voip::test::RunPjsuaMediaClockTests();
    voip::test::RunPjsuaConferenceClockTests();
    printk("PJSUA PLAN 5 COMPONENT RESULT: PASSED\n");
    k_sleep(K_FOREVER);
}
