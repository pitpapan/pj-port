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

#if defined(CONFIG_PJMEDIA_PHASE5_LOOP_CALL_TEST)
int phase5_loop_call_run(void);
#endif

#if defined(CONFIG_PJMEDIA_PHASE6_UDP_CALL_TEST)
int phase6_udp_call_run(void);
#endif

#if defined(CONFIG_PJMEDIA_PHASE7_G711_TEST)
int phase7_g711_run(void);
#endif

#if defined(CONFIG_PJMEDIA_PHASE7_LINK_PROBE)
int phase7_link_probe_run(void);
#endif

#if defined(CONFIG_PJMEDIA_PHASE8_PACKET_TEST)
int phase8_packet_run(void);
#endif

#if defined(CONFIG_PJMEDIA_PHASE8_LINK_PROBE)
int phase8_link_probe_run(void);
#endif

#if defined(CONFIG_PJMEDIA_PHASE9_TRANSPORT_TEST)
int phase9_transport_run(void);
#endif

#if defined(CONFIG_PJMEDIA_PHASE9_LINK_PROBE)
int phase9_link_probe_run(void);
#endif

#if defined(CONFIG_PJMEDIA_PHASE11_LINK_PROBE)
int phase11_link_probe_run(void);
#endif

#if defined(CONFIG_PJMEDIA_PHASE11_CALL_TEST)
int phase11_call_run(void);
#endif

#if defined(CONFIG_PJMEDIA_PHASE12_ROBUSTNESS_TEST)
int phase12_robustness_run(void);
#endif

#if defined(CONFIG_PJMEDIA_SRTP_PRIMITIVE_TEST)
int srtp_primitives_run(void);
#endif

#if defined(CONFIG_PJMEDIA_SRTP_TRANSPORT_TEST)
int srtp_transport_run(void);
#endif

#if defined(CONFIG_PJMEDIA_SRTP_SDES_TEST)
int srtp_sdes_run(void);
#endif

int main(void)
{
	printk("PJMEDIA minimal Zephyr application\n");

#if defined(CONFIG_PJMEDIA_SRTP_SDES_TEST)
	return srtp_sdes_run();
#elif defined(CONFIG_PJMEDIA_SRTP_TRANSPORT_TEST)
	return srtp_transport_run();
#elif defined(CONFIG_PJMEDIA_SRTP_PRIMITIVE_TEST)
	return srtp_primitives_run();
#elif defined(CONFIG_PJMEDIA_PHASE12_ROBUSTNESS_TEST)
	return phase12_robustness_run();
#elif defined(CONFIG_PJMEDIA_PHASE11_CALL_TEST)
	return phase11_call_run();
#elif defined(CONFIG_PJMEDIA_PHASE11_LINK_PROBE)
	return phase11_link_probe_run();
#elif defined(CONFIG_PJMEDIA_PHASE9_LINK_PROBE)
	return phase9_link_probe_run();
#elif defined(CONFIG_PJMEDIA_PHASE9_TRANSPORT_TEST)
	return phase9_transport_run();
#elif defined(CONFIG_PJMEDIA_PHASE9_DISABLED_TEST)
	{
		pj_status_t status = pj_init();

		if (status != PJ_SUCCESS) {
			printk("Phase 9 disabled profile: PJLIB init failed: %d\n",
			       status);
			return 1;
		}
		pj_shutdown();
		printk("PHASE 9 DISABLED: PASSED (no media transport objects)\n");
		return 0;
	}
#elif defined(CONFIG_PJMEDIA_PHASE8_LINK_PROBE)
	return phase8_link_probe_run();
#elif defined(CONFIG_PJMEDIA_PHASE8_PACKET_TEST)
	return phase8_packet_run();
#elif defined(CONFIG_PJMEDIA_PHASE8_DISABLED_TEST)
	{
		pj_status_t status = pj_init();

		if (status != PJ_SUCCESS) {
			printk("Phase 8 disabled profile: PJLIB init failed: %d\n",
			       status);
			return 1;
		}
		pj_shutdown();
		printk("PHASE 8 DISABLED: PASSED (no RTP/RTCP/jitter objects)\n");
		return 0;
	}
#elif defined(CONFIG_PJMEDIA_PHASE7_LINK_PROBE)
	return phase7_link_probe_run();
#elif defined(CONFIG_PJMEDIA_PHASE7_G711_TEST)
	return phase7_g711_run();
#elif defined(CONFIG_PJMEDIA_PHASE6_UDP_CALL_TEST)
	return phase6_udp_call_run();
#elif defined(CONFIG_PJMEDIA_PHASE5_LOOP_CALL_TEST)
	return phase5_loop_call_run();
#elif defined(CONFIG_PJMEDIA_PHASE3_LINK_PROBE)
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
