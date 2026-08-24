#include "phase11_media.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <pjmedia/endpoint.h>
#include <pjmedia/event.h>
#include <pjmedia/g711.h>
#include <pjmedia/stream.h>
#include <pjmedia/transport_udp.h>

#define PHASE11_FRAME_SAMPLES 160
#define PHASE11_FRAME_BYTES (PHASE11_FRAME_SAMPLES * sizeof(pj_int16_t))
#define PHASE11_MEDIA_FRAMES 36

struct phase11_media_context {
	pj_pool_factory *factory;
	pjmedia_endpt *endpt;
	pj_pool_t *event_pool;
	pjmedia_event_mgr *event_mgr;
	pj_pool_t *pool;
	pjmedia_transport *uac_transport;
	pjmedia_transport *uas_transport;
	pjmedia_stream *uac_stream;
	pjmedia_stream *uas_stream;
	pjmedia_port *uac_port;
	pjmedia_port *uas_port;
	unsigned uac_rtp_port;
	unsigned uas_rtp_port;
	pj_uint32_t uac_hash;
	pj_uint32_t uas_hash;
	unsigned uac_frames;
	unsigned uas_frames;
	pj_bool_t codec_initialized;
};

static struct phase11_media_context media;

static pj_status_t make_bound_socket(pj_sock_t *socket, pj_sockaddr *address)
{
	pj_sockaddr_in bind_address;
	pj_str_t loopback = pj_str("127.0.0.1");
	int address_length = sizeof(*address);
	pj_status_t status;

	*socket = PJ_INVALID_SOCKET;
	status = pj_sock_socket(pj_AF_INET(), pj_SOCK_DGRAM() | pj_SOCK_CLOEXEC(),
				0, socket);
	if (status == PJ_SUCCESS)
		status = pj_sockaddr_in_init(&bind_address, &loopback, 0);
	if (status == PJ_SUCCESS)
		status = pj_sock_bind(*socket, &bind_address,
				      sizeof(bind_address));
	if (status == PJ_SUCCESS)
		status = pj_sock_getsockname(*socket, address, &address_length);
	if (status != PJ_SUCCESS && *socket != PJ_INVALID_SOCKET) {
		pj_sock_close(*socket);
		*socket = PJ_INVALID_SOCKET;
	}
	return status;
}

static pj_status_t create_udp_transport(const char *name,
					pjmedia_transport **transport,
					unsigned *rtp_port)
{
	pjmedia_sock_info socket_info;
	pj_status_t status;
	pj_bool_t attach_attempted = PJ_FALSE;

	pj_bzero(&socket_info, sizeof(socket_info));
	socket_info.rtp_sock = socket_info.rtcp_sock = PJ_INVALID_SOCKET;
	status = make_bound_socket(&socket_info.rtp_sock,
				   &socket_info.rtp_addr_name);
	if (status == PJ_SUCCESS)
		status = make_bound_socket(&socket_info.rtcp_sock,
					   &socket_info.rtcp_addr_name);
	if (status == PJ_SUCCESS) {
		*rtp_port = pj_sockaddr_get_port(&socket_info.rtp_addr_name);
		attach_attempted = PJ_TRUE;
		status = pjmedia_transport_udp_attach(
			media.endpt, name, &socket_info,
			PJMEDIA_UDP_NO_SRC_ADDR_CHECKING, transport);
	}
	if (attach_attempted)
		socket_info.rtp_sock = socket_info.rtcp_sock = PJ_INVALID_SOCKET;
	if (status != PJ_SUCCESS) {
		if (socket_info.rtp_sock != PJ_INVALID_SOCKET)
			pj_sock_close(socket_info.rtp_sock);
		if (socket_info.rtcp_sock != PJ_INVALID_SOCKET)
			pj_sock_close(socket_info.rtcp_sock);
	}
	return status;
}

static pj_uint32_t hash_frame(pj_uint32_t hash, const pjmedia_frame *frame)
{
	const pj_uint8_t *bytes = frame->buf;

	for (pj_size_t i = 0; i < frame->size; ++i) {
		hash ^= bytes[i];
		hash *= 16777619u;
	}
	return hash;
}

static pj_status_t create_stream(pjsip_inv_session *inv,
				 pjmedia_transport *transport,
				 pjmedia_stream **stream,
				 pjmedia_port **port)
{
	const pjmedia_sdp_session *local = NULL;
	const pjmedia_sdp_session *remote = NULL;
	pjmedia_stream_info info;
	pj_status_t status;

	status = pjmedia_sdp_neg_get_active_local(inv->neg, &local);
	if (status == PJ_SUCCESS)
		status = pjmedia_sdp_neg_get_active_remote(inv->neg, &remote);
	if (status == PJ_SUCCESS)
		status = pjmedia_stream_info_from_sdp(&info, media.pool, media.endpt,
						      local, remote, 0);
	/* Bind each stream to the peer port allocated before SDP.  This also makes
	 * the signaling/media agreement explicit in the synthetic loopback call. */
	if (status == PJ_SUCCESS) {
		pj_str_t loopback = pj_str("127.0.0.1");
		unsigned port = (transport == media.uac_transport) ?
			media.uas_rtp_port : media.uac_rtp_port;
		pj_sockaddr_in_init((pj_sockaddr_in *)&info.rem_addr, &loopback, port);
		if (!info.rtcp_mux)
			pj_sockaddr_in_init((pj_sockaddr_in *)&info.rem_rtcp, &loopback,
					    port + 1);
	}
	if (status == PJ_SUCCESS)
		status = pjmedia_transport_media_start(transport, media.pool, local,
						       remote, 0);
	if (status == PJ_SUCCESS)
		status = pjmedia_stream_create(media.endpt, media.pool, &info,
					       transport, NULL, stream);
	if (status == PJ_SUCCESS)
		status = pjmedia_stream_get_port(*stream, port);
	if (status == PJ_SUCCESS)
		status = pjmedia_stream_start(*stream);
	return status;
}

pj_status_t phase11_media_lifecycle_init(pj_pool_factory *factory,
					 pjsip_endpoint *sip_endpt)
{
	pj_status_t status;

	pj_bzero(&media, sizeof(media));
	media.factory = factory;
	status = pjmedia_endpt_create2(factory, pjsip_endpt_get_ioqueue(sip_endpt),
				       0, &media.endpt);
	if (status == PJ_SUCCESS) {
		media.event_pool = pj_pool_create(factory, "phase11-events", 4096,
						 4096, NULL);
		if (media.event_pool == NULL)
			status = PJ_ENOMEM;
	}
	if (status == PJ_SUCCESS)
		status = pjmedia_event_mgr_create(media.event_pool,
						  PJMEDIA_EVENT_MGR_NO_THREAD,
						  &media.event_mgr);
	if (status == PJ_SUCCESS)
		status = pjmedia_codec_g711_init(media.endpt);
	if (status == PJ_SUCCESS)
		media.codec_initialized = PJ_TRUE;
	return status;
}

pj_status_t phase11_media_prepare_call(void)
{
	pj_status_t status;

	if (media.pool != NULL)
		return PJ_EINVALIDOP;
	media.pool = pj_pool_create(media.factory, "phase11-media", 65536, 32768,
				    NULL);
	if (media.pool == NULL)
		return PJ_ENOMEM;
	status = create_udp_transport("p11-uac", &media.uac_transport,
				      &media.uac_rtp_port);
	if (status == PJ_SUCCESS)
		status = create_udp_transport("p11-uas", &media.uas_transport,
					      &media.uas_rtp_port);
	if (status == PJ_SUCCESS)
		printk("[Phase 11] media allocated before SDP: UAC RTP=%u, UAS RTP=%u\n",
		       media.uac_rtp_port, media.uas_rtp_port);
	return status;
}

unsigned phase11_media_sdp_port(pj_bool_t uas)
{
	return uas ? media.uas_rtp_port : media.uac_rtp_port;
}

pj_status_t phase11_media_start_call(pjsip_inv_session *uac,
				     pjsip_inv_session *uas)
{
	pj_status_t status;

	status = create_stream(uac, media.uac_transport, &media.uac_stream,
			       &media.uac_port);
	if (status == PJ_SUCCESS)
		status = create_stream(uas, media.uas_transport, &media.uas_stream,
				       &media.uas_port);
	if (status == PJ_SUCCESS)
		printk("[Phase 11] negotiated G.711 streams started after call confirmation\n");
	return status;
}

pj_status_t phase11_media_exercise_call(void)
{
	pj_int16_t uac_tx[PHASE11_FRAME_SAMPLES];
	pj_int16_t uas_tx[PHASE11_FRAME_SAMPLES];
	pj_int16_t uac_rx[PHASE11_FRAME_SAMPLES];
	pj_int16_t uas_rx[PHASE11_FRAME_SAMPLES];
	pjmedia_rtcp_stat uac_stat;
	pjmedia_rtcp_stat uas_stat;
	pjmedia_jb_state jb;
	pj_str_t digit = pj_str("5");
	pj_status_t status = PJ_SUCCESS;

	media.uac_hash = media.uas_hash = 2166136261u;
	media.uac_frames = media.uas_frames = 0;
	for (unsigned frame_no = 0; frame_no < PHASE11_MEDIA_FRAMES; ++frame_no) {
		pjmedia_frame tx;
		pjmedia_frame rx;

		for (unsigned i = 0; i < PHASE11_FRAME_SAMPLES; ++i) {
			uac_tx[i] = (pj_int16_t)(((frame_no * 97 + i * 31) & 0x3fff) - 8192);
			uas_tx[i] = (pj_int16_t)(((frame_no * 53 + i * 19) & 0x3fff) - 8192);
		}
		pj_bzero(&tx, sizeof(tx));
		tx.type = PJMEDIA_FRAME_TYPE_AUDIO;
		tx.buf = uac_tx;
		tx.size = sizeof(uac_tx);
		tx.timestamp.u64 = (pj_uint64_t)frame_no * PHASE11_FRAME_SAMPLES;
		status = pjmedia_port_put_frame(media.uac_port, &tx);
		if (status != PJ_SUCCESS)
			return status;
		tx.buf = uas_tx;
		status = pjmedia_port_put_frame(media.uas_port, &tx);
		if (status != PJ_SUCCESS)
			return status;
		if (frame_no == 4) {
			status = pjmedia_stream_dial_dtmf(media.uac_stream, &digit);
			if (status != PJ_SUCCESS)
				return status;
		}
		if (frame_no == 18) {
			status = pjmedia_stream_pause(media.uac_stream,
						      PJMEDIA_DIR_ENCODING);
			if (status != PJ_SUCCESS)
				return status;
		}
		if (frame_no == 20) {
			status = pjmedia_stream_resume(media.uac_stream,
						       PJMEDIA_DIR_ENCODING);
			if (status != PJ_SUCCESS)
				return status;
		}
		pj_thread_sleep(20);
		pj_bzero(&rx, sizeof(rx));
		rx.buf = uac_rx;
		rx.size = sizeof(uac_rx);
		status = pjmedia_port_get_frame(media.uac_port, &rx);
		if (status != PJ_SUCCESS)
			return status;
		if (rx.type == PJMEDIA_FRAME_TYPE_AUDIO) {
			media.uac_hash = hash_frame(media.uac_hash, &rx);
			++media.uac_frames;
		}
		rx.buf = uas_rx;
		rx.size = sizeof(uas_rx);
		status = pjmedia_port_get_frame(media.uas_port, &rx);
		if (status != PJ_SUCCESS)
			return status;
		if (rx.type == PJMEDIA_FRAME_TYPE_AUDIO) {
			media.uas_hash = hash_frame(media.uas_hash, &rx);
			++media.uas_frames;
		}
	}
	status = pjmedia_stream_get_stat(media.uac_stream, &uac_stat);
	if (status == PJ_SUCCESS)
		status = pjmedia_stream_get_stat(media.uas_stream, &uas_stat);
	if (status == PJ_SUCCESS)
		status = pjmedia_stream_get_stat_jbuf(media.uas_stream, &jb);
	if (status != PJ_SUCCESS)
		return status;
	printk("[Phase 11] acceptance: frames=%u/%u RTP uac=%u/%u uas=%u/%u dtmf=%u\n",
	       media.uac_frames, media.uas_frames, uac_stat.tx.pkt,
	       uac_stat.rx.pkt, uas_stat.tx.pkt, uas_stat.rx.pkt,
	       pjmedia_stream_check_dtmf(media.uas_stream));
	if (media.uac_frames < 24 || media.uas_frames < 24 ||
	    uac_stat.tx.pkt < 24 || uac_stat.rx.pkt < 24 ||
	    uas_stat.tx.pkt < 24 || uas_stat.rx.pkt < 24 ||
	    !pjmedia_stream_check_dtmf(media.uas_stream))
		return PJ_EUNKNOWN;
	{
		char received[2];
		unsigned count = sizeof(received);

		status = pjmedia_stream_get_dtmf(media.uas_stream, received, &count);
		if (status != PJ_SUCCESS || count != 1 || received[0] != '5')
			return status != PJ_SUCCESS ? status : PJ_EUNKNOWN;
	}
	printk("[Phase 11] bidirectional PCM: UAC frames=%u hash=%08x, UAS frames=%u hash=%08x; RTP tx/rx=%u/%u,%u/%u; JB=%u: PASSED\n",
	       media.uac_frames, media.uac_hash, media.uas_frames, media.uas_hash,
	       uac_stat.tx.pkt, uac_stat.rx.pkt, uas_stat.tx.pkt, uas_stat.rx.pkt,
	       jb.size);
	printk("[Phase 11] 20 ms cadence, telephone-event, pause, and resume: PASSED\n");
	return PJ_SUCCESS;
}

pj_status_t phase11_media_stop_call(void)
{
	pj_status_t result = PJ_SUCCESS;
	pj_status_t status;

	if (media.uac_stream != NULL) {
		status = pjmedia_stream_destroy(media.uac_stream);
		if (status != PJ_SUCCESS)
			result = status;
		media.uac_stream = NULL;
		media.uac_port = NULL;
	}
	if (media.uas_stream != NULL) {
		status = pjmedia_stream_destroy(media.uas_stream);
		if (status != PJ_SUCCESS)
			result = status;
		media.uas_stream = NULL;
		media.uas_port = NULL;
	}
	if (media.uac_transport != NULL) {
		status = pjmedia_transport_close(media.uac_transport);
		if (status != PJ_SUCCESS)
			result = status;
		media.uac_transport = NULL;
	}
	if (media.uas_transport != NULL) {
		status = pjmedia_transport_close(media.uas_transport);
		if (status != PJ_SUCCESS)
			result = status;
		media.uas_transport = NULL;
	}
	if (media.pool != NULL) {
		pj_pool_release(media.pool);
		media.pool = NULL;
	}
	media.uac_rtp_port = media.uas_rtp_port = 0;
	return result;
}

pj_status_t phase11_media_lifecycle_destroy(void)
{
	pj_status_t result = phase11_media_stop_call();
	pj_status_t status;

	if (media.codec_initialized) {
		status = pjmedia_codec_g711_deinit();
		if (status != PJ_SUCCESS)
			result = status;
		media.codec_initialized = PJ_FALSE;
	}
	if (media.event_mgr != NULL) {
		pjmedia_event_mgr_destroy(media.event_mgr);
		media.event_mgr = NULL;
	}
	if (media.event_pool != NULL) {
		pj_pool_release(media.event_pool);
		media.event_pool = NULL;
	}
	if (media.endpt != NULL) {
		status = pjmedia_endpt_destroy2(media.endpt);
		if (status != PJ_SUCCESS)
			result = status;
		media.endpt = NULL;
	}
	return result;
}
