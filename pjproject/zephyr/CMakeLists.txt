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

if(CONFIG_PJPROJECT AND CONFIG_PJLIB)
  message(STATUS "PJPROJECT Zephyr module: PJLIB enabled")

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

  zephyr_library_named(pjlib-util)
  zephyr_library_sources(${PJLIB_UTIL_SOURCES})
  zephyr_library_sources_ifdef(CONFIG_PJLIB_UTIL_DNS_RESOLVER
    ${PJLIB_UTIL_DNS_RESOLVER_SOURCES}
  )
  zephyr_library_sources_ifdef(CONFIG_PJMEDIA_SRTP_TRANSPORT
    ${PJLIB_UTIL_BASE64_SOURCES}
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
    zephyr_library_sources_ifdef(CONFIG_PJMEDIA_SDP_NEG
      ${PJMEDIA_SDP_NEG_SOURCES}
    )
    zephyr_library_sources_ifdef(CONFIG_PJMEDIA_ENDPOINT
      ${PJMEDIA_ENDPOINT_SOURCES}
    )
    zephyr_library_sources_ifdef(CONFIG_PJMEDIA_G711
      ${PJMEDIA_G711_SOURCES}
    )
    zephyr_library_sources_ifdef(CONFIG_PJMEDIA_RTP_RTCP
      ${PJMEDIA_RTP_RTCP_SOURCES}
    )
    zephyr_library_sources_ifdef(CONFIG_PJMEDIA_LOOP_TRANSPORT
      ${PJMEDIA_LOOP_TRANSPORT_SOURCES}
    )
    zephyr_library_sources_ifdef(CONFIG_PJMEDIA_UDP_TRANSPORT
      ${PJMEDIA_UDP_TRANSPORT_SOURCES}
    )
    zephyr_library_sources_ifdef(CONFIG_PJMEDIA_STREAM
      ${PJMEDIA_STREAM_SOURCES}
    )
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
  zephyr_library_sources_ifdef(CONFIG_PJSIP_UDP_TRANSPORT
    ${PJSIP_UDP_TRANSPORT_SOURCES}
  )
  zephyr_library_sources_ifdef(CONFIG_PJSIP_TCP_TRANSPORT
    ${PJSIP_TCP_TRANSPORT_SOURCES}
  )
  zephyr_library_sources_ifdef(CONFIG_PJSIP_INVITE
    ${PJSIP_INVITE_SOURCES}
  )
  zephyr_library_sources_ifdef(CONFIG_PJSIP_REGC
    ${PJSIP_REGC_SOURCES}
  )
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
