#include <zephyr/sys/printk.h>

#include <pjlib.h>
#include <pjmedia/jbuf.h>
#include <pjmedia/rtcp.h>
#include <pjmedia/rtcp_fb.h>
#include <pjmedia/rtp.h>

/* Retain ordinary call relocations under the normal --gc-sections policy. */
static volatile pj_bool_t invoke_probe_calls;

int phase8_link_probe_run(void)
{
	pjmedia_rtp_session rtp;
	pjmedia_rtp_session_setting rtp_setting;
	pjmedia_rtp_status rtp_status;
	pjmedia_rtp_seq_session seq_session;
	pjmedia_rtp_hdr rtp_header;
	pjmedia_rtp_dec_hdr decoded_header;
	const pjmedia_rtp_hdr *header = NULL;
	const void *payload = NULL;
	unsigned payload_len = 0;
	int header_len = 0;
	pjmedia_rtcp_session rtcp;
	pjmedia_rtcp_session_setting rtcp_setting;
	pjmedia_rtcp_stat rtcp_stat;
	pjmedia_rtcp_ntp_rec ntp;
	pjmedia_rtcp_sdes sdes;
	pjmedia_rtcp_fb_setting fb_setting;
	pjmedia_rtcp_fb_setting fb_setting_copy;
	pjmedia_rtcp_fb_info fb_info;
	pjmedia_rtcp_fb_info fb_info_copy;
	pjmedia_rtcp_fb_nack nack;
	pjmedia_rtcp_fb_sli sli;
	pjmedia_rtcp_fb_rpsi rpsi;
	pjmedia_jbuf *jbuf = NULL;
	pjmedia_jb_state jb_state;
	pj_pool_t *pool = NULL;
	pjmedia_endpt *endpt = NULL;
	pjmedia_sdp_session *sdp = NULL;
	pj_str_t name = pj_str("phase8-probe");
	pj_str_t reason = pj_str("probe");
	pj_uint8_t buffer[128] = {0};
	pj_size_t size = sizeof(buffer);
	char frame_type = 0;
	pj_uint32_t bit_info = 0;
	pj_uint32_t timestamp = 0;
	int frame_seq = 0;
	pj_bool_t discarded = PJ_FALSE;
	unsigned count = 1;
	void *packet = NULL;
	int packet_len = 0;

	if (invoke_probe_calls) {
		(void)pjmedia_rtp_session_init(&rtp, 0, 1);
		(void)pjmedia_rtp_session_init2(&rtp, rtp_setting);
		(void)pjmedia_rtp_encode_rtp(&rtp, 0, 0, 1, 160,
					       (const void **)&header, &header_len);
		(void)pjmedia_rtp_decode_rtp(&rtp, buffer, sizeof(buffer),
					       &header, &payload, &payload_len);
		(void)pjmedia_rtp_decode_rtp2(&rtp, buffer, sizeof(buffer),
						&header, &decoded_header,
						&payload, &payload_len);
		pjmedia_rtp_session_update(&rtp, &rtp_header, &rtp_status);
		pjmedia_rtp_session_update2(&rtp, &rtp_header, &rtp_status,
					    PJ_FALSE);
		pjmedia_rtp_seq_init(&seq_session, 0);
		pjmedia_rtp_seq_update(&seq_session, 1, &rtp_status);

		pjmedia_rtcp_session_setting_default(&rtcp_setting);
		pjmedia_rtcp_init_stat(&rtcp_stat);
		pjmedia_rtcp_init(&rtcp, "probe", 8000, 160, 1);
		pjmedia_rtcp_init2(&rtcp, &rtcp_setting);
		pjmedia_rtcp_update(&rtcp, &rtcp_setting);
		(void)pjmedia_rtcp_get_ntp_time(&rtcp, &ntp);
		pjmedia_rtcp_rx_rtp(&rtcp, 1, 160, 80);
		pjmedia_rtcp_rx_rtp2(&rtcp, 1, 160, 80, PJ_FALSE);
		pjmedia_rtcp_tx_rtp(&rtcp, 80);
		pjmedia_rtcp_rx_rtcp(&rtcp, buffer, size);
		pjmedia_rtcp_build_rtcp(&rtcp, &packet, &packet_len);
		(void)pjmedia_rtcp_build_rtcp_sdes(&rtcp, buffer, &size, &sdes);
		(void)pjmedia_rtcp_build_rtcp_bye(&rtcp, buffer, &size, &reason);
		(void)pjmedia_rtcp_enable_xr(&rtcp, PJ_FALSE);
		pjmedia_rtcp_fini(&rtcp);

		(void)pjmedia_rtcp_fb_setting_default(&fb_setting);
		pjmedia_rtcp_fb_setting_dup(pool, &fb_setting_copy, &fb_setting);
		pjmedia_rtcp_fb_info_dup(pool, &fb_info_copy, &fb_info);
		(void)pjmedia_rtcp_fb_encode_sdp(pool, endpt, &fb_setting,
						  sdp, 0, NULL);
		(void)pjmedia_rtcp_fb_decode_sdp(pool, endpt, NULL, sdp, 0,
						  &fb_info);
		(void)pjmedia_rtcp_fb_decode_sdp2(pool, endpt, NULL, sdp, 0,
						   -1, &fb_info);
		(void)pjmedia_rtcp_fb_build_nack(&rtcp, buffer, &size, 1, &nack);
		(void)pjmedia_rtcp_fb_build_pli(&rtcp, buffer, &size);
		(void)pjmedia_rtcp_fb_build_sli(&rtcp, buffer, &size, 1, &sli);
		(void)pjmedia_rtcp_fb_build_rpsi(&rtcp, buffer, &size, &rpsi);
		(void)pjmedia_rtcp_fb_parse_nack(buffer, size, &count, &nack);
		(void)pjmedia_rtcp_fb_parse_pli(buffer, size);
		(void)pjmedia_rtcp_fb_parse_sli(buffer, size, &count, &sli);
		(void)pjmedia_rtcp_fb_parse_rpsi(buffer, size, &rpsi);

		(void)pjmedia_jbuf_create(pool, &name, 4, 20, 4, &jbuf);
		(void)pjmedia_jbuf_set_ptime(jbuf, 20);
		(void)pjmedia_jbuf_set_ptime2(jbuf, 40, 2);
		(void)pjmedia_jbuf_set_fixed(jbuf, 1);
		(void)pjmedia_jbuf_set_adaptive(jbuf, 1, 0, 3);
		(void)pjmedia_jbuf_set_discard(jbuf, PJMEDIA_JB_DISCARD_NONE);
		(void)pjmedia_jbuf_reset(jbuf);
		pjmedia_jbuf_put_frame(jbuf, buffer, 4, 1);
		pjmedia_jbuf_put_frame2(jbuf, buffer, 4, 0, 1, &discarded);
		pjmedia_jbuf_put_frame3(jbuf, buffer, 4, 0, 1, 160, &discarded);
		pjmedia_jbuf_get_frame(jbuf, buffer, &frame_type);
		pjmedia_jbuf_get_frame2(jbuf, buffer, &size, &frame_type,
					 &bit_info);
		pjmedia_jbuf_get_frame3(jbuf, buffer, &size, &frame_type,
					 &bit_info, &timestamp, &frame_seq);
		pjmedia_jbuf_peek_frame(jbuf, 0, &payload, &size, &frame_type,
					 &bit_info, &timestamp, &frame_seq);
		(void)pjmedia_jbuf_remove_frame(jbuf, 1);
		(void)pjmedia_jbuf_is_full(jbuf);
		(void)pjmedia_jbuf_get_state(jbuf, &jb_state);
		(void)pjmedia_jbuf_set_min_delay(jbuf, 20);
		(void)pjmedia_jbuf_destroy(jbuf);
	}

	printk("PHASE 8 LINK PROBE: PASSED (RTP/RTCP/jitter public closure retained)\n");
	return 0;
}
