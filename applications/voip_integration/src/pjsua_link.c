#include <pjsua-lib/pjsua.h>
#include <pj_zephyr_pool_arena.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <stdbool.h>

_Static_assert(PJSUA_MAX_ACC == 5, "PJSUA_MAX_ACC must be five");
_Static_assert(PJSUA_MAX_CALLS == 7, "PJSUA_MAX_CALLS must be seven");
_Static_assert(PJSUA_MAX_CONF_PORTS == 12,
	       "PJSUA_MAX_CONF_PORTS must be twelve");
_Static_assert(PJMEDIA_HAS_SRTP == 0, "PJMEDIA SRTP must remain disabled");
_Static_assert(PJSIP_HAS_TLS_TRANSPORT == 0,
	       "PJSIP TLS transport must remain disabled");

#define PJSUA_LIFECYCLES 5

static k_tid_t actor_thread;
static unsigned callback_count;
static bool callback_affinity_ok;

static int expect(int condition, const char *message)
{
	if (!condition) {
		printk("PJSUA LINK CHECK FAILED: %s\n", message);
		return -1;
	}
	return 0;
}

static void record_callback(void)
{
	callback_count++;
	if (k_current_get() != actor_thread)
		callback_affinity_ok = false;
}

static void on_call_state(pjsua_call_id call_id, pjsip_event *event)
{
	PJ_UNUSED_ARG(call_id);
	PJ_UNUSED_ARG(event);
	record_callback();
}

static void on_timer(void *user_data)
{
	PJ_UNUSED_ARG(user_data);
	record_callback();
}

static int run_lifecycle(void)
{
	pj_status_t status;
	pjsua_config ua_cfg;
	pjsua_logging_config log_cfg;
	pjsua_media_config media_cfg;
	struct pj_zephyr_pool_arena_stats before;
	struct pj_zephyr_pool_arena_stats after_start;
	struct pj_zephyr_pool_arena_stats after_destroy;
	unsigned lifecycle;

	actor_thread = k_current_get();
	callback_affinity_ok = true;
	status = pj_zephyr_pool_arena_install();
	if (expect(status == PJ_SUCCESS, "arena install") != 0)
		return -1;

	for (lifecycle = 0; lifecycle < PJSUA_LIFECYCLES; ++lifecycle) {
		callback_count = 0;
		status = pj_zephyr_pool_arena_reset();
		if (expect(status == PJ_SUCCESS, "arena reset before lifecycle") != 0)
			return -1;
		pj_zephyr_pool_arena_get_stats(&before);
		if (expect(before.capacity_bytes == CONFIG_PJSUA_ARENA_BYTES &&
			   before.used_bytes == 0 && before.live_blocks == 0,
			   "arena starts without live blocks") != 0)
			return -1;

		status = pjsua_create();
		if (status != PJ_SUCCESS)
			return -1;

		pjsua_config_default(&ua_cfg);
		pjsua_logging_config_default(&log_cfg);
		pjsua_media_config_default(&media_cfg);
		log_cfg.level = 6;
		ua_cfg.thread_cnt = 0;
		ua_cfg.max_calls = PJSUA_MAX_CALLS;
		ua_cfg.enable_unsolicited_mwi = PJ_FALSE;
		ua_cfg.stun_srv_cnt = 0;
		ua_cfg.enable_upnp = PJ_FALSE;
		ua_cfg.cb.on_call_state = &on_call_state;
		media_cfg.thread_cnt = 0;
		media_cfg.max_media_ports = PJSUA_MAX_CONF_PORTS;
		media_cfg.has_ioqueue = PJ_FALSE;
		media_cfg.conf_threads = 1;
		media_cfg.enable_ice = PJ_FALSE;
		media_cfg.enable_turn = PJ_FALSE;

		if (ua_cfg.max_calls != PJSUA_MAX_CALLS ||
		    media_cfg.max_media_ports != PJSUA_MAX_CONF_PORTS) {
			(void)pjsua_destroy();
			return -1;
		}

		status = pjsua_init(&ua_cfg, &log_cfg, &media_cfg);
		if (status == PJ_SUCCESS && pjsua_set_no_snd_dev() == NULL)
			status = PJ_ENOTFOUND;
		if (status == PJ_SUCCESS)
			status = pjsua_start();
		pj_zephyr_pool_arena_get_stats(&after_start);
		if (expect(after_start.peak_bytes > 0 &&
			   after_start.peak_bytes <= after_start.capacity_bytes,
			   "arena peak is bounded and non-zero") != 0)
			status = PJ_EUNKNOWN;

		/* First poll the bare lifecycle; use a timer only if it was quiet. */
		if (status == PJ_SUCCESS && pjsua_handle_events(10) < 0)
			status = PJ_EUNKNOWN;
		if (status == PJ_SUCCESS && callback_count == 0 &&
		    pjsua_schedule_timer2(&on_timer, NULL, 0) != PJ_SUCCESS)
			status = PJ_EUNKNOWN;
		while (status == PJ_SUCCESS && callback_count == 0)
			if (pjsua_handle_events(10) < 0)
				status = PJ_EUNKNOWN;

		if (callback_count == 0 || !callback_affinity_ok)
			status = PJ_EUNKNOWN;
		if (pjsua_destroy() != PJ_SUCCESS)
			status = PJ_EUNKNOWN;
		pj_zephyr_pool_arena_get_stats(&after_destroy);
		if (expect(after_destroy.capacity_bytes == CONFIG_PJSUA_ARENA_BYTES &&
			   after_destroy.live_blocks == 0 &&
			   after_destroy.used_bytes == 0 &&
			   after_destroy.peak_bytes > 0 &&
			   after_destroy.peak_bytes <= after_destroy.capacity_bytes,
			   "arena is clean after destroy") != 0)
			status = PJ_EUNKNOWN;
		if (status == PJ_SUCCESS)
			printk("PJSUA LINK lifecycle %u: callbacks=%u actor-affine\n",
			       lifecycle + 1, callback_count);
		if (status != PJ_SUCCESS)
			return -1;
	}

	return 0;
}

int main(void)
{
	const int result = run_lifecycle();

	printk("PJSUA LINK RESULT: %s\n",
	       result == 0 ? "PASSED (5 lifecycles, arena clean)" : "FAILED");
	return result;
}
