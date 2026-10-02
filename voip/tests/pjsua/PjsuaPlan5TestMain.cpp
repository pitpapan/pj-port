#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include "../../src/pjsua/PjsuaRuntimeAdapter.hpp"
namespace voip::test { void RunPjsuaAudioPortTests(); void RunPjsuaMediaClockTests(); }
namespace voip::test { void RunPjsuaConferenceClockTests(); }
namespace voip::test { void RunPjsuaMediaBridgeTests(); }
namespace voip::test { void RunPjsuaNativeBridgeTests(); }
namespace voip::test { void RunPjsuaMediaManagerTests(); }
namespace voip::test { void RunPjsuaMediaOwnershipTests(); }
int main() {
    voip::test::RunPjsuaAudioPortTests();
    voip::test::RunPjsuaMediaClockTests();
    voip::test::RunPjsuaMediaBridgeTests();
    voip::test::RunPjsuaMediaManagerTests();
    voip::test::RunPjsuaMediaOwnershipTests();
    voip::test::RunPjsuaConferenceClockTests();
    voip::test::RunPjsuaNativeBridgeTests();
    size_t unused=0;
    if (k_thread_stack_space_get(k_current_get(),&unused)==0)
        printk("Plan5 component main stack unused=%u adapter bytes=%u\n",
               static_cast<unsigned>(unused),static_cast<unsigned>(sizeof(voip::PjsuaRuntimeAdapter)));
    printk("PJSUA PLAN 5 COMPONENT RESULT: PASSED\n");
    k_sleep(K_FOREVER);
}
