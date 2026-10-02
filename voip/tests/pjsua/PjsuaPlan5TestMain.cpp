#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
namespace voip::test { void RunPjsuaAudioPortTests(); void RunPjsuaMediaClockTests(); }
namespace voip::test { void RunPjsuaConferenceClockTests(); }
namespace voip::test { void RunPjsuaMediaBridgeTests(); }
namespace voip::test { void RunPjsuaNativeBridgeTests(); }
int main() {
    voip::test::RunPjsuaAudioPortTests();
    voip::test::RunPjsuaMediaClockTests();
    voip::test::RunPjsuaMediaBridgeTests();
    voip::test::RunPjsuaConferenceClockTests();
    voip::test::RunPjsuaNativeBridgeTests();
    printk("PJSUA PLAN 5 COMPONENT RESULT: PASSED\n");
    k_sleep(K_FOREVER);
}
