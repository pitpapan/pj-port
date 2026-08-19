# Zephyr-only PJLIB integration. The PJPROJECT root CMakeLists.txt remains the
# upstream entry point for Linux, Windows, macOS, and other native builds.

if(CONFIG_PJPROJECT AND CONFIG_PJLIB)
  message(STATUS "PJPROJECT Zephyr module: PJLIB enabled")

  get_filename_component(PJPROJECT_ROOT_DIR
    "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE
  )

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
