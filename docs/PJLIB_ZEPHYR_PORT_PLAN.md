PJLIB → Zephyr Port Plan
1. Objective

Port PJLIB from PJPROJECT to Zephyr RTOS.

Initial scope:

PJLIB only
Zephyr RTOS
Final target: NXP i.MX RT1064 / Cortex-M7
Primary early validation target: a Zephyr-supported QEMU target when practical
Final compile target: mimxrt1064_evk

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

If a problem appears to require reading Zephyr implementation source,
stop and report the reason before doing so.

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
identify the appropriate Zephyr API;
implement the smallest correct adaptation;
verify it with compilation or runtime tests.
4. Stage 0 — Establish Build Baseline
Goal

Verify that the existing Zephyr development environment works before
introducing PJPROJECT.

Do not inspect Zephyr source files.

Actions

Run environment/build commands such as:

west --version
west topdir
west list

Verify that the intended Zephyr boards are available using normal west/Zephyr
command-line interfaces.

Build an existing minimal Zephyr application or create our own minimal
application without PJLIB.

Preferred initial runtime target:

qemu_cortex_m3

Final compile target:

mimxrt1064_evk
Completion Criteria

A pure Zephyr application:

configures successfully;
compiles successfully;
links successfully;
runs under QEMU where supported.

No PJPROJECT code is involved.

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
atomic operations;
critical sections;
time;
sleep;
memory;
pools;
sockets;
DNS/address resolution;
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
Deliverable

Create:

docs/PJLIB_PORT_ANALYSIS.md

It must contain:

PJLIB source classification;
platform abstraction boundaries;
platform-specific source files;
required configuration macros;
implementations that may be reusable;
functionality requiring a Zephyr-specific implementation;
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

and identify which pieces must change for Zephyr.

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

The application successfully builds and runs on the selected QEMU target.

It must also compile and link for:

mimxrt1064_evk

where practical.

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

PJPROJECT must be integrated through the Zephyr module mechanism rather than
being manually compiled by the application.

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
selected platform implementation

Avoid source globs where they could accidentally include mutually exclusive
platform implementations.

Completion Criteria

The build reaches actual PJLIB compilation.

Compile errors caused by missing platform functionality are acceptable at this
stage.

CMake/module-discovery failures are not.

9. Stage 5 — Zephyr Platform Configuration
Goal

Provide PJLIB with an explicit Zephyr configuration.

Create or adapt the appropriate PJ configuration mechanism, normally through
PJLIB's platform/application configuration facilities.

The Zephyr port should explicitly define relevant platform properties instead
of pretending to be Linux or another supported operating system.

Configuration Areas

Investigate and configure only when necessary:

threading;
semaphore support;
TCP;
UDP;
IPv4;
IPv6;
DNS;
floating point;
byte order;
libc facilities;
debug/logging behavior.

Do not replicate all upstream configure options.

Only introduce configuration required by the active port.

10. Stage 6 — Compile and Link PJLIB
Goal

Reach a complete PJLIB compile and link.

For every unresolved platform symbol:

error
  ↓
declaration
  ↓
existing PJ implementation
  ↓
required semantics
  ↓
Zephyr mapping

Do not create dummy functions whose only purpose is removing linker errors.

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

Investigate failures involving:

thread registration;
TLS;
synchronization;
atomics;
time;
memory initialization.
Completion Criteria

The application reports successful PJLIB initialization and shutdown using the
real implementation.

No fake success path is permitted.