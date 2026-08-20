#include <zephyr/sys/printk.h>

#if defined(CONFIG_PJLIB)
#include <pjlib.h>
#endif

#if defined(CONFIG_PJMEDIA_PHASE1_BOUNDARY_TEST)
int phase1_boundary_run(void);
#endif

int main(void)
{
	printk("PJMEDIA minimal Zephyr application\n");

#if defined(CONFIG_PJMEDIA_PHASE1_BOUNDARY_TEST)
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
