# Audited Zephyr source manifests for the embedded PJPROJECT build.
# Keep these lists explicit; do not replace them with globs.

# Portable PJLIB core. Keep this list explicit so platform backends cannot
# be pulled in accidentally as new files are added upstream.
pj_zephyr_sources(PJLIB_COMMON_SOURCES pjlib/src/pj
  activesock.c
  array.c
  atomic_slist.c
  atomic_queue.cpp
  config.c
  ctype.c
  errno.c
  except.c
  fifobuf.c
  guid.c
  hash.c
  ip_helper_generic.c
  list.c
  lock.c
  log.c
  os_info.c
  os_time_common.c
  os_timestamp_common.c
  pool.c
  pool_buf.c
  pool_caching.c
  pool_dbg.c
  rand.c
  rbtree.c
  sock_common.c
  sock_qos_common.c
  ssl_sock_common.c
  string.c
  timer.c
  types.c
  unittest.c
)

# Zephyr reuses the existing PJLIB POSIX/BSD implementations where their
# contracts have been validated by the PJLIB Zephyr test application.
pj_zephyr_sources(PJLIB_PLATFORM_SOURCES pjlib/src/pj
  addr_resolv_sock.c
  file_access_unistd.c
  file_io_ansi.c
  guid_simple.c
  ioqueue_select.c
  log_writer_stdout.c
  os_core_unix.c
  os_error_unix.c
  os_time_unix.c
  os_timestamp_posix.c
  pool_policy_malloc.c
  sock_bsd.c
  sock_qos_bsd.c
  sock_select.c
)

# Phase 1 proved that this is the complete initial dependency set required by
# PJSIP core. DNS, CLI, HTTP, XML, STUN, and extra cryptographic helpers remain
# outside the base build until a selected feature requires them.
pj_zephyr_sources(PJLIB_UTIL_SOURCES pjlib-util/src/pjlib-util
  errno.c
  scanner.c
  string.c
  md5.c
  crc32.c
  hmac_sha1.c
  sha1.c
  http_client.c
  xml.c
)

# PJSUA's media initialization uses the simple STUN client helpers even when
# STUN is disabled at runtime. Keep them out of minimal PJLIB-UTIL profiles.
pj_zephyr_sources(PJLIB_UTIL_PJSUA_SOURCES pjlib-util/src/pjlib-util
  stun_simple_client.c
  stun_simple.c
)

pj_zephyr_sources(PJLIB_UTIL_DNS_RESOLVER_SOURCES pjlib-util/src/pjlib-util
  dns.c
  resolver.c
  srv_resolver.c
)

pj_zephyr_sources(PJLIB_UTIL_BASE64_SOURCES pjlib-util/src/pjlib-util
  base64.c
)

# Phase 1 froze candidate PJMEDIA source families. Each selected Kconfig gate
# still owns the corresponding audited compile/link closure.
pj_zephyr_sources(PJMEDIA_SDP_SOURCES pjmedia/src/pjmedia
  errno.c
  sdp.c
  sdp_cmp.c
)

pj_zephyr_sources(PJMEDIA_SDP_NEG_SOURCES pjmedia/src/pjmedia
  sdp_neg.c
  codec.c
  stream_common.c
  types.c
)

pj_zephyr_sources(PJMEDIA_ENDPOINT_SOURCES pjmedia/src/pjmedia
  endpoint.c
  codec.c
  event.c
  format.c
  types.c
)

pj_zephyr_sources(PJMEDIA_RTP_RTCP_SOURCES pjmedia/src/pjmedia
  rtp.c
  rtcp.c
  rtcp_fb.c
  jbuf.c
)

pj_zephyr_sources(PJMEDIA_LOOP_TRANSPORT_SOURCES pjmedia/src/pjmedia
  transport_loop.c
)

pj_zephyr_sources(PJMEDIA_UDP_TRANSPORT_SOURCES pjmedia/src/pjmedia
  transport_udp.c
)

pj_zephyr_sources(PJMEDIA_STREAM_SOURCES pjmedia/src/pjmedia
  av_sync.c
  stream_common.c
  stream_info.c
  stream.c
)

pj_zephyr_sources(PJMEDIA_G711_SOURCES pjmedia/src/pjmedia
  g711.c
  alaw_ulaw.c
  plc_common.c
  wsola.c
  silencedet.c
  port.c
)

pj_zephyr_sources(PJMEDIA_SRTP_SOURCES third_party/srtp
  srtp/srtp.c
  crypto/cipher/cipher.c
  crypto/cipher/null_cipher.c
  crypto/cipher/cipher_test_cases.c
  crypto/cipher/aes.c
  crypto/cipher/aes_icm.c
  crypto/hash/null_auth.c
  crypto/hash/auth.c
  crypto/hash/auth_test_cases.c
  crypto/hash/sha1.c
  crypto/hash/hmac.c
  crypto/replay/rdb.c
  crypto/replay/rdbx.c
  crypto/math/datatypes.c
  crypto/kernel/crypto_kernel.c
  crypto/kernel/alloc.c
  crypto/kernel/key.c
  pjlib/srtp_err.c
)

pj_zephyr_sources(PJMEDIA_SRTP_TRANSPORT_SOURCES pjmedia/src/pjmedia
  transport_srtp.c
)

# This is the Phase 1 audited PJSIP core set. Keep loop transport in the base
# for socket-free validation; add real transports only by Kconfig.
pj_zephyr_sources(PJSIP_CORE_SOURCES pjsip/src/pjsip
  sip_config.c
  sip_multipart.c
  sip_errno.c
  sip_msg.c
  sip_parser.c
  sip_tel_uri.c
  sip_uri.c
  sip_endpoint.c
  sip_util.c
  sip_util_proxy.c
  sip_resolve.c
  sip_transport.c
  sip_transport_loop.c
  sip_auth_client.c
  sip_auth_msg.c
  sip_auth_parser.c
  sip_auth_server.c
  sip_transaction.c
  sip_util_statefull.c
  sip_dialog.c
  sip_ua_layer.c
)

pj_zephyr_sources(PJSIP_UDP_TRANSPORT_SOURCES pjsip/src/pjsip
  sip_transport_udp.c
)

pj_zephyr_sources(PJSIP_TCP_TRANSPORT_SOURCES pjsip/src/pjsip
  sip_transport_tcp.c
)

pj_zephyr_sources(PJSIP_INVITE_SOURCES pjsip/src/pjsip-ua
  sip_inv.c
  sip_100rel.c
  sip_timer.c
)

pj_zephyr_sources(PJSIP_REGC_SOURCES pjsip/src/pjsip-ua
  sip_reg.c
)

pj_zephyr_sources(PJNATH_SOURCES pjnath/src/pjnath
  errno.c ice_session.c ice_strans.c nat_detect.c stun_auth.c
  stun_msg.c stun_msg_dump.c stun_session.c stun_sock.c
  stun_transaction.c turn_session.c turn_sock.c upnp.c)

pj_zephyr_sources(PJSIP_UA_SOURCES pjsip/src/pjsip-ua
  sip_inv.c sip_reg.c sip_replaces.c sip_xfer.c
  sip_100rel.c sip_timer.c sip_siprec.c)

pj_zephyr_sources(PJSIP_SIMPLE_SOURCES pjsip/src/pjsip-simple
  errno.c evsub.c evsub_msg.c iscomposing.c mwi.c pidf.c
  dialog_info.c presence.c dlg_event.c presence_body.c
  publishc.c rpid.c xpidf.c)

pj_zephyr_sources(PJSUA_SOURCES pjsip/src/pjsua-lib
  pjsua_acc.c pjsua_aud.c pjsua_call.c pjsua_core.c pjsua_dump.c
  pjsua_im.c pjsua_media.c pjsua_pres.c pjsua_txt.c pjsua_vid.c)

pj_zephyr_sources(PJMEDIA_PJSUA_SOURCES pjmedia/src/pjmedia
  audiodev.c bidirectional.c clock_thread.c conference.c converter.c
  delaybuf.c echo_common.c echo_port.c echo_suppress.c master_port.c
  mem_capture.c mem_player.c null_port.c resample_port.c session.c
  sound_port.c splitcomb.c stereo_port.c tonegen.c transport_ice.c
  txt_stream.c wav_player.c wav_playlist.c wav_writer.c wave.c)

pj_zephyr_sources(PJMEDIA_PJSUA_RESAMPLE_SOURCES pjmedia/src/pjmedia
  resample_resample.c)

pj_zephyr_sources(PJMEDIA_PJSUA_CODEC_SOURCES pjmedia/src/pjmedia-codec
  audio_codecs.c)

pj_zephyr_sources(PJMEDIA_AUDIODEV_SOURCES pjmedia/src/pjmedia-audiodev
  audiodev.c errno.c null_dev.c)
