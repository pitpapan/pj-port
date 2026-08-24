#include <zephyr/sys/printk.h>

#include <pjlib.h>
#include <pjmedia/stream.h>

static volatile pj_bool_t invoke_probe_calls;

int phase11_link_probe_run(void)
{
	pjmedia_stream *stream = NULL;
	pjmedia_stream_info info;
	pjmedia_port *port = NULL;
	pjmedia_rtcp_stat stat;
	pjmedia_jb_state jb;
	pj_str_t digits = pj_str("5");
	pj_status_t status = pj_init();

	pj_bzero(&info, sizeof(info));
	if (status == PJ_SUCCESS && invoke_probe_calls) {
		(void)pjmedia_stream_create(NULL, NULL, &info, NULL, NULL, &stream);
		(void)pjmedia_stream_get_port(stream, &port);
		(void)pjmedia_stream_get_info(stream, &info);
		(void)pjmedia_stream_get_stat(stream, &stat);
		(void)pjmedia_stream_get_stat_jbuf(stream, &jb);
		(void)pjmedia_stream_start(stream);
		(void)pjmedia_stream_pause(stream, PJMEDIA_DIR_ENCODING_DECODING);
		(void)pjmedia_stream_resume(stream, PJMEDIA_DIR_ENCODING_DECODING);
		(void)pjmedia_stream_dial_dtmf(stream, &digits);
		(void)pjmedia_stream_destroy(stream);
	}
	pj_shutdown();
	if (status != PJ_SUCCESS)
		return 1;
	printk("PHASE 11 LINK PROBE: PASSED (integrated G.711 stream closure retained)\n");
	return 0;
}
