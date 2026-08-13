cmake_minimum_required(VERSION 3.20.0)

list(APPEND ZEPHYR_EXTRA_MODULES
  "${CMAKE_CURRENT_LIST_DIR}/../../pjproject"
)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(pjlib_minimal)

target_sources(app PRIVATE src/main.c)
target_sources_ifdef(CONFIG_PJLIB_STAGE5_PROBE app PRIVATE src/posix_probe.c)
target_sources_ifdef(CONFIG_PJLIB_STAGE8_TEST app PRIVATE src/stage8_core.c)
