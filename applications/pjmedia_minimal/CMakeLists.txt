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
