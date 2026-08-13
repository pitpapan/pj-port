/*
 * PJLIB platform configuration for Zephyr RTOS with Picolibc and Zephyr's
 * POSIX compatibility layer.
 */
#ifndef __PJ_COMPAT_OS_ZEPHYR_H__
#define __PJ_COMPAT_OS_ZEPHYR_H__

#define PJ_OS_NAME                          "zephyr"

/* C library and POSIX headers verified by the Stage 5 target compile probe. */
#define PJ_HAS_ARPA_INET_H                  1
#define PJ_HAS_ASSERT_H                     1
#define PJ_HAS_CTYPE_H                      1
#define PJ_HAS_ERRNO_H                      1
#define PJ_HAS_FCNTL_H                      1
#define PJ_HAS_INTTYPES_H                   1
#define PJ_HAS_LIMITS_H                     1
#define PJ_HAS_MALLOC_H                     0
#define PJ_HAS_NETDB_H                      1
#define PJ_HAS_NETINET_IN_H                 1
#define PJ_HAS_NETINET_IN_SYSTM_H           0
#define PJ_HAS_NETINET_IP_H                 0
#define PJ_HAS_NETINET_TCP_H                1
#define PJ_HAS_NET_IF_H                     0
#define PJ_HAS_IFADDRS_H                    0
#define PJ_HAS_SEMAPHORE_H                  1
#define PJ_HAS_SETJMP_H                     1
#define PJ_HAS_STDARG_H                     1
#define PJ_HAS_STDDEF_H                     1
#define PJ_HAS_STDINT_H                     1
#define PJ_HAS_STDIO_H                      1
#define PJ_HAS_STDLIB_H                     1
#define PJ_HAS_STRING_H                     1
#define PJ_HAS_STRINGS_H                    1
#define PJ_HAS_SYS_IOCTL_H                  0
#define PJ_HAS_SYS_SELECT_H                 1
#define PJ_HAS_SYS_SOCKET_H                 1
#define PJ_HAS_SYS_STAT_H                   1
#define PJ_HAS_SYS_TIME_H                   1
#define PJ_HAS_SYS_TIMEB_H                  0
#define PJ_HAS_SYS_TYPES_H                  1
#define PJ_HAS_TIME_H                       1
#define PJ_HAS_UNISTD_H                     1

#define PJ_HAS_LINUX_SOCKET_H               0
#define PJ_HAS_MSWSOCK_H                    0
#define PJ_HAS_WINSOCK_H                    0
#define PJ_HAS_WINSOCK2_H                   0
#define PJ_HAS_WS2TCPIP_H                   0

/* Cortex-M targets used by this port are ARMv7-M little-endian systems. */
#define PJ_M_ARMV7                          1
#define PJ_IS_LITTLE_ENDIAN                 1
#define PJ_IS_BIG_ENDIAN                    0

/* libc, time, and native error behavior.
 *
 * Wall-clock policy: pj_gettimeofday() delegates to Zephyr's POSIX
 * gettimeofday(). PJLIB does not replace it with a boot-uptime clock. On a
 * target without an initialized real-time clock, applications may use it for
 * interval ordering but must not assume the absolute value is calendar time.
 * Monotonic intervals use clock_gettime(CLOCK_MONOTONIC) independently.
 */
#define PJ_HAS_ERRNO_VAR                    1
#define PJ_NATIVE_ERR_POSITIVE              1
#define PJ_HAS_HIGH_RES_TIMER               1
#define PJ_HAS_LOCALTIME_R                  1
#define PJ_HAS_MALLOC                       1
#define PJ_NATIVE_STRING_IS_UNICODE         0
#define PJ_POOL_ALIGNMENT                   8
#define PJ_OS_HAS_CHECK_STACK               0

/* pthread and synchronization capabilities verified by the compile probe. */
#define PJ_HAS_THREADS                      1
#define PJ_HAS_PTHREAD_MUTEXATTR_SETTYPE    1
#define PJ_HAS_PTHREAD_NP_H                 0
#define PJ_HAS_PTHREAD_SETNAME_NP           0
#define PJ_HAS_PTHREAD_SET_NAME_NP          0
#define PJ_HAS_SEMAPHORE                    1
#define PJ_HAS_EVENT_OBJ                    1
#define PJ_EMULATE_RWMUTEX                  0
#define PJ_ATOMIC_VALUE_TYPE                long

/* Let the POSIX layer own and align thread stacks; honor an explicitly
 * requested size. PJLIB joins created threads but must not free a raw stack
 * pointer because none is allocated or returned through the PJLIB pool.
 */
#define PJ_THREAD_SET_STACK_SIZE            1
#define PJ_THREAD_ALLOCATE_STACK            0

/* Priority policy: PJLIB uses pthread_get/setschedparam() values as opaque
 * POSIX priorities and never translates them to Zephyr kernel priorities.
 * This Zephyr configuration does not export sched_get_priority_min/max(), so
 * PJLIB's range-query helpers report their Unix-backend unsupported fallback.
 */

/* BSD socket and resolver capabilities verified by the compile probe. */
#define PJ_HAS_SOCKLEN_T                    1
#define PJ_SOCKADDR_HAS_LEN                 0
#define PJ_SOCK_HAS_INET_ATON               0
#define PJ_SOCK_HAS_INET_PTON               1
#define PJ_SOCK_HAS_INET_NTOP               1
#define PJ_SOCK_HAS_GETADDRINFO             1
#define PJ_HAS_GETHOSTBYNAME                0
#define PJ_SOCK_HAS_SOCKETPAIR              0
#define PJ_SOCK_HAS_IPV6_V6ONLY             0
#define PJ_HAS_SO_ERROR                     1
#define PJ_SELECT_NEEDS_NFDS                1
#define PJ_IOQUEUE_MAX_HANDLES              32
#define PJ_BLOCKING_ERROR_VAL               EWOULDBLOCK
#define PJ_BLOCKING_CONNECT_ERROR_VAL       EINPROGRESS

#endif /* __PJ_COMPAT_OS_ZEPHYR_H__ */
