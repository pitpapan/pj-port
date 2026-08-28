#include <pjsua-lib/pjsua.h>

#include <zephyr/sys/printk.h>

static int run_lifecycle(void)
{
	pj_status_t status;
	pjsua_config ua_cfg;
	pjsua_logging_config log_cfg;
	pjsua_media_config media_cfg;

	status = pjsua_create();
	if (status != PJ_SUCCESS)
		return -1;

	pjsua_config_default(&ua_cfg);
	pjsua_logging_config_default(&log_cfg);
	pjsua_media_config_default(&media_cfg);
	log_cfg.level = 6;
	ua_cfg.thread_cnt = 0;
	ua_cfg.max_calls = 7;
	ua_cfg.enable_unsolicited_mwi = PJ_FALSE;
	ua_cfg.stun_srv_cnt = 0;
	ua_cfg.enable_upnp = PJ_FALSE;
	media_cfg.thread_cnt = 0;
	media_cfg.max_media_ports = 12;
	media_cfg.has_ioqueue = PJ_FALSE;
	media_cfg.conf_threads = 1;
	media_cfg.enable_ice = PJ_FALSE;
	media_cfg.enable_turn = PJ_FALSE;

	status = pjsua_init(&ua_cfg, &log_cfg, &media_cfg);
	if (status == PJ_SUCCESS && pjsua_set_no_snd_dev() == NULL)
		status = PJ_ENOTFOUND;
	if (status == PJ_SUCCESS)
		status = pjsua_start();
	if (status == PJ_SUCCESS && pjsua_handle_events(0) < 0)
		status = PJ_EUNKNOWN;

	if (pjsua_destroy() != PJ_SUCCESS)
		status = PJ_EUNKNOWN;
	return status == PJ_SUCCESS ? 0 : -1;
}

int main(void)
{
	const int result = run_lifecycle();

	printk("PJSUA LINK RESULT: %s\n", result == 0 ? "PASSED" : "FAILED");
	return result;
}
