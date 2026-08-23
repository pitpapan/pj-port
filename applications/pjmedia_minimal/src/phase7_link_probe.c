#include <zephyr/sys/printk.h>

#include <pjlib.h>
#include <pjmedia/alaw_ulaw.h>
#include <pjmedia/endpoint.h>
#include <pjmedia/event.h>
#include <pjmedia/g711.h>
#include <pjmedia/plc.h>
#include <pjmedia/port.h>
#include <pjmedia/silencedet.h>
#include <pjmedia/wsola.h>

/* The volatile guard retains normal call relocations under --gc-sections
 * without executing calls whose arguments are intentionally placeholders. */
static volatile pj_bool_t invoke_probe_calls;

int phase7_link_probe_run(void)
{
	pj_pool_factory *factory = NULL;
	pj_pool_t *pool = NULL;
	pj_ioqueue_t *ioqueue = NULL;
	pjmedia_endpt *endpt = NULL;
	pjmedia_codec_mgr *codec_mgr = NULL;
	pjmedia_codec_factory *codec_factory = NULL;
	pjmedia_codec_info codec_info;
	pjmedia_codec_info infos[2];
	const pjmedia_codec_info *info_ptrs[2];
	pjmedia_codec_param codec_param;
	pjmedia_codec *codec = NULL;
	pjmedia_event_mgr *event_mgr = NULL;
	pjmedia_event event;
	pjmedia_format format;
	pjmedia_port port;
	pjmedia_frame frame;
	pjmedia_plc *plc = NULL;
	pjmedia_wsola *wsola = NULL;
	pjmedia_silence_det *silence = NULL;
	pjmedia_sdp_session *sdp = NULL;
	pjmedia_sdp_media *media = NULL;
	pjmedia_sock_info sock_info;
	pjmedia_endpt_create_sdp_param sdp_param;
	pj_timestamp timestamp;
	pj_str_t name = pj_str("phase7");
	pj_str_t id = pj_str("PCMU/8000/1");
	pj_int16_t samples[80];
	pj_uint8_t bytes[80];
	char id_buffer[32];
	unsigned count = 2;
	unsigned priorities[2];
	unsigned erase_count = 80;
	pj_bool_t flag = PJ_FALSE;

	if (invoke_probe_calls) {
		pjmedia_endpt_create_sdp_param_default(&sdp_param);
		(void)pjmedia_endpt_create2(factory, ioqueue, 0, &endpt);
		(void)pjmedia_endpt_destroy2(endpt);
		(void)pjmedia_endpt_set_flag(endpt,
			PJMEDIA_ENDPT_HAS_TELEPHONE_EVENT_FLAG, &flag);
		(void)pjmedia_endpt_get_flag(endpt,
			PJMEDIA_ENDPT_HAS_TELEPHONE_EVENT_FLAG, &flag);
		(void)pjmedia_endpt_get_ioqueue(endpt);
		(void)pjmedia_endpt_get_thread_count(endpt);
		(void)pjmedia_endpt_get_thread(endpt, 0);
		(void)pjmedia_endpt_stop_threads(endpt);
		(void)pjmedia_endpt_create_pool(endpt, "probe", 1, 1);
		(void)pjmedia_endpt_get_codec_mgr(endpt);
		(void)pjmedia_endpt_create_sdp(endpt, pool, 1, &sock_info, &sdp);
		(void)pjmedia_endpt_create_base_sdp(endpt, pool, &name,
			(const pj_sockaddr *)&sock_info.rtp_addr_name, &sdp);
		(void)pjmedia_endpt_create_audio_sdp(endpt, pool, &sock_info,
			&sdp_param, &media);
		(void)pjmedia_endpt_create_text_sdp(endpt, pool, &sock_info,
			&sdp_param, &media);
		(void)pjmedia_endpt_dump(endpt);
		(void)pjmedia_endpt_atexit(endpt, NULL);

		(void)pjmedia_codec_param_clone(pool, &codec_param);
		(void)pjmedia_codec_mgr_init(codec_mgr, factory);
		(void)pjmedia_codec_mgr_destroy(codec_mgr);
		(void)pjmedia_codec_mgr_register_factory(codec_mgr, codec_factory);
		(void)pjmedia_codec_mgr_unregister_factory(codec_mgr, codec_factory);
		(void)pjmedia_codec_mgr_enum_codecs(codec_mgr, &count, infos,
			priorities);
		(void)pjmedia_codec_mgr_get_codec_info(codec_mgr, 0, &info_ptrs[0]);
		(void)pjmedia_codec_info_to_id(&codec_info, id_buffer,
			sizeof(id_buffer));
		(void)pjmedia_codec_mgr_find_codecs_by_id(codec_mgr, &id, &count,
			info_ptrs, priorities);
		(void)pjmedia_codec_mgr_set_codec_priority(codec_mgr, &id, 128);
		(void)pjmedia_codec_mgr_get_default_param(codec_mgr, &codec_info,
			&codec_param);
		(void)pjmedia_codec_mgr_set_default_param(codec_mgr, &codec_info,
			&codec_param);
		(void)pjmedia_codec_mgr_alloc_codec(codec_mgr, &codec_info, &codec);
		(void)pjmedia_codec_mgr_dealloc_codec(codec_mgr, codec);

		(void)pjmedia_codec_g711_init(endpt);
		(void)pjmedia_codec_g711_deinit();
		(void)pjmedia_linear2alaw(0);
		(void)pjmedia_alaw2linear(0);
		(void)pjmedia_linear2ulaw(0);
		(void)pjmedia_ulaw2linear(0);
		(void)pjmedia_alaw2ulaw(0);
		(void)pjmedia_ulaw2alaw(0);

		(void)pjmedia_event_mgr_create(pool, PJMEDIA_EVENT_MGR_NO_THREAD,
			&event_mgr);
		(void)pjmedia_event_mgr_instance();
		pjmedia_event_mgr_set_instance(event_mgr);
		pjmedia_event_mgr_destroy(event_mgr);
		pjmedia_event_init(&event, PJMEDIA_EVENT_NONE, &timestamp, endpt);
		(void)pjmedia_event_subscribe(event_mgr, NULL, NULL, endpt);
		(void)pjmedia_event_unsubscribe(event_mgr, NULL, NULL, endpt);
		(void)pjmedia_event_publish(event_mgr, endpt, &event,
			PJMEDIA_EVENT_PUBLISH_DEFAULT);

		(void)pjmedia_format_copy(&format, &format);
		(void)pjmedia_format_get_audio_format_detail(&format, PJ_FALSE);
		(void)pjmedia_port_info_init(&port.info, &name, 0, 8000, 1, 16, 80);
		(void)pjmedia_port_info_init2(&port.info, &name, 0,
			PJMEDIA_DIR_ENCODING_DECODING, &format);
		(void)pjmedia_port_get_clock_src(&port, PJMEDIA_DIR_ENCODING);
		(void)pjmedia_port_get_frame(&port, &frame);
		(void)pjmedia_port_put_frame(&port, &frame);
		(void)pjmedia_port_destroy(&port);
		(void)pjmedia_port_init_grp_lock(&port, pool, NULL);
		(void)pjmedia_port_add_ref(&port);
		(void)pjmedia_port_dec_ref(&port);
		(void)pjmedia_port_add_destroy_handler(&port, NULL, NULL);
		(void)pjmedia_port_del_destroy_handler(&port, NULL, NULL);

		(void)pjmedia_plc_create(pool, 8000, 80, 0, &plc);
		(void)pjmedia_plc_save(plc, samples);
		(void)pjmedia_plc_generate(plc, samples);
		(void)pjmedia_wsola_create(pool, 8000, 80, 1, 0, &wsola);
		(void)pjmedia_wsola_set_max_expand(wsola, 80);
		(void)pjmedia_wsola_destroy(wsola);
		(void)pjmedia_wsola_reset(wsola, 0);
		(void)pjmedia_wsola_save(wsola, samples, PJ_FALSE);
		(void)pjmedia_wsola_generate(wsola, samples);
		(void)pjmedia_wsola_discard(wsola, samples, 80, NULL, 0,
			&erase_count);

		(void)pjmedia_silence_det_create(pool, 8000, 80, &silence);
		(void)pjmedia_silence_det_set_name(silence, "probe");
		(void)pjmedia_silence_det_set_fixed(silence, 1000);
		(void)pjmedia_silence_det_set_adaptive(silence, 1000);
		(void)pjmedia_silence_det_set_params(silence, 1, 1, 1);
		(void)pjmedia_silence_det_disable(silence);
		(void)pjmedia_silence_det_detect(silence, samples, 80, NULL);
		(void)pjmedia_calc_avg_signal(samples, 80);
		(void)pjmedia_silence_det_apply(silence, 0);
		pjmedia_ulaw_encode(bytes, samples, 80);
		pjmedia_alaw_encode(bytes, samples, 80);
		pjmedia_ulaw_decode(samples, bytes, 80);
		pjmedia_alaw_decode(samples, bytes, 80);
	}

	printk("PHASE 7 LINK PROBE: PASSED (endpoint/G.711 public closure retained)\n");
	return 0;
}
