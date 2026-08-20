cmake_minimum_required(VERSION 3.20.0)

list(APPEND ZEPHYR_EXTRA_MODULES
  "${CMAKE_CURRENT_LIST_DIR}/../../pjproject"
)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(pjsip_minimal)

target_sources(app PRIVATE src/main.c)
target_sources_ifdef(CONFIG_PJSIP_PHASE3_TEST app PRIVATE src/phase3_util.c)
target_sources_ifdef(CONFIG_PJSIP_PHASE4_TEST app PRIVATE src/phase4_core.c)
target_sources_ifdef(CONFIG_PJSIP_PHASE5_TEST app PRIVATE src/phase5_core.c)
target_sources_ifdef(CONFIG_PJSIP_PHASE6_TEST app PRIVATE src/phase6_loop.c)
target_sources_ifdef(CONFIG_PJSIP_PHASE7_TEST app PRIVATE src/phase7_udp.c)
target_sources_ifdef(CONFIG_PJSIP_PHASE8_TEST app PRIVATE src/phase8_tcp.c)
