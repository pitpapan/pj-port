#include <pjlib.h>
#include <zephyr/sys/printk.h>

#if defined(CONFIG_PJLIB_STAGE5_PROBE)
int posix_probe_run(void);
#endif

int main(void)
{
	printk("PJLIB minimal Zephyr application started\n");

#if defined(CONFIG_PJLIB_STAGE5_PROBE)
	if (posix_probe_run() != 0) {
		printk("Stage 5 POSIX runtime probes: FAILED\n");
		return 1;
	}

	printk("Stage 5 POSIX runtime probes: PASSED\n");
#endif
	return 0;
}
