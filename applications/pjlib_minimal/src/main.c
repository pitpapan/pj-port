#include <pjlib.h>
#include <zephyr/sys/printk.h>

#if defined(CONFIG_PJLIB_STAGE5_PROBE)
int posix_probe_run(void);
#endif

#if defined(CONFIG_PJLIB_STAGE8_TEST)
int stage8_core_run(void);
#endif

#if defined(CONFIG_PJLIB_STAGE9_TEST)
int stage9_network_run(void);
#endif

#if defined(CONFIG_PJLIB_STAGE10_TEST)
int stage10_ioqueue_run(void);
#endif

int main(void)
{
	pj_status_t status;

	printk("PJLIB minimal Zephyr application started\n");

#if defined(CONFIG_PJLIB_STAGE8_TEST)
	return stage8_core_run();
#endif

#if defined(CONFIG_PJLIB_STAGE9_TEST)
	return stage9_network_run();
#endif


#if defined(CONFIG_PJLIB_STAGE10_TEST)
	return stage10_ioqueue_run();
#endif

#if defined(CONFIG_PJLIB_STAGE5_PROBE)
	if (posix_probe_run() != 0) {
		printk("Stage 5 POSIX runtime probes: FAILED\n");
		return 1;
	}

	printk("Stage 5 POSIX runtime probes: PASSED\n");
#endif

	status = pj_init();
	if (status != PJ_SUCCESS) {
		printk("PJLIB initialization failed: %d\n", status);
		return 1;
	}

	printk("PJLIB initialization successful\n");
	pj_shutdown();
	printk("PJLIB shutdown successful\n");

	return 0;
}
