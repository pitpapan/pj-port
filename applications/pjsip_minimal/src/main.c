#include <zephyr/sys/printk.h>

#if defined(CONFIG_PJLIB)
#include <pjlib.h>
#endif

#if defined(CONFIG_PJSIP_PHASE3_TEST)
int phase3_util_run(void);
#endif

#if defined(CONFIG_PJSIP_PHASE4_TEST)
int phase4_core_run(void);
#endif

#if defined(CONFIG_PJSIP_PHASE5_TEST)
int phase5_core_run(void);
#endif

#if defined(CONFIG_PJSIP_PHASE6_TEST)
int phase6_loop_run(void);
#endif

#if defined(CONFIG_PJSIP_PHASE7_TEST)
int phase7_udp_run(void);
#endif

#if defined(CONFIG_PJSIP_PHASE8_TEST)
int phase8_tcp_run(void);
#endif

#if defined(CONFIG_PJSIP_PHASE9_TEST)
int phase9_resolver_run(void);
#endif

int main(void)
{
	printk("PJSIP minimal Zephyr application\n");
#if defined(CONFIG_PJSIP_PHASE9_TEST)
	return phase9_resolver_run();
#elif defined(CONFIG_PJSIP_PHASE8_TEST)
	return phase8_tcp_run();
#elif defined(CONFIG_PJSIP_PHASE7_TEST)
	return phase7_udp_run();
#elif defined(CONFIG_PJSIP_PHASE6_TEST)
	return phase6_loop_run();
#elif defined(CONFIG_PJSIP_PHASE5_TEST)
	return phase5_core_run();
#elif defined(CONFIG_PJSIP_PHASE4_TEST)
	return phase4_core_run();
#elif defined(CONFIG_PJSIP_PHASE3_TEST)
	return phase3_util_run();
#endif

#if defined(CONFIG_PJLIB)
	pj_status_t status = pj_init();

	if (status != PJ_SUCCESS) {
		printk("PJLIB initialization failed: %d\n", status);
		return 1;
	}

	printk("PJLIB initialization successful\n");
	pj_shutdown();
	printk("PJLIB shutdown successful\n");
#else
	printk("PJLIB disabled for component source-boundary validation\n");
#endif

	return 0;
}
