cmake_minimum_required(VERSION 3.20.0)

list(APPEND ZEPHYR_EXTRA_MODULES
  "${CMAKE_CURRENT_LIST_DIR}/../../voip"
  "${CMAKE_CURRENT_LIST_DIR}/../../pjproject"
)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(voip_integration)

target_sources_ifdef(CONFIG_VOIP_PHASE1_TEST app PRIVATE src/main.cpp)
target_sources_ifdef(CONFIG_VOIP_PHASE2_RUNTIME_TEST app PRIVATE
  src/phase2_runtime.cpp
)
target_sources_ifdef(CONFIG_VOIP_PHASE3_ACCOUNT_TEST app PRIVATE
  src/phase3_account.cpp
)
target_sources_ifdef(CONFIG_VOIP_PHASE4_REGISTRATION_TEST app PRIVATE
  src/phase4_registration.cpp
)
