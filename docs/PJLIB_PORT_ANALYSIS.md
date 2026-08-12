# PJLIB Zephyr Port Analysis

## Scope

This document analyzes `pjproject/pjlib` only. It uses PJPROJECT sources and
build metadata; no Zephyr source was inspected. The purpose is to define a
Zephyr-specific PJLIB build and port boundary without reusing upstream
autoconf platform detection.

```text
PJLIB
├── portable core
│   ├── containers, strings, errors, logging, random, pools, timer heap
│   └── common socket, QoS, active-socket, and SSL plumbing
├── OS abstraction
│   ├── lifecycle, threads, TLS, locks, atomics, semaphores, events
│   ├── wall/monotonic time, timestamp, native errors, files
│   └── sockets, DNS/address resolution, and one ioqueue backend
└── platform configuration
    ├── compiler and architecture properties
    ├── OS/header/socket capability properties
    └── implementation and feature selection
```

Key public boundaries are declared in `include/pj/os.h`, `sock.h`,
`addr_resolv.h`, `ioqueue.h`, `pool.h`, `file_io.h`, and `file_access.h`.
`include/pj/config.h` chooses compiler/OS/architecture configuration and then
unconditionally includes the application-owned `pj/config_site.h`.

## Source classification

### Portable core

The ordinary PJLIB Makefile selects this portable base:

```text
activesock.c          array.c              atomic_slist.c
atomic_queue.cpp      config.c             ctype.c
errno.c               except.c             fifobuf.c
guid.c                hash.c               ip_helper_generic.c
list.c                lock.c               log.c
os_info.c             os_time_common.c     pool.c
pool_buf.c            pool_caching.c       pool_dbg.c
rand.c                rbtree.c             sock_common.c
sock_qos_common.c     ssl_sock_common.c    string.c
timer.c               types.c              unittest.c
```

`atomic_queue.cpp` requires C++ support. `activesock.c`, `sock_common.c`, and
`timer.c` are portable logic but depend on the selected OS/socket/ioqueue
implementation. `ssl_sock_common.c` is common plumbing, not an instruction to
enable TLS; the initial port should keep SSL disabled.

### OS-specific and mutually exclusive implementations

The RTEMS Make fragment is the closest embedded reference, but it is not a
Zephyr source list. It chooses:

```text
addr_resolv_sock.c    guid_simple.c        log_writer_stdout.c
os_core_unix.c        os_error_unix.c      os_time_unix.c
os_timestamp_common.c os_timestamp_posix.c pool_policy_malloc.c
sock_bsd.c            sock_select.c        ioqueue_select.c
file_access_unistd.c  file_io_ansi.c       sock_qos_bsd.c
```

These sources rely on POSIX APIs including pthreads, POSIX semaphores and
condition variables, `gettimeofday()`, `clock_gettime()`, `select()`,
`getaddrinfo()`, BSD sockets, and filesystem interfaces. They must be adapted
or individually verified; they cannot be selected solely because RTEMS uses
them.

| Area | Select exactly one implementation, or deliberately disable it |
| --- | --- |
| OS core | `os_core_unix.c`, `os_core_win32.c`, or a Zephyr implementation |
| Native errors | Unix, Win32, or Zephyr adapter |
| Wall time | Unix, Win32, or Zephyr adapter |
| Timestamp | POSIX, Win32, or Zephyr adapter |
| Files | Unix/ANSI, Win32, or Zephyr policy/adapter |
| GUID | simple, UUID, BSD, Darwin, Android, or Win32 provider |
| Sockets | BSD, Windows, Symbian/UWP, or Zephyr adapter |
| Address resolution | socket resolver, Symbian resolver, or Zephyr adapter |
| I/O queue | dummy, select, epoll, kqueue, IOCP, UWP, Symbian, or Zephyr backend |
| Socket QoS | dummy, BSD/Darwin, Symbian, Windows, or Zephyr adapter |
| Log writer | stdout, printk, Symbian console, or Zephyr writer |
| SSL | none or exactly one backend |

Do not compile multiple members of an implementation family. In particular,
only one ioqueue backend, GUID provider, file backend, socket backend, QoS
backend, and SSL backend may be selected.

### Architecture-specific code

There is no Cortex-M PJLIB source implementation in this checkout. The
architecture boundary is configuration:

- `config.h` detects ARM, but ARM targets must provide endianness.
- PJPROJECT includes `compat/m_armv4.h`, but has no Cortex-M7-specific header.
- The GCC compatibility layer is `compat/cc_gcc.h`.
- `os_timestamp_posix.c` is a POSIX clock implementation, not a Cortex-M
  implementation.

The Zephyr port must explicitly establish ARM/Cortex-M7 identity, little
endian byte order, alignment, and timestamp semantics.

## Platform abstraction boundaries

| Boundary | Semantics PJLIB expects | Existing implementation | Zephyr direction |
| --- | --- | --- | --- |
| Init/shutdown | Reference-counted init; initializes logging, thread state, recursive critical section, exceptions, GUID, timestamp, and hash key. | `os_core_unix.c` | New Zephyr core adaptation preserving ordering and reentrancy. |
| Threads/registration | Created and externally created threads need persistent `pj_thread_t` records; external threads register with PJLIB. | pthreads in `os_core_unix.c` | Map to Zephyr threads and TLS; define stack, priority, join, destroy, and suspended-thread behavior. |
| TLS | Allocate/free slots and set/get per-thread pointers. | pthread keys | Zephyr TLS wrapper or verified POSIX adapter. |
| Mutexes/critical section | Simple and recursive mutexes, trylock, debug ownership, global recursive critical section. | pthread mutexes | Map while preserving recursive behavior. |
| RW mutex | Read/write lock, or semaphore-based emulation. | pthread rwlock / `os_rwmutex.c` | Use compatible primitive or PJLIB emulation. |
| Semaphores/events/barriers | Counting semaphore; auto/manual-reset event operations; optional barrier. | POSIX sem/condvar/barrier | New mappings or emulation; test event semantics. |
| Atomics | Atomic set/get/inc/dec/add, with C atomics or mutex fallback. | C atomics/mutex in Unix core | Reuse C atomics if available; otherwise use the mutex adaptation. |
| Time/sleep | Wall clock, calendar conversion, tick count, millisecond sleep. | Unix time and common helpers | Define epoch/wall-clock policy and implement conversions. |
| Timestamp | Monotonic 64-bit counter and frequency; checked at init when enabled. | POSIX timestamp file | Zephyr timestamp adapter or verified compatible API. |
| Pools/memory | Pool factory uses block alloc/free callbacks; caching pools use locks. | `pool*.c`, malloc policy | Core reusable; initial policy can use configured C allocation. |
| Sockets | BSD-shaped create/bind/connect/accept/send/recv/options semantics and PJ error mapping. | `sock_bsd.c` | Reuse only if required socket behavior/error values are verified. |
| I/O queue | One asynchronous socket-dispatch backend at build time. | select/epoll/kqueue/IOCP/etc. | Dummy backend only for a no-active-I/O profile; otherwise choose/adapt a compatible backend. |
| DNS | `pj_getaddrinfo()` and host helpers depend on name-service APIs. | `addr_resolv_sock.c` | Include only when selected networking configuration provides required resolver semantics. |
| Files | Public open/read/write/seek/stat/delete/move APIs. | ANSI+unistd or Win32 | Decide scope; use correct unsupported adapter if no filesystem exists. |
| libc compatibility | Header/function availability drives PJ wrappers, exceptions, logging, strings, and ctype. | `compat/*.h` | Define availability macros truthfully for the selected C library. |

## Build configuration

### Upstream mechanisms

The Make build composes `PJLIB_OBJS` from `pjlib/build/Makefile` plus an OS
fragment such as `os-rtems.mak`. Autoconf sets `PJ_AUTOCONF=1`, generates
`compat/os_auto.h` and `compat/m_auto.h`, probes host facilities, and appends
Unix/Windows objects. It must not be used to detect a Zephyr target.

PJPROJECT also has an experimental CMake build. Its PJLIB target contains
several I/O queue, QoS, and SSL implementation sources and then probes host
headers, thread libraries, and system features. It is unsuitable for a Zephyr
module without a new explicit Zephyr source list and configuration path.

### Required configuration decisions

The Zephyr build must provide `pj/config_site.h` on its include path. It should
set or establish the following groups:

| Group | Required settings |
| --- | --- |
| Compiler | GCC compatibility macros, C/C++ selection, integer support, `PJ_ALIGN_DATA`. |
| OS identity | A Zephyr selection path, `PJ_OS_NAME`, and only true `PJ_HAS_*_H` header capabilities. |
| Architecture | ARM/Cortex-M7 identity, `PJ_HAS_PENTIUM=0`, `PJ_IS_LITTLE_ENDIAN=1`, `PJ_IS_BIG_ENDIAN=0`, pool alignment. |
| Threads | `PJ_HAS_THREADS`, stack allocation/size policy, stack checking, atomic value type, RW mutex emulation. |
| OS services | High-resolution timer, malloc, Unicode/native strings, native errno, semaphores, and events. |
| Networking | Socket header/options capabilities, `socklen_t`, nonblocking errors, TCP/IPv6, ioqueue backend and limits. |
| Files/DNS | Actual file, select, and resolver capability values. |
| Optional features | Initially `PJ_HAS_SSL_SOCK=0` with the none backend; choose file and QoS behavior explicitly. |

`config.h` sanity checks require high-resolution timer configuration,
endianness, `PJ_EMULATE_RWMUTEX`, `PJ_THREAD_SET_STACK_SIZE`, and
`PJ_THREAD_ALLOCATE_STACK`.

## Reuse assessment

Reusable core candidates: containers, strings, hash, random, exceptions,
logging core, timer heap, pools, `os_time_common.c`,
`os_timestamp_common.c`, common socket helpers, and `guid.c` with one selected
provider.

Conditional candidates: `pool_policy_malloc.c`, `file_io_ansi.c`,
`addr_resolv_sock.c`, `sock_bsd.c`, `sock_select.c`, `ioqueue_select.c`,
`os_error_unix.c`, and `log_writer_stdout.c`. Each relies on POSIX/BSD/stdio
contracts and requires verification.

New Zephyr adaptation is expected for OS core behavior (lifecycle, threads,
TLS, mutexes, critical section, semaphores, events, barriers), sleep,
wall/monotonic time and timestamp, and any socket/DNS/file/ioqueue API not
provided with the required semantics.

## Stage 4 source-selection rule

Build an explicit list consisting of the portable core plus exactly one GUID
provider, pool policy, log writer, OS core, error, time, timestamp, file,
socket/QoS, resolver, and ioqueue choice. Do not use source globs. Enable no
SSL implementation until TLS is explicitly in scope.

## Unresolved questions

1. Is the initial PJLIB profile no-network, basic sockets, or sockets with
   active I/O and DNS?
2. Which Zephyr C library/configuration is the supported baseline for QEMU and
   `mimxrt1064_evk`, and which POSIX compatibility contracts does it provide?
3. What is the required wall-clock behavior when no RTC/time source exists?
4. How must PJLIB thread stack size, priority, join/destroy, and suspended
   creation map to Zephyr threads?
5. What is the required PJLIB mapping for Zephyr and network error values?
6. Is filesystem support required initially, and if not which file APIs need a
   correct unsupported adapter?
7. Can a supported readiness API satisfy PJLIB's ioqueue contract, or is a
   Zephyr-native backend required?
8. Is C++ support for `atomic_queue.cpp` required in the initial port?

