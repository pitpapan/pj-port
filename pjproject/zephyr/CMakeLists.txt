# Zephyr-only PJLIB integration. The PJPROJECT root CMakeLists.txt remains the
# upstream entry point for Linux, Windows, macOS, and other native builds.

get_filename_component(PJPROJECT_ROOT_DIR
  "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE
)

# Keep Zephyr source manifests explicit, but list files relative to the
# PJPROJECT root so the selected embedded closure remains auditable.
function(pj_zephyr_sources output_variable prefix)
  set(paths)
  foreach(source ${ARGN})
    list(APPEND paths "${PJPROJECT_ROOT_DIR}/${prefix}/${source}")
  endforeach()
  set(${output_variable} ${paths} PARENT_SCOPE)
endfunction()

include(${CMAKE_CURRENT_LIST_DIR}/sources.cmake)

if(CONFIG_PJPROJECT AND CONFIG_PJNATH)
  zephyr_include_directories(${PJPROJECT_ROOT_DIR}/pjnath/include)
endif()

# Keep feature-to-source wiring compact while leaving the audited source
# manifests in sources.cmake as the single source of truth.
macro(pj_zephyr_add_sources_ifdef config source_variable)
  zephyr_library_sources_ifdef(${config} ${${source_variable}})
endmacro()

if(CONFIG_PJPROJECT AND CONFIG_PJLIB)
  message(STATUS "PJPROJECT Zephyr module: PJLIB enabled")

  zephyr_library_named(pjlib)
  zephyr_library_sources(
    ${PJLIB_COMMON_SOURCES}
    ${PJLIB_PLATFORM_SOURCES}
  )
  zephyr_library_include_directories(${PJPROJECT_ROOT_DIR}/pjlib/include)
  zephyr_include_directories(${PJPROJECT_ROOT_DIR}/pjlib/include)
  zephyr_include_directories(${CMAKE_CURRENT_LIST_DIR}/include)
  if(CONFIG_PJSUA)
    zephyr_library_sources(
      ${CMAKE_CURRENT_LIST_DIR}/pj_zephyr_pool_arena.c
    )
  endif()
endif()

if(CONFIG_PJPROJECT AND CONFIG_PJLIB_UTIL)
  message(STATUS "PJPROJECT Zephyr module: PJLIB-UTIL enabled")

  zephyr_library_named(pjlib-util)
  zephyr_library_sources(${PJLIB_UTIL_SOURCES})
  if(CONFIG_PJLIB_UTIL_DNS_RESOLVER OR CONFIG_PJSUA)
    zephyr_library_sources(${PJLIB_UTIL_DNS_RESOLVER_SOURCES})
  endif()
  pj_zephyr_add_sources_ifdef(CONFIG_PJSUA PJLIB_UTIL_PJSUA_SOURCES)
  pj_zephyr_add_sources_ifdef(CONFIG_PJMEDIA_SRTP_TRANSPORT
    PJLIB_UTIL_BASE64_SOURCES)
  zephyr_library_include_directories(
    ${PJPROJECT_ROOT_DIR}/pjlib-util/include
    ${PJPROJECT_ROOT_DIR}/pjlib/include
  )
  zephyr_include_directories(${PJPROJECT_ROOT_DIR}/pjlib-util/include)
  target_link_libraries(pjlib-util PUBLIC pjlib)
endif()

if(CONFIG_PJPROJECT AND CONFIG_PJMEDIA)
  message(STATUS "PJPROJECT Zephyr module: PJMEDIA configuration boundary enabled")

  # Public PJMEDIA headers are visible only when the PJMEDIA boundary is on.
  zephyr_include_directories(${PJPROJECT_ROOT_DIR}/pjmedia/include)

  if(CONFIG_PJMEDIA_SDP)
    message(STATUS "PJPROJECT Zephyr module: PJMEDIA SDP enabled")
    zephyr_library_named(pjmedia)
    zephyr_library_sources(${PJMEDIA_SDP_SOURCES})
    zephyr_library_include_directories(
      ${PJPROJECT_ROOT_DIR}/pjmedia/include
      ${PJPROJECT_ROOT_DIR}/pjlib-util/include
      ${PJPROJECT_ROOT_DIR}/pjlib/include
    )
    target_link_libraries(pjmedia PUBLIC pjlib-util pjlib)
    foreach(feature IN ITEMS
      SDP_NEG
      ENDPOINT
      G711
      RTP_RTCP
      LOOP_TRANSPORT
      UDP_TRANSPORT
      STREAM
    )
      pj_zephyr_add_sources_ifdef(CONFIG_PJMEDIA_${feature}
        PJMEDIA_${feature}_SOURCES)
    endforeach()
    if(CONFIG_PJSUA)
      zephyr_library_sources(${PJMEDIA_PJSUA_SOURCES})
      zephyr_library_sources(${PJMEDIA_PJSUA_RESAMPLE_SOURCES})
      zephyr_library_sources(${PJMEDIA_PJSUA_CODEC_SOURCES})
    endif()
    if(CONFIG_PJMEDIA_AUDIODEV)
      zephyr_library_sources(${PJMEDIA_AUDIODEV_SOURCES})
    endif()
    if(CONFIG_PJMEDIA_SRTP)
      zephyr_library_named(srtp)
      zephyr_library_sources(${PJMEDIA_SRTP_SOURCES})
      zephyr_library_include_directories(
        ${PJPROJECT_ROOT_DIR}/third_party/build/srtp
        ${PJPROJECT_ROOT_DIR}/third_party/srtp/include
        ${PJPROJECT_ROOT_DIR}/third_party/srtp/crypto/include
        ${PJPROJECT_ROOT_DIR}/pjlib/include
      )
      zephyr_library_compile_definitions(HAVE_CONFIG_H=1)
      zephyr_include_directories(
        ${PJPROJECT_ROOT_DIR}/third_party/srtp/include
        ${PJPROJECT_ROOT_DIR}/third_party/srtp/crypto/include
      )
      target_link_libraries(srtp PUBLIC pjlib)
      if(CONFIG_PJMEDIA_SRTP_TRANSPORT)
        target_sources(pjmedia PRIVATE ${PJMEDIA_SRTP_TRANSPORT_SOURCES})
        target_include_directories(pjmedia PRIVATE
          ${PJPROJECT_ROOT_DIR}/third_party/build/srtp
        )
        target_link_libraries(pjmedia PUBLIC srtp)
      endif()
    endif()
  endif()
endif()

if(CONFIG_PJPROJECT AND CONFIG_PJSIP)
  message(STATUS "PJPROJECT Zephyr module: PJSIP core enabled")

  zephyr_library_named(pjsip)
  zephyr_library_sources(${PJSIP_CORE_SOURCES})
  foreach(feature IN ITEMS UDP_TRANSPORT TCP_TRANSPORT)
    pj_zephyr_add_sources_ifdef(CONFIG_PJSIP_${feature}
      PJSIP_${feature}_SOURCES)
  endforeach()
  if(NOT CONFIG_PJSIP_UA)
    foreach(feature IN ITEMS INVITE REGC)
      pj_zephyr_add_sources_ifdef(CONFIG_PJSIP_${feature}
        PJSIP_${feature}_SOURCES)
    endforeach()
  endif()
  zephyr_library_include_directories(
    ${PJPROJECT_ROOT_DIR}/pjsip/include
    ${PJPROJECT_ROOT_DIR}/pjlib-util/include
    ${PJPROJECT_ROOT_DIR}/pjlib/include
  )
  zephyr_include_directories(${PJPROJECT_ROOT_DIR}/pjsip/include)
  target_link_libraries(pjsip PUBLIC pjlib-util pjlib)
  if(CONFIG_PJSIP_INVITE)
    zephyr_library_include_directories(
      ${PJPROJECT_ROOT_DIR}/pjmedia/include
    )
    target_link_libraries(pjsip PUBLIC pjmedia)
  endif()
endif()

if(CONFIG_PJPROJECT AND CONFIG_PJNATH)
  message(STATUS "PJPROJECT Zephyr module: PJNATH enabled")
  zephyr_library_named(pjnath)
  zephyr_library_sources(${PJNATH_SOURCES})
  zephyr_library_include_directories(
    ${PJPROJECT_ROOT_DIR}/pjnath/include
    ${PJPROJECT_ROOT_DIR}/pjlib-util/include
    ${PJPROJECT_ROOT_DIR}/pjlib/include
  )
  target_link_libraries(pjnath PUBLIC pjlib-util pjlib)
endif()

if(CONFIG_PJPROJECT AND CONFIG_PJSIP_SIMPLE)
  message(STATUS "PJPROJECT Zephyr module: PJSIP SIMPLE enabled")
  zephyr_library_named(pjsip-simple)
  zephyr_library_sources(${PJSIP_SIMPLE_SOURCES})
  zephyr_library_include_directories(
    ${PJPROJECT_ROOT_DIR}/pjsip/include
    ${PJPROJECT_ROOT_DIR}/pjlib-util/include
    ${PJPROJECT_ROOT_DIR}/pjlib/include
  )
  target_link_libraries(pjsip-simple PUBLIC pjsip pjlib-util pjlib)
endif()

if(CONFIG_PJPROJECT AND CONFIG_PJSIP_UA)
  message(STATUS "PJPROJECT Zephyr module: PJSIP-UA enabled")
  zephyr_library_named(pjsip-ua)
  zephyr_library_sources(${PJSIP_UA_SOURCES})
  zephyr_library_include_directories(
    ${PJPROJECT_ROOT_DIR}/pjsip/include
    ${PJPROJECT_ROOT_DIR}/pjmedia/include
    ${PJPROJECT_ROOT_DIR}/pjlib-util/include
    ${PJPROJECT_ROOT_DIR}/pjlib/include
  )
  target_link_libraries(pjsip-ua PUBLIC pjsip-simple pjsip pjmedia)
endif()

if(CONFIG_PJPROJECT AND CONFIG_PJSUA)
  message(STATUS "PJPROJECT Zephyr module: PJSUA-LIB enabled")
  zephyr_library_named(pjsua)
  zephyr_library_sources(${PJSUA_SOURCES})
  zephyr_library_include_directories(
    ${PJPROJECT_ROOT_DIR}/pjsip/include
    ${PJPROJECT_ROOT_DIR}/pjmedia/include
    ${PJPROJECT_ROOT_DIR}/pjnath/include
    ${PJPROJECT_ROOT_DIR}/pjlib-util/include
    ${PJPROJECT_ROOT_DIR}/pjlib/include
  )
  target_link_libraries(pjsua PUBLIC pjsip-ua pjsip-simple pjsip pjmedia pjnath)
endif()
