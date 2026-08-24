#include <zephyr/sys/printk.h>

#include <pjlib.h>
#include <pjlib-util.h>
#include <pjmedia/endpoint.h>
#include <pjmedia/transport.h>
#include <pjmedia/transport_loop.h>
#include <pjmedia/transport_udp.h>
#include <pjsip.h>

/* Retain ordinary call relocations under the normal --gc-sections policy. */
static volatile pj_bool_t invoke_probe_calls;

int phase9_link_probe_run(void)
{
	pj_status_t status = pj_init();
	pjmedia_loop_tp_setting loop_setting;
	pjmedia_sock_info socket_info;
	pjmedia_transport *transport = NULL;
	pjmedia_endpt *endpt = NULL;
	pj_str_t address = pj_str("127.0.0.1");

	if (status != PJ_SUCCESS)
		return 1;
	status = pjlib_util_init();
	if (status == PJ_SUCCESS && invoke_probe_calls) {
		pjmedia_loop_tp_setting_default(&loop_setting);
		(void)pjmedia_transport_loop_create(endpt, &transport);
		(void)pjmedia_transport_loop_create2(endpt, &loop_setting,
						     &transport);
		(void)pjmedia_transport_loop_disable_rx(transport, NULL, PJ_TRUE);
		(void)pjmedia_transport_udp_create(endpt, NULL, 4000, 0,
						   &transport);
		(void)pjmedia_transport_udp_create2(endpt, NULL, &address, 4000,
						    0, &transport);
		(void)pjmedia_transport_udp_create3(endpt, pj_AF_INET(), NULL,
						    &address, 4000, 0,
						    &transport);
		(void)pjmedia_transport_udp_attach(endpt, NULL, &socket_info, 0,
						   &transport);
	}
	pj_shutdown();
	if (status != PJ_SUCCESS)
		return 1;
	printk("PHASE 9 LINK PROBE: PASSED (loop/UDP transport public closure retained)\n");
	return 0;
}
