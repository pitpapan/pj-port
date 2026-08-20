# Zephyr-only PJLIB integration. The PJPROJECT root CMakeLists.txt remains the
# upstream entry point for Linux, Windows, macOS, and other native builds.

get_filename_component(PJPROJECT_ROOT_DIR
  "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE
)

if(CONFIG_PJPROJECT AND CONFIG_PJLIB)
  message(STATUS "PJPROJECT Zephyr module: PJLIB enabled")

  # Portable PJLIB core. Keep this list explicit so platform backends cannot
  # be pulled in accidentally as new files are added upstream.
  set(PJLIB_COMMON_SOURCES
    ${PJPROJECT_ROOT_DIR}/pjlib/src/pj/activesock.c
    ${PJPROJECT_ROOT_DIR}/pjlib/src/pj/array.c
    ${PJPROJECT_ROOT_DIR}/pjlib/src/pj/atomic_slist.c
    ${PJPROJECT_ROOT_DIR}/pjlib/src/pj/atomic_queue.cpp
    ${PJPROJECT_ROOT_DIR}/pjlib/src/pj/config.c
    ${PJPROJECT_ROOT_DIR}/pjlib/src/pj/ctype.c
    ${PJPROJECT_ROOT_DIR}/pjlib/src/pj/errno.c
    ${PJPROJECT_ROOT_DIR}/pjlib/src/pj/except.c
    ${PJPROJECT_ROOT_DIR}/pjlib/src/pj/fifobuf.c
    ${PJPROJECT_ROOT_DIR}/pjlib/src/pj/guid.c
    ${PJPROJECT_ROOT_DIR}/pjlib/src/pj/hash.c
    ${PJPROJECT_ROOT_DIR}/pjlib/src/pj/ip_helper_generic.c
    ${PJPROJECT_ROOT_DIR}/pjlib/src/pj/list.c
    ${PJPROJECT_ROOT_DIR}/pjlib/src/pj/lock.c
    ${PJPROJECT_ROOT_DIR}/pjlib/src/pj/log.c
    ${PJPROJECT_ROOT_DIR}/pjlib/src/pj/os_info.c
    ${PJPROJECT_ROOT_DIR}/pjlib/src/pj/os_time_common.c
    ${PJPROJECT_ROOT_DIR}/pjlib/src/pj/os_timestamp_common.c
    ${PJPROJECT_ROOT_DIR}/pjlib/src/pj/pool.c
    ${PJPROJECT_ROOT_DIR}/pjlib/src/pj/pool_buf.c
    ${PJPROJECT_ROOT_DIR}/pjlib/src/pj/pool_caching.c
    ${PJPROJECT_ROOT_DIR}/pjlib/src/pj/pool_dbg.c
    ${PJPROJECT_ROOT_DIR}/pjlib/src/pj/rand.c
    ${PJPROJECT_ROOT_DIR}/pjlib/src/pj/rbtree.c
    ${PJPROJECT_ROOT_DIR}/pjlib/src/pj/sock_common.c
    ${PJPROJECT_ROOT_DIR}/pjlib/src/pj/sock_qos_common.c
    ${PJPROJECT_ROOT_DIR}/pjlib/src/pj/ssl_sock_common.c
    ${PJPROJECT_ROOT_DIR}/pjlib/src/pj/string.c
    ${PJPROJECT_ROOT_DIR}/pjlib/src/pj/timer.c
    ${PJPROJECT_ROOT_DIR}/pjlib/src/pj/types.c
    ${PJPROJECT_ROOT_DIR}/pjlib/src/pj/unittest.c
  )

  # Zephyr reuses the existing PJLIB POSIX/BSD implementations where their
  # contracts have been validated by the PJLIB Zephyr test application.
  set(PJLIB_PLATFORM_SOURCES
    ${PJPROJECT_ROOT_DIR}/pjlib/src/pj/addr_resolv_sock.c
    ${PJPROJECT_ROOT_DIR}/pjlib/src/pj/file_access_unistd.c
    ${PJPROJECT_ROOT_DIR}/pjlib/src/pj/file_io_ansi.c
    ${PJPROJECT_ROOT_DIR}/pjlib/src/pj/guid_simple.c
    ${PJPROJECT_ROOT_DIR}/pjlib/src/pj/ioqueue_select.c
    ${PJPROJECT_ROOT_DIR}/pjlib/src/pj/log_writer_stdout.c
    ${PJPROJECT_ROOT_DIR}/pjlib/src/pj/os_core_unix.c
    ${PJPROJECT_ROOT_DIR}/pjlib/src/pj/os_error_unix.c
    ${PJPROJECT_ROOT_DIR}/pjlib/src/pj/os_time_unix.c
    ${PJPROJECT_ROOT_DIR}/pjlib/src/pj/os_timestamp_posix.c
    ${PJPROJECT_ROOT_DIR}/pjlib/src/pj/pool_policy_malloc.c
    ${PJPROJECT_ROOT_DIR}/pjlib/src/pj/sock_bsd.c
    ${PJPROJECT_ROOT_DIR}/pjlib/src/pj/sock_qos_bsd.c
    ${PJPROJECT_ROOT_DIR}/pjlib/src/pj/sock_select.c
  )

  zephyr_library_named(pjlib)
  zephyr_library_sources(
    ${PJLIB_COMMON_SOURCES}
    ${PJLIB_PLATFORM_SOURCES}
  )
  zephyr_library_include_directories(${PJPROJECT_ROOT_DIR}/pjlib/include)
  zephyr_include_directories(${PJPROJECT_ROOT_DIR}/pjlib/include)
endif()

if(CONFIG_PJPROJECT AND CONFIG_PJLIB_UTIL)
  message(STATUS "PJPROJECT Zephyr module: PJLIB-UTIL enabled")

  # Phase 1 proved that this is the complete initial dependency set required
  # by PJSIP core. DNS, CLI, HTTP, XML, STUN, and extra cryptographic helpers
  # remain outside the build until a selected feature requires them.
  set(PJLIB_UTIL_SOURCES
    ${PJPROJECT_ROOT_DIR}/pjlib-util/src/pjlib-util/errno.c
    ${PJPROJECT_ROOT_DIR}/pjlib-util/src/pjlib-util/scanner.c
    ${PJPROJECT_ROOT_DIR}/pjlib-util/src/pjlib-util/string.c
    ${PJPROJECT_ROOT_DIR}/pjlib-util/src/pjlib-util/md5.c
  )

  zephyr_library_named(pjlib-util)
  zephyr_library_sources(${PJLIB_UTIL_SOURCES})
  zephyr_library_sources_ifdef(CONFIG_PJLIB_UTIL_DNS_RESOLVER
    ${PJPROJECT_ROOT_DIR}/pjlib-util/src/pjlib-util/dns.c
    ${PJPROJECT_ROOT_DIR}/pjlib-util/src/pjlib-util/resolver.c
    ${PJPROJECT_ROOT_DIR}/pjlib-util/src/pjlib-util/srv_resolver.c
  )
  zephyr_library_include_directories(
    ${PJPROJECT_ROOT_DIR}/pjlib-util/include
    ${PJPROJECT_ROOT_DIR}/pjlib/include
  )
  zephyr_include_directories(${PJPROJECT_ROOT_DIR}/pjlib-util/include)
  target_link_libraries(pjlib-util PUBLIC pjlib)
endif()

if(CONFIG_PJPROJECT AND CONFIG_PJMEDIA)
  message(STATUS "PJPROJECT Zephyr module: PJMEDIA configuration boundary enabled")

  # Phase 1 freezes candidate source families but deliberately does not pass
  # any of them to zephyr_library_sources(). Each later phase audits its full
  # compile/link closure before activating the corresponding list. Overlap
  # between candidate lists is resolved when the first consumer is enabled.
  set(PJMEDIA_SDP_SOURCES
    ${PJPROJECT_ROOT_DIR}/pjmedia/src/pjmedia/errno.c
    ${PJPROJECT_ROOT_DIR}/pjmedia/src/pjmedia/sdp.c
    ${PJPROJECT_ROOT_DIR}/pjmedia/src/pjmedia/sdp_cmp.c
  )
  set(PJMEDIA_SDP_NEG_SOURCES
    ${PJPROJECT_ROOT_DIR}/pjmedia/src/pjmedia/sdp_neg.c
    ${PJPROJECT_ROOT_DIR}/pjmedia/src/pjmedia/codec.c
    ${PJPROJECT_ROOT_DIR}/pjmedia/src/pjmedia/stream_common.c
    ${PJPROJECT_ROOT_DIR}/pjmedia/src/pjmedia/types.c
  )
  set(PJMEDIA_ENDPOINT_SOURCES
    ${PJPROJECT_ROOT_DIR}/pjmedia/src/pjmedia/endpoint.c
    ${PJPROJECT_ROOT_DIR}/pjmedia/src/pjmedia/codec.c
    ${PJPROJECT_ROOT_DIR}/pjmedia/src/pjmedia/event.c
    ${PJPROJECT_ROOT_DIR}/pjmedia/src/pjmedia/format.c
    ${PJPROJECT_ROOT_DIR}/pjmedia/src/pjmedia/types.c
  )
  set(PJMEDIA_RTP_RTCP_SOURCES
    ${PJPROJECT_ROOT_DIR}/pjmedia/src/pjmedia/rtp.c
    ${PJPROJECT_ROOT_DIR}/pjmedia/src/pjmedia/rtcp.c
    ${PJPROJECT_ROOT_DIR}/pjmedia/src/pjmedia/rtcp_fb.c
    ${PJPROJECT_ROOT_DIR}/pjmedia/src/pjmedia/jbuf.c
  )
  set(PJMEDIA_UDP_TRANSPORT_SOURCES
    ${PJPROJECT_ROOT_DIR}/pjmedia/src/pjmedia/transport_udp.c
  )
  set(PJMEDIA_STREAM_SOURCES
    ${PJPROJECT_ROOT_DIR}/pjmedia/src/pjmedia/stream_common.c
    ${PJPROJECT_ROOT_DIR}/pjmedia/src/pjmedia/stream_info.c
    ${PJPROJECT_ROOT_DIR}/pjmedia/src/pjmedia/stream.c
  )
  set(PJMEDIA_G711_SOURCES
    ${PJPROJECT_ROOT_DIR}/pjmedia/src/pjmedia/g711.c
    ${PJPROJECT_ROOT_DIR}/pjmedia/src/pjmedia/alaw_ulaw.c
    ${PJPROJECT_ROOT_DIR}/pjmedia/src/pjmedia/plc_common.c
    ${PJPROJECT_ROOT_DIR}/pjmedia/src/pjmedia/wsola.c
    ${PJPROJECT_ROOT_DIR}/pjmedia/src/pjmedia/silencedet.c
    ${PJPROJECT_ROOT_DIR}/pjmedia/src/pjmedia/port.c
  )
  set(PJMEDIA_AUDIODEV_SOURCES
    ${PJPROJECT_ROOT_DIR}/pjmedia/src/pjmedia-audiodev/audiodev.c
    ${PJPROJECT_ROOT_DIR}/pjmedia/src/pjmedia-audiodev/errno.c
  )
  set(PJMEDIA_AUDIODEV_NULL_SOURCES
    ${PJPROJECT_ROOT_DIR}/pjmedia/src/pjmedia-audiodev/null_dev.c
  )
  set(PJMEDIA_AUDIODEV_ZEPHYR_SOURCES)

  # These unsupported/deferred families intentionally have no Zephyr source
  # selector in the initial profile.
  set(PJMEDIA_VIDEO_SOURCES)
  set(PJMEDIA_SRTP_SOURCES)
  set(PJMEDIA_ICE_SOURCES)
  set(PJMEDIA_RESAMPLE_SOURCES)
  set(PJMEDIA_AEC_SOURCES)
  set(PJMEDIA_LEGACY_SOUND_SOURCES)
  set(PJMEDIA_OTHER_CODEC_SOURCES)

  # Public PJMEDIA headers are visible only when the PJMEDIA boundary is on.
  # The Phase 1 application compiles configuration/header assertions only.
  zephyr_include_directories(${PJPROJECT_ROOT_DIR}/pjmedia/include)
endif()

if(CONFIG_PJPROJECT AND CONFIG_PJSIP)
  message(STATUS "PJPROJECT Zephyr module: PJSIP core enabled")

  # This is the Phase 1 audited PJSIP core set. Keep loop transport in the
  # base for socket-free validation; add real transports only by Kconfig.
  set(PJSIP_CORE_SOURCES
    ${PJPROJECT_ROOT_DIR}/pjsip/src/pjsip/sip_config.c
    ${PJPROJECT_ROOT_DIR}/pjsip/src/pjsip/sip_multipart.c
    ${PJPROJECT_ROOT_DIR}/pjsip/src/pjsip/sip_errno.c
    ${PJPROJECT_ROOT_DIR}/pjsip/src/pjsip/sip_msg.c
    ${PJPROJECT_ROOT_DIR}/pjsip/src/pjsip/sip_parser.c
    ${PJPROJECT_ROOT_DIR}/pjsip/src/pjsip/sip_tel_uri.c
    ${PJPROJECT_ROOT_DIR}/pjsip/src/pjsip/sip_uri.c
    ${PJPROJECT_ROOT_DIR}/pjsip/src/pjsip/sip_endpoint.c
    ${PJPROJECT_ROOT_DIR}/pjsip/src/pjsip/sip_util.c
    ${PJPROJECT_ROOT_DIR}/pjsip/src/pjsip/sip_util_proxy.c
    ${PJPROJECT_ROOT_DIR}/pjsip/src/pjsip/sip_resolve.c
    ${PJPROJECT_ROOT_DIR}/pjsip/src/pjsip/sip_transport.c
    ${PJPROJECT_ROOT_DIR}/pjsip/src/pjsip/sip_transport_loop.c
    ${PJPROJECT_ROOT_DIR}/pjsip/src/pjsip/sip_auth_client.c
    ${PJPROJECT_ROOT_DIR}/pjsip/src/pjsip/sip_auth_msg.c
    ${PJPROJECT_ROOT_DIR}/pjsip/src/pjsip/sip_auth_parser.c
    ${PJPROJECT_ROOT_DIR}/pjsip/src/pjsip/sip_auth_server.c
    ${PJPROJECT_ROOT_DIR}/pjsip/src/pjsip/sip_transaction.c
    ${PJPROJECT_ROOT_DIR}/pjsip/src/pjsip/sip_util_statefull.c
    ${PJPROJECT_ROOT_DIR}/pjsip/src/pjsip/sip_dialog.c
    ${PJPROJECT_ROOT_DIR}/pjsip/src/pjsip/sip_ua_layer.c
  )

  zephyr_library_named(pjsip)
  zephyr_library_sources(${PJSIP_CORE_SOURCES})
  zephyr_library_sources_ifdef(CONFIG_PJSIP_UDP_TRANSPORT
    ${PJPROJECT_ROOT_DIR}/pjsip/src/pjsip/sip_transport_udp.c
  )
  zephyr_library_sources_ifdef(CONFIG_PJSIP_TCP_TRANSPORT
    ${PJPROJECT_ROOT_DIR}/pjsip/src/pjsip/sip_transport_tcp.c
  )
  zephyr_library_sources_ifdef(CONFIG_PJSIP_REGC
    ${PJPROJECT_ROOT_DIR}/pjsip/src/pjsip-ua/sip_reg.c
  )
  zephyr_library_include_directories(
    ${PJPROJECT_ROOT_DIR}/pjsip/include
    ${PJPROJECT_ROOT_DIR}/pjlib-util/include
    ${PJPROJECT_ROOT_DIR}/pjlib/include
  )
  zephyr_include_directories(${PJPROJECT_ROOT_DIR}/pjsip/include)
  target_link_libraries(pjsip PUBLIC pjlib-util pjlib)
endif()
