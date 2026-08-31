cmake_minimum_required(VERSION 3.20.0)

list(APPEND ZEPHYR_EXTRA_MODULES
  "${CMAKE_CURRENT_LIST_DIR}/../../voip"
  "${CMAKE_CURRENT_LIST_DIR}/../../pjproject"
)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(voip_integration)

if(CONFIG_VOIP_PJSUA_PLAN3_COMPONENT_TEST)
  target_include_directories(app PRIVATE ../../voip/include)
  target_compile_options(app PRIVATE -std=c++17)
endif()

target_sources_ifdef(CONFIG_VOIP_SDK_CONTRACT_TEST app PRIVATE
  src/sdk_contract.cpp
)
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
target_sources_ifdef(CONFIG_VOIP_PHASE5_CALL_TEST app PRIVATE
  src/phase5_call.cpp
)
target_sources_ifdef(CONFIG_VOIP_PHASE6_MEDIA_TEST app PRIVATE
  src/phase6_media.cpp
)
target_sources_ifdef(CONFIG_VOIP_PHASE7_SIP_MEDIA_TEST app PRIVATE
  src/phase5_call.cpp
)
target_sources_ifdef(CONFIG_VOIP_PHASE8_HOLD_RECOVERY_TEST app PRIVATE
  src/phase5_call.cpp
)
target_sources_ifdef(CONFIG_VOIP_PHASE9_ROBUSTNESS_TEST app PRIVATE
  src/phase5_call.cpp
)
target_sources_ifdef(CONFIG_VOIP_SRTP_KEY_TEST app PRIVATE
  src/srtp_key_lifecycle.cpp
)
target_sources_ifdef(CONFIG_VOIP_SRTP_CALL_TEST app PRIVATE
  src/phase5_call.cpp
)
target_sources_ifdef(CONFIG_VOIP_PJSUA_LINK_TEST app PRIVATE
  src/pjsua_link.c
)
target_sources_ifdef(CONFIG_VOIP_PJSUA_ARENA_TEST app PRIVATE
  src/pjsua_arena_test.c
)
target_sources_ifdef(CONFIG_VOIP_PJSUA_CAPACITY_TEST app PRIVATE
  src/pjsua_capacity.c
)
target_sources_ifdef(CONFIG_VOIP_PJSUA_PLAN3_COMPONENT_TEST app PRIVATE
  ../../voip/tests/pjsua/PjsuaPlan3TestMain.cpp
  ../../voip/tests/pjsua/PjsuaRuntimeTest.cpp
  ../../voip/tests/pjsua/PjsuaCallbackRouterTest.cpp
  ../../voip/tests/pjsua/PjsuaAccountManagerTest.cpp
  ../../voip/src/pjsua/PjsuaApi.cpp
  ../../voip/src/pjsua/PjsuaRuntime.cpp
  ../../voip/src/pjsua/PjsuaTransportManager.cpp
  ../../voip/src/pjsua/PjsuaCallbackRouter.cpp
  ../../voip/src/pjsua/PjsuaAccountManager.cpp
  ../../voip/src/core/AgentRegistry.cpp
)
target_sources_ifdef(CONFIG_VOIP_CORE_SERVICE_TEST app PRIVATE
  src/core_service.cpp
)
