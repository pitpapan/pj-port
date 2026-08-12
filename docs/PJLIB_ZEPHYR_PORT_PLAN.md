PJLIB → Zephyr Port Plan

1. Objective

Port PJLIB from PJPROJECT to Zephyr RTOS.

Initial scope:

PJLIB only

Zephyr RTOS

Final product target: NXP i.MX RT1064 / Cortex-M7

Primary port-development and validation target: Zephyr mps2/an385 under QEMU

During the PJLIB port phase, QEMU is the normal build and runtime validationenvironment.

Do not continuously rebuild or validate mimxrt1064_evk after each stage,source change, or compatibility experiment.

RT1064-specific compilation and hardware validation are deferred to a laterintegration milestone unless explicitly requested.

The following components are currently out of scope:

PJLIB-UTIL

PJNATH

PJMEDIA

PJSIP

SIP functionality

RTP functionality

audio devices

codecs

They must not be added until the PJLIB port is validated.

2. Repository Boundaries

The primary source repository to inspect is:

pjproject/

The port may also create and modify:

applications/pjlib_minimal/

and project documentation/configuration files explicitly related to the port.

Zephyr Source Policy

Do not inspect, recursively search, index, or modify:

zephyr/

Treat the Zephyr source tree as an external platform dependency.

Use:

documented Zephyr APIs;

Kconfig/CMake interfaces;

west commands;

compiler diagnostics;

linker diagnostics;

runtime output;

to perform the port.

If a problem appears to require reading Zephyr implementation source,stop and report the reason before doing so.

3. Porting Philosophy

The goal is not to preserve PJPROJECT's multi-platform build system.

This fork only needs to support Zephyr.

It is therefore acceptable to:

create a Zephyr-specific CMake build;

create Kconfig integration;

create zephyr/module.yml;

simplify platform selection;

bypass upstream configure/autoconf for the Zephyr build;

add Zephyr-specific platform implementation files.

However, do not modify PJLIB behavior merely to make compilation succeed.

Before replacing a platform abstraction:

locate its declaration;

inspect the existing PJPROJECT implementation;

determine the semantics PJLIB expects;

determine whether Zephyr's POSIX compatibility layer already provides those semantics;

if it does, prefer reusing the existing PJLIB POSIX/BSD implementation;

only if the POSIX path is unavailable or semantically incorrect, identify the appropriate Zephyr-native API;

implement the smallest correct adaptation;

verify it with compilation or runtime tests.

POSIX Compatibility Policy

Prefer Zephyr's POSIX compatibility layer whenever it provides the semanticsrequired by PJLIB.

Do not rewrite a PJLIB implementation merely because its filename containsunix, posix, or bsd.

Existing PJLIB POSIX/BSD implementations should be evaluated first.

Important candidates include:

os_core_unix.c
os_error_unix.c
os_time_unix.c
os_timestamp_posix.c
pool_policy_malloc.c
sock_bsd.c
sock_select.c
addr_resolv_sock.c
ioqueue_select.c
file_io_ansi.c
file_access_unistd.c
log_writer_stdout.c

A Zephyr-specific replacement should be introduced only when at least one ofthe following is demonstrated:

Zephyr does not provide the required API;

the API is unavailable in the selected Zephyr configuration;

the API compiles but does not implement the semantics PJLIB requires;

the resource, lifetime, scheduling, or stack model cannot safely representPJLIB behavior;

the existing PJLIB implementation would depend on host behavior rather thanZephyr target behavior.

Do not assume that use of POSIX APIs implies a Linux dependency. Zephyr's ownPOSIX compatibility implementation is an acceptable target dependency.

4. Stage 0 — Establish Build Baseline

Goal

Verify that the existing Zephyr development environment works beforeintroducing PJPROJECT.

Do not inspect Zephyr source files.

Actions

Run environment/build commands such as:

west --version
west topdir
west list

Verify that the intended QEMU board is available using normal west/Zephyrcommand-line interfaces.

Build an existing minimal Zephyr application or create our own minimalapplication without PJLIB.

Preferred initial runtime target:

mps2/an385

Completion Criteria

A pure Zephyr application:

configures successfully;

compiles successfully;

links successfully;

runs under QEMU.

No PJPROJECT code is involved.

No product-board build is required at this stage.

5. Stage 1 — Analyze PJLIB

Goal

Understand exactly what must be ported before changing the implementation.

Inspect only the PJPROJECT repository.

Investigate

Determine:

Source structure

Identify:

platform-independent source files;

OS-specific source files;

architecture-specific source files;

mutually exclusive implementations.

Platform abstraction

Locate implementations and declarations related to:

pj_init() / pj_shutdown();

threads;

thread registration;

TLS;

mutexes;

semaphores;

events;

barriers;

atomic operations;

critical sections;

time;

sleep;

timestamps;

memory;

pools;

sockets;

DNS/address resolution;

ioqueue;

files;

logging;

random/GUID behavior;

libc compatibility.

Build configuration

Investigate:

pj/config.h;

pj/config_site.h;

compatibility headers;

platform detection macros;

upstream Makefiles;

configure/autoconf logic relevant to PJLIB;

actual PJLIB source selection.

For every Unix/POSIX/BSD implementation, record whether it appears suitable forlater Zephyr POSIX compatibility testing.

Do not classify a source as requiring a Zephyr-specific replacement solelybecause it uses POSIX APIs.

Deliverable

Create:

docs/PJLIB_PORT_ANALYSIS.md

It must contain:

PJLIB source classification;

platform abstraction boundaries;

platform-specific source files;

required configuration macros;

implementations that may be reusable;

POSIX/BSD implementations that should be tested first on Zephyr;

functionality that likely requires a Zephyr-specific implementation;

unresolved questions.

Completion Criteria

Before implementation begins, we must be able to explain:

PJLIB
|
+-- portable core
|
+-- OS abstraction
|
+-- platform configuration

and identify which pieces are:

reusable directly
|
+-- reusable through Zephyr POSIX compatibility
|
+-- requiring semantic verification
|
+-- requiring a Zephyr-specific adapter

6. Stage 2 — Minimal Zephyr Test Application

Goal

Create an isolated application for PJLIB port development.

Target layout:

applications/
└── pjlib_minimal/
    ├── CMakeLists.txt
    ├── prj.conf
    └── src/
        └── main.c

Initially it must contain no PJPROJECT dependency.

The program should only print a startup message.

Completion Criteria

The application successfully builds and runs on:

mps2/an385

No mimxrt1064_evk build is required during routine port development.

7. Stage 3 — Zephyr Module Integration

Goal

Make PJPROJECT discoverable as a Zephyr module.

Introduce the minimum required integration:

pjproject/
├── Kconfig
├── CMakeLists.txt
└── zephyr/
    └── module.yml

Initial configuration should contain only what is needed for:

CONFIG_PJPROJECT
CONFIG_PJLIB

Do not expose the entire PJPROJECT feature set through Kconfig yet.

Requirements

The application must not contain hard-coded machine-specific paths.

PJPROJECT must be integrated through the Zephyr module mechanism rather thanbeing manually compiled by the application.

Kconfig should become the source of truth for Zephyr-facing port options.

Do not independently duplicate the same feature decision across:

prj.conf
CMakeLists.txt
config_site.h

where it can be mapped from one source of truth.

Completion Criteria

Enabling PJLIB causes the PJPROJECT module build logic to execute.

Disabling it removes PJLIB from the build.

8. Stage 4 — PJLIB CMake Build

Goal

Make Zephyr CMake compile the correct PJLIB source set.

The source list must be derived from the Stage 1 analysis.

Do not compile every .c file.

Explicitly distinguish between:

common PJLIB sources
+
exactly one selected implementation from each platform family

Avoid source globs where they could accidentally include mutually exclusiveplatform implementations.

Before introducing a new *_zephyr.c file, first try the correspondingexisting POSIX/BSD implementation when the Stage 1 analysis identifies it as acandidate.

Initial candidates to try before replacement include:

pool_policy_malloc.c
sock_bsd.c
addr_resolv_sock.c
ioqueue_select.c
os_timestamp_posix.c
file_io_ansi.c
file_access_unistd.c

and compatible portions of:

os_core_unix.c
os_error_unix.c
os_time_unix.c

Compile failures at this stage are diagnostic information.

For each failure, determine whether the cause is:

missing header/API
configuration macro mismatch
unsupported Zephyr POSIX feature
semantic incompatibility
source-selection error

Completion Criteria

The build reaches actual PJLIB compilation.

Compile errors caused by unresolved platform functionality are acceptable atthis stage.

CMake/module-discovery failures are not.

9. Stage 5 — Zephyr Platform Configuration

Goal

Provide PJLIB with an explicit Zephyr configuration.

Create or adapt the appropriate PJ configuration mechanism, normally throughPJLIB's platform/application configuration facilities.

The Zephyr port should explicitly identify Zephyr rather than pretending to beLinux, RTEMS, or another upstream operating system.

At the same time, individual POSIX/BSD implementation files may still be reusedwhen Zephyr supplies the required APIs and semantics.

Configuration Areas

Investigate and configure only when necessary:

threading;

TLS;

mutexes;

recursive mutexes;

RW locks;

semaphores;

events/barriers;

TCP;

UDP;

IPv4;

IPv6;

DNS;

ioqueue;

floating point;

byte order;

alignment;

libc facilities;

time/timestamp behavior;

filesystem behavior;

errno mapping;

random/GUID behavior;

debug/logging behavior.

Do not replicate all upstream configure options.

Only introduce configuration required by the active port.

POSIX Capability Probes

Before writing new platform adapters, explicitly test the Zephyr configurationfor the APIs needed by the candidate PJLIB implementations.

Relevant probes include:

pthread_create
pthread_join
pthread_self
pthread_key_create
pthread_key_delete
pthread_setspecific
pthread_getspecific
pthread_mutex_*
pthread_cond_*
pthread_rwlock_*
pthread_barrier_*
sem_*
clock_gettime
clock_getres
gettimeofday
nanosleep
socket
bind
connect
listen
accept
send
recv
getsockopt
setsockopt
getaddrinfo
freeaddrinfo
select
poll

A successful compile is not enough for behavior that has important semantics.

Where necessary, add small runtime probes for:

recursive mutex behavior;

multiple TLS keys;

thread registration/lifetime;

thread join/destroy;

stack ownership;

priority handling;

semaphore/event behavior;

monotonic clock behavior;

nonblocking socket errors;

select() readiness behavior.

libc baseline

Use Picolibc as the initial libc baseline unless the existing project has anexplicit reason to require another Zephyr libc.

Do not turn the initial PJLIB port into a minimal-libc compatibility exercise.

10. Stage 6 — Compile and Link PJLIB

Goal

Reach a complete PJLIB compile and link.

For every unresolved platform symbol or implementation failure:

error
↓
declaration
↓
existing PJ implementation
↓
required semantics
↓
Zephyr POSIX compatibility check
↓
reuse existing implementation if correct
↓
otherwise smallest Zephyr-specific mapping

Do not create dummy functions whose only purpose is removing linker errors.

Specific reuse priorities

Threads and synchronization

Prefer Zephyr-provided POSIX APIs first for:

pthread/TLS
mutexes
condition variables
RW locks
barriers
semaphores

Only replace the incompatible portions of the OS abstraction.

Do not rewrite all of os_core_unix.c preemptively.

Sockets

First attempt:

sock_bsd.c

against Zephyr's BSD/POSIX socket interface.

Only introduce sock_zephyr.c after a concrete incompatible contract isidentified.

DNS

First attempt:

addr_resolv_sock.c

against Zephyr address-resolution support.

ioqueue

First attempt:

ioqueue_select.c

using Zephyr's socket readiness/select compatibility.

Do not design a custom Zephyr-native ioqueue backend until ioqueue_select.chas been shown to be insufficient.

Time

Treat separately:

wall clock
monotonic timestamp
relative sleep

Do not use boot uptime as wall-clock time without an explicit policy.

Files

If filesystem support is not needed initially, public PJLIB file APIs muststill link and return a deliberate unsupported/error result where required.

Do not leave unresolved public symbols.

Completion Criteria

The minimum application can:

#include <pjlib.h>

and links with PJLIB.

Runtime initialization is handled in the next stage.

11. Stage 7 — PJLIB Initialization

Goal

Execute the real:

pj_init();

and:

pj_shutdown();

paths.

Investigate failures involving

thread registration;

persistent external-thread descriptors;

TLS;

synchronization;

recursive locking;

atomics;

time;

timestamp initialization;

memory/pool initialization;

logging;

native error mapping.

Completion Criteria

The application reports successful PJLIB initialization and shutdown using thereal implementation.

No fake success path is permitted.

The test runs on:

mps2/an385

under QEMU.

12. Stage 8 — Core PJLIB Runtime Validation

Goal

Validate the PJLIB abstractions that are required before networking is trusted.

Prefer existing PJLIB tests where practical instead of inventing replacementtests.

Validate:

repeated pj_init() / pj_shutdown();

threads;

external thread registration;

TLS;

mutexes;

recursive mutexes;

RW locks;

semaphores;

events/barriers if enabled;

atomics;

exceptions;

pools;

sleep;

wall-clock behavior;

monotonic timestamps;

timer heap;

native error conversion.

Critical semantic checks

External threads

pj_thread_register() storage must remain valid for as long as PJLIB may usethe thread record.

Do not use temporary stack storage for persistent thread registration data.

Priorities

Do not directly assume PJLIB numeric priorities equal Zephyr kernel priorities.

Document the mapping or the behavior supplied by Zephyr's POSIX schedulerinterface.

Thread stack ownership

Verify how PJLIB-created thread stacks are allocated, aligned, owned, andreleased.

Do not assume a generic malloc(stack_size) buffer automatically satisfiesZephyr thread-stack requirements.

Timestamp

Verify:

monotonicity;

reported frequency;

conversion accuracy;

64-bit behavior on the 32-bit ARM target;

sleep/timer consistency.

Completion Criteria

The required core PJLIB runtime tests pass repeatedly under QEMU.

13. Stage 9 — Basic Networking

Goal

Validate the existing PJLIB networking abstractions on Zephyr.

Networking should be added only after the core runtime is stable.

Socket strategy

Use this order:

sock_bsd.c
↓
compile
↓
semantic tests
↓
reuse if correct
↓
small Zephyr-specific adapter only if required

Test:

socket creation/close;

IPv4;

UDP bind/send/receive;

TCP bind/listen/accept;

TCP connect;

nonblocking mode;

EINPROGRESS;

EAGAIN / EWOULDBLOCK;

timeout/error mapping;

common socket options required by PJLIB;

peer shutdown/reset behavior.

DNS

If DNS is in the active scope, try:

addr_resolv_sock.c

first.

Test:

numeric addresses;

successful hostname lookup;

failed lookup;

repeated lookup;

error conversion.

IPv6 can remain disabled until the IPv4 path is stable.

Completion Criteria

Basic PJLIB UDP/TCP behavior works under QEMU without a custom socket backendunless one was proven necessary.

14. Stage 10 — ioqueue

Goal

Validate PJLIB asynchronous socket readiness behavior.

First attempt:

PJ_IOQUEUE_IMP_SELECT
+
ioqueue_select.c
+
Zephyr select/socket readiness support

A custom Zephyr-native or poll-based backend is a fallback or lateroptimization.

Required tests

Validate:

readable readiness;

writable readiness;

connect completion;

timeout;

multiple sockets;

registration/unregistration;

unregister while poll/select is blocked;

close while waiting;

safe unregister behavior;

callback concurrency;

repeated register/unregister;

configured maximum handle count.

Pay specific attention to PJLIB configuration and semantics around:

PJ_IOQUEUE_HAS_SAFE_UNREG
PJ_IOQUEUE_DEFAULT_ALLOW_CONCURRENCY

Do not declare ioqueue complete after only a simple select() smoke test.

Completion Criteria

The selected existing ioqueue backend passes the required semantics underQEMU, or a concrete documented incompatibility justifies a Zephyr-specificreplacement.

15. Stage 11 — Deferred Product Integration

This stage is deliberately outside the normal PJLIB port loop.

Do not run it after every source change.

Only enter this stage when explicitly requested or when the QEMU PJLIB port isconsidered stable enough for product integration.

Possible later work includes:

mimxrt1064_evk cross-build;

real i.MX RT1064 board smoke tests;

ENET integration;

hardware-specific resource measurements;

cache/DMA-related validation;

product-specific memory budgets.

These concerns must not cause the generic PJLIB Zephyr port to fork intoboard-specific implementations unless a genuine platform requirement exists.

16. Deferred Features

Do not add these until the current PJLIB baseline is stable:

PJLIB-UTIL;

PJNATH;

PJMEDIA;

PJSIP;

SIP;

RTP;

SSL/TLS socket backend;

codecs;

audio devices;

custom Zephyr-native ioqueue optimization;

minimal-libc support;

product-board validation.

17. Decision Rule for Platform Code

For every existing PJLIB Unix/POSIX/BSD implementation, use this sequence:

Does Zephyr provide the required POSIX/BSD API?
        |
       yes
        ↓
Does the existing PJLIB source compile against it?
        |
       yes
        ↓
Does it satisfy PJLIB semantics under QEMU?
        |
       yes
        ↓
Reuse it.

Only when one answer is no:

identify the exact missing or incompatible contract
        ↓
write the smallest Zephyr-specific adapter needed
        ↓
add a test proving why that adapter exists

The objective is not to maximize Zephyr-specific code.

The objective is to produce the smallest explicit and testable PJLIB port thatruns correctly on Zephyr.