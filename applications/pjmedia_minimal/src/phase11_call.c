#define phase6_udp_call_run phase11_signaling_run
#include "phase6_udp_call.c"

int phase11_call_run(void)
{
	int result = phase11_signaling_run();

	if (result == 0)
		printk("PHASE 11 RESULT: PASSED (integrated SIP-controlled headless G.711 media)\n");
	else
		printk("PHASE 11 RESULT: FAILED\n");
	return result;
}
