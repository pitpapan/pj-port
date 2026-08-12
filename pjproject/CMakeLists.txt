if(DEFINED ZEPHYR_BASE)
  if(CONFIG_PJPROJECT AND CONFIG_PJLIB)
    message(STATUS "PJPROJECT Zephyr module: PJLIB enabled")

    # Portable PJLIB core. Keep this list explicit so platform backends cannot
    # be pulled in accidentally as new files are added upstream.
    set(PJLIB_COMMON_SOURCES
      pjlib/src/pj/activesock.c
      pjlib/src/pj/array.c
      pjlib/src/pj/atomic_slist.c
      pjlib/src/pj/atomic_queue.cpp
      pjlib/src/pj/config.c
      pjlib/src/pj/ctype.c
      pjlib/src/pj/errno.c
      pjlib/src/pj/except.c
      pjlib/src/pj/fifobuf.c
      pjlib/src/pj/guid.c
      pjlib/src/pj/hash.c
      pjlib/src/pj/ip_helper_generic.c
      pjlib/src/pj/list.c
      pjlib/src/pj/lock.c
      pjlib/src/pj/log.c
      pjlib/src/pj/os_info.c
      pjlib/src/pj/os_time_common.c
      pjlib/src/pj/os_timestamp_common.c
      pjlib/src/pj/pool.c
      pjlib/src/pj/pool_buf.c
      pjlib/src/pj/pool_caching.c
      pjlib/src/pj/pool_dbg.c
      pjlib/src/pj/rand.c
      pjlib/src/pj/rbtree.c
      pjlib/src/pj/sock_common.c
      pjlib/src/pj/sock_qos_common.c
      pjlib/src/pj/ssl_sock_common.c
      pjlib/src/pj/string.c
      pjlib/src/pj/timer.c
      pjlib/src/pj/types.c
      pjlib/src/pj/unittest.c
    )

    # One implementation is selected for each platform-dependent family.
    # Stage 4 deliberately starts with PJLIB's existing POSIX/BSD backends;
    # Stage 5 will supply and validate the Zephyr platform configuration.
    set(PJLIB_PLATFORM_SOURCES
      pjlib/src/pj/addr_resolv_sock.c
      pjlib/src/pj/file_access_unistd.c
      pjlib/src/pj/file_io_ansi.c
      pjlib/src/pj/guid_simple.c
      pjlib/src/pj/ioqueue_select.c
      pjlib/src/pj/log_writer_stdout.c
      pjlib/src/pj/os_core_unix.c
      pjlib/src/pj/os_error_unix.c
      pjlib/src/pj/os_time_unix.c
      pjlib/src/pj/os_timestamp_posix.c
      pjlib/src/pj/pool_policy_malloc.c
      pjlib/src/pj/sock_bsd.c
      pjlib/src/pj/sock_qos_bsd.c
      pjlib/src/pj/sock_select.c
    )

    zephyr_library_named(pjlib)
    zephyr_library_sources(
      ${PJLIB_COMMON_SOURCES}
      ${PJLIB_PLATFORM_SOURCES}
    )
    zephyr_library_include_directories(pjlib/include)
    zephyr_include_directories(pjlib/include)
  endif()

  return()
endif()

cmake_minimum_required(VERSION 3.28...4.0)

# Read version from version.mak
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/version.mak" _version_lines)
foreach(_line IN LISTS _version_lines)
  if(_line MATCHES "^export PJ_VERSION_MAJOR[ \t]*:=[ \t]*([0-9]+)")
    set(_pj_ver_major "${CMAKE_MATCH_1}")
  elseif(_line MATCHES "^export PJ_VERSION_MINOR[ \t]*:=[ \t]*([0-9]+)")
    set(_pj_ver_minor "${CMAKE_MATCH_1}")
  elseif(_line MATCHES "^export PJ_VERSION_REV[ \t]*:=[ \t]*([0-9]+)")
    set(_pj_ver_rev "${CMAKE_MATCH_1}")
  endif()
endforeach()
# CMake VERSION field only accepts numeric components
if(_pj_ver_rev)
  set(_pj_cmake_version "${_pj_ver_major}.${_pj_ver_minor}.${_pj_ver_rev}")
else()
  set(_pj_cmake_version "${_pj_ver_major}.${_pj_ver_minor}")
endif()

project(pjproject
  VERSION ${_pj_cmake_version}
  DESCRIPTION "Multimedia communication library"
  HOMEPAGE_URL "http://www.pjsip.org"
  LANGUAGES C CXX
)

# project modules
list(INSERT CMAKE_MODULE_PATH 0 "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# experimental warning notice
option(PJ_SKIP_EXPERIMENTAL_NOTICE OFF)
if (NOT PJ_SKIP_EXPERIMENTAL_NOTICE)
  message(WARNING
    " ****************************************************\n"
    " * Building `pjproject` with CMake is experimental  *\n"
    " *--------------------------------------------------*\n"
    " * Tested platforms:                                *\n"
    " * - Linux x86_64                                   *\n"
    " * - macOS                                          *\n"
    " ****************************************************\n"
    "Set `PJ_SKIP_EXPERIMENTAL_NOTICE=ON` to disable this warning"
  )
endif()

# ##############################################################################
# Testing support

if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
  include(CTest)
endif()

if(BUILD_TESTING)
  if(CMAKE_SYSTEM_NAME MATCHES "rtems")
    set(PJ_TEST_MAIN "main_rtems.c")
  else()
    set(PJ_TEST_MAIN "main.c")
  endif()
endif()

# ##############################################################################
# Installation preparation

include(GNUInstallDirs)

# add `$ORIGIN` to targets' RPATH
if(BUILD_SHARED_LIBS)
  if(APPLE)
    list(APPEND CMAKE_INSTALL_RPATH
      "@loader_path"
      "@executable_path/../${CMAKE_INSTALL_LIBDIR}"
    )
  elseif(UNIX)
    list(APPEND CMAKE_INSTALL_RPATH
      "$ORIGIN"
      "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}"
    )
  endif()
endif()

# ##############################################################################
# Sub-directories

# base library
add_subdirectory(pjlib)

# third-party libraries
add_subdirectory(third_party)

# remaning libraries
add_subdirectory(pjlib-util)
add_subdirectory(pjnath)
add_subdirectory(pjmedia)
add_subdirectory(pjsip)
add_subdirectory(pjsip-apps)

# ##############################################################################
# Installation

set(install_targets
  pjlib
  pjlib-ssl
  pjlib++
  pjlib-util
  pjnath
  pjmedia
  pjmedia-codec
  pjmedia-audiodev
  pjmedia-videodev
  pjsip
  pjsip-simple
  pjsip-ua
  pjsua-lib
  pjsua2
  pjsua
)

# set `VERSION` and `SO_VERSION`
foreach(target IN LISTS install_targets)
  set_target_properties(${target} PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
  )
endforeach()

# install targets
install(
  TARGETS ${install_targets}
  EXPORT PjTargets

  FILE_SET HEADERS
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    COMPONENT PjDevelopment
  FILE_SET config_headers
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    COMPONENT PjDevelopment
  RUNTIME
    DESTINATION "${CMAKE_INSTALL_BINDIR}"
    COMPONENT PjRuntime
  LIBRARY
    DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    COMPONENT PjRuntime
    NAMELINK_COMPONENT PjDevelopment
  ARCHIVE
    DESTINATION  "${CMAKE_INSTALL_BINDIR}"
    COMPONENT PjDevelopment
)

# ##############################################################################
# CMAKe Package config

include(CMakePackageConfigHelpers)

set(PJ_CMAKE_INSTALL_DIR         "${CMAKE_INSTALL_LIBDIR}/cmake/Pj")
set(PJ_CMAKE_TARGETS_FILE        "PjTargets.cmake")
set(PJ_CMAKE_CONFIG_FILE         "PjConfig.cmake")
set(PJ_CMAKE_CONFIG_VERSION_FILE "PjConfigVersion.cmake")

# install exports
install(
  EXPORT      PjTargets
  NAMESPACE   Pj::
  FILE        "${PJ_CMAKE_TARGETS_FILE}"
  DESTINATION "${PJ_CMAKE_INSTALL_DIR}"
  COMPONENT   PjDevelopment
)

# write package config
configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Pj/${PJ_CMAKE_CONFIG_FILE}.in"
  "${CMAKE_CURRENT_BINARY_DIR}/${PJ_CMAKE_CONFIG_FILE}"

  INSTALL_DESTINATION "${PJ_CMAKE_INSTALL_DIR}"
)

# write package config version
write_basic_package_version_file(
  "${PJ_CMAKE_CONFIG_VERSION_FILE}"

  COMPATIBILITY SameMajorVersion
)

# collect third-party dependency provider info for PjDeps.cmake
set(PJ_BUNDLED_DEPS "")
set(PJ_SYSTEM_DEPS "")

set(_pj_all_deps G7221 GSM iLBC Resample Speex SRTP YUV)
if(NOT (APPLE OR MINGW OR CYGWIN))
  list(APPEND _pj_all_deps WebRTC WebRTC_AEC3)
endif()

foreach(_dep IN LISTS _pj_all_deps)
  string(TOUPPER "PJ_DEP_${_dep}" _dep_option)
  if(DEFINED ${_dep_option})
    if(${_dep_option} STREQUAL "bundled")
      list(APPEND PJ_BUNDLED_DEPS "${_dep}")
    elseif(${_dep_option} STREQUAL "system")
      list(APPEND PJ_SYSTEM_DEPS "${_dep}")
    endif()
  endif()
endforeach()
unset(_pj_all_deps)

list(JOIN PJ_BUNDLED_DEPS ";" PJ_BUNDLED_DEPS_STR)
list(JOIN PJ_SYSTEM_DEPS ";" PJ_SYSTEM_DEPS_STR)

configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Pj/PjDeps.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/PjDeps.cmake"
  @ONLY
)

# Install custom Find modules needed by enabled dependencies.
# CMake built-in modules (Threads, OpenSSL, GnuTLS, ALSA, OpenGL, SDL2)
# do not need to be installed.
set(_pj_find_modules)

# Helper: append a Find module if the feature is enabled and the file exists
macro(_pj_collect_find_module _condition _package)
  if(${_condition})
    set(_fm "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Find${_package}.cmake")
    if(EXISTS "${_fm}")
      list(APPEND _pj_find_modules "${_fm}")
    endif()
    unset(_fm)
  endif()
endmacro()

# System third-party deps (bundled deps use Pj::Dep:: aliases instead)
set(_pj_pkg_G7221    "G7221")
set(_pj_pkg_GSM      "GSM")
set(_pj_pkg_Resample "Resample")
set(_pj_pkg_Speex    "Speex")
set(_pj_pkg_SRTP     "SRTP")
set(_pj_pkg_YUV      "YUV")
foreach(_dep IN LISTS PJ_SYSTEM_DEPS)
  if(DEFINED _pj_pkg_${_dep})
    _pj_collect_find_module(TRUE ${_pj_pkg_${_dep}})
  endif()
endforeach()

# Platform libs and optional features with custom Find modules
_pj_collect_find_module(PJLIB_WITH_LIBUUID               UUID)
_pj_collect_find_module(PJNATH_WITH_UPNP                 UPNP)
_pj_collect_find_module(PJMEDIA_WITH_AUDIODEV_COREAUDIO   CoreAudio)
_pj_collect_find_module(PJMEDIA_WITH_VIDEODEV_V4L2        V4L2)
if(PJMEDIA_WITH_RESAMPLE STREQUAL "libsamplerate")
  _pj_collect_find_module(TRUE SampleRate)
endif()
_pj_collect_find_module(PJMEDIA_WITH_SPEEX_AEC            SpeexDSP)
_pj_collect_find_module(PJMEDIA_WITH_FFMPEG               FFMPEG)
_pj_collect_find_module(PJMEDIA_WITH_OPENCORE_AMRNB_CODEC OpenCoreAMRNB)
_pj_collect_find_module(PJMEDIA_WITH_OPENCORE_AMRWB_CODEC OpenCoreAMRWB)
_pj_collect_find_module(PJMEDIA_WITH_OPENCORE_AMRWB_CODEC VisualOnAMRWBEnc)
_pj_collect_find_module(PJMEDIA_WITH_SILK_CODEC           Silk)
_pj_collect_find_module(PJMEDIA_WITH_OPUS_CODEC           OPUS)
_pj_collect_find_module(PJMEDIA_WITH_BCG729_CODEC         BCG729)
_pj_collect_find_module(PJMEDIA_WITH_LYRA_CODEC           Lyra)
_pj_collect_find_module(PJMEDIA_WITH_VPX_CODEC            VPX)
_pj_collect_find_module(PJMEDIA_WITH_OPEN_H264_CODEC      OpenH264)

if(_pj_find_modules)
  list(REMOVE_DUPLICATES _pj_find_modules)
  install(FILES ${_pj_find_modules}
    DESTINATION "${PJ_CMAKE_INSTALL_DIR}"
    COMPONENT PjDevelopment
  )
endif()

install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/${PJ_CMAKE_CONFIG_FILE}"
  "${CMAKE_CURRENT_BINARY_DIR}/${PJ_CMAKE_CONFIG_VERSION_FILE}"
  "${CMAKE_CURRENT_BINARY_DIR}/PjDeps.cmake"

  DESTINATION "${PJ_CMAKE_INSTALL_DIR}"
  COMPONENT PjDevelopment
)

# ##############################################################################
# pkg-config file

set(PREFIX "${CMAKE_INSTALL_PREFIX}") # this should be done at install time
set(LIBDIR "${PREFIX}/${CMAKE_INSTALL_LIBDIR}")
set(INCLUDEDIR "${PREFIX}/${CMAKE_INSTALL_INCLUDEDIR}")
set(PJ_VERSION "${PROJECT_VERSION}")
set(PJ_INSTALL_LDFLAGS "")
set(PJ_INSTALL_LDFLAGS "")

configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/libpjproject.pc.in"
  "${CMAKE_CURRENT_BINARY_DIR}/libpjproject.pc"
)

install(
  FILES "${CMAKE_CURRENT_BINARY_DIR}/libpjproject.pc"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig"
)
