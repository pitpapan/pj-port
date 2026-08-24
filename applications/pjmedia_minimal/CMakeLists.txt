cmake_minimum_required(VERSION 3.20.0)

list(APPEND ZEPHYR_EXTRA_MODULES
  "${CMAKE_CURRENT_LIST_DIR}/../../pjproject"
)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(pjmedia_minimal)

target_sources(app PRIVATE src/main.c)
target_sources_ifdef(CONFIG_PJMEDIA_PHASE1_BOUNDARY_TEST app PRIVATE
  src/phase1_boundary.c
)
target_sources_ifdef(CONFIG_PJMEDIA_PHASE2_SDP_TEST app PRIVATE
  src/phase2_sdp.c
)
target_sources_ifdef(CONFIG_PJMEDIA_PHASE3_SDP_NEG_TEST app PRIVATE
  src/phase3_sdp_neg.c
)
target_sources_ifdef(CONFIG_PJMEDIA_PHASE3_LINK_PROBE app PRIVATE
  src/phase3_link_probe.c
)
target_sources_ifdef(CONFIG_PJMEDIA_PHASE4_INVITE_TEST app PRIVATE
  src/phase4_invite.c
)
target_sources_ifdef(CONFIG_PJMEDIA_PHASE5_LOOP_CALL_TEST app PRIVATE
  src/phase5_loop_call.c
)
target_sources_ifdef(CONFIG_PJMEDIA_PHASE6_UDP_CALL_TEST app PRIVATE
  src/phase6_udp_call.c
)
target_sources_ifdef(CONFIG_PJMEDIA_PHASE7_G711_TEST app PRIVATE
  src/phase7_g711.c
)
target_sources_ifdef(CONFIG_PJMEDIA_PHASE7_LINK_PROBE app PRIVATE
  src/phase7_link_probe.c
)
target_sources_ifdef(CONFIG_PJMEDIA_PHASE8_PACKET_TEST app PRIVATE
  src/phase8_packet.c
)
target_sources_ifdef(CONFIG_PJMEDIA_PHASE8_LINK_PROBE app PRIVATE
  src/phase8_link_probe.c
)
target_sources_ifdef(CONFIG_PJMEDIA_PHASE9_TRANSPORT_TEST app PRIVATE
  src/phase9_transport.c
)
target_sources_ifdef(CONFIG_PJMEDIA_PHASE9_LINK_PROBE app PRIVATE
  src/phase9_link_probe.c
)
target_sources_ifdef(CONFIG_PJMEDIA_PHASE11_LINK_PROBE app PRIVATE
  src/phase11_link_probe.c
)
target_sources_ifdef(CONFIG_PJMEDIA_PHASE11_CALL_TEST app PRIVATE
  src/phase11_call.c
  src/phase11_media.c
)
target_sources_ifdef(CONFIG_PJMEDIA_PHASE12_ROBUSTNESS_TEST app PRIVATE
  src/phase12_robustness.c
  src/phase11_media.c
)
