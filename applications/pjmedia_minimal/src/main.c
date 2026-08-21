#include <zephyr/sys/printk.h>

#if defined(CONFIG_PJLIB)
#include <pjlib.h>
#endif

#if defined(CONFIG_PJMEDIA_PHASE1_BOUNDARY_TEST)
int phase1_boundary_run(void);
#endif

#if defined(CONFIG_PJMEDIA_PHASE2_SDP_TEST)
int phase2_sdp_run(void);
#endif

#if defined(CONFIG_PJMEDIA_PHASE3_SDP_NEG_TEST)
int phase3_sdp_neg_run(void);
#endif

#if defined(CONFIG_PJMEDIA_PHASE3_LINK_PROBE)
int phase3_link_probe_run(void);
#endif

#if defined(CONFIG_PJMEDIA_PHASE4_INVITE_TEST)
int phase4_invite_run(void);
#endif

int main(void)
{
	printk("PJMEDIA minimal Zephyr application\n");

#if defined(CONFIG_PJMEDIA_PHASE3_LINK_PROBE)
	return phase3_link_probe_run();
#elif defined(CONFIG_PJMEDIA_PHASE4_INVITE_TEST)
	return phase4_invite_run();
#elif defined(CONFIG_PJMEDIA_PHASE3_SDP_NEG_TEST)
	return phase3_sdp_neg_run();
#elif defined(CONFIG_PJMEDIA_PHASE2_SDP_TEST)
	return phase2_sdp_run();
#elif defined(CONFIG_PJMEDIA_PHASE1_BOUNDARY_TEST)
	return phase1_boundary_run();
#elif defined(CONFIG_PJLIB)
	pj_status_t status = pj_init();

	if (status != PJ_SUCCESS) {
		printk("PJLIB initialization failed: %d\n", status);
		return 1;
	}

	printk("PJMEDIA disabled; PJLIB boundary initialized\n");
	pj_shutdown();
#else
	printk("PJPROJECT disabled\n");
#endif

	return 0;
}
