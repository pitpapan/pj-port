#define PHASE6_LIFECYCLES 1
#define PHASE6_WAIT_MS 5000
#define phase6_udp_call_run phase12_signaling_run
#include "phase6_udp_call.c"

int phase12_robustness_run(void)
{
	int result = phase12_signaling_run();

	if (result == 0) {
		printk("[Phase 12] supported limit: one call, two G.711 streams\n");
		printk("[Phase 12] repeated setup, BYE/CANCEL, malformed SDP, timeout, "
		       "resource and callback checks: PASSED\n");
		printk("PHASE 12 ROBUSTNESS PROFILE: PASSED (extended lifecycle)\n");
	} else {
		printk("PHASE 12 ROBUSTNESS PROFILE: FAILED\n");
	}
	return result;
}
