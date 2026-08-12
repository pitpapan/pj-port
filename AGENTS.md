# Project Context

This workspace is used to port PJPROJECT to Zephyr RTOS.

## Target

- RTOS: Zephyr
- Final MCU target: NXP i.MX RT1064, Cortex-M7
- Zephyr board target: mimxrt1064_evk
- Initial porting scope: PJLIB only
- PJLIB-UTIL, PJNATH, PJMEDIA, and PJSIP are out of scope until PJLIB is validated.

## Repository Layout

- `zephyr/`: Zephyr source tree
- `pjproject/`: PJPROJECT upstream repository / Zephyr port
- `applications/`: local test applications for the port
- `.west/`: west workspace metadata

Treat `pjproject/` as an independent Git repository.

## Porting Strategy

Work incrementally.

For each meaningful change:

1. Inspect the relevant existing implementation.
2. Explain the intended change.
3. Make the smallest coherent modification.
4. Build or run the relevant test.
5. Inspect actual errors or runtime behavior before continuing.

Do not attempt to port the entire PJPROJECT stack at once.

## Build-System Policy

PJPROJECT in this workspace only needs to support Zephyr.

It is acceptable to simplify or replace upstream build-system logic for the Zephyr port.

However:

- Do not guess PJLIB source-file selection.
- Inspect upstream build files and source organization first.
- Do not compile all `*.c` files indiscriminately.
- Avoid filesystem paths specific to this machine.
- Use Zephyr module, CMake, and Kconfig mechanisms where appropriate.

## Porting Policy

Do not create fake implementations merely to make the project compile.

Do not:

- return unconditional success from unimplemented APIs;
- silently disable required PJLIB functionality;
- replace missing behavior with empty stubs without explicitly reporting it;
- pretend Zephyr is Linux or another OS just to pass preprocessing;
- alter PJLIB public API semantics without a documented reason.

Before implementing an OS abstraction, inspect the corresponding upstream implementation and determine the semantics PJLIB expects.

Prefer small, reviewable changes.

## Platform Abstraction

Evaluate each subsystem independently:

- initialization
- memory and pools
- thread registration
- threads
- mutexes
- semaphores
- TLS
- time
- atomic operations
- sockets
- DNS/network helpers

Do not assume POSIX compatibility only because API names are similar.

Use Zephyr native APIs where they provide a better and clearer implementation, while reusing compatible BSD/POSIX interfaces where appropriate.

## Verification

A task is not complete merely because code was generated.

Run relevant builds and tests.

For Zephyr/QEMU work, use actual `west build` / runtime results where possible.

For the final target, verify at least compile and link for `mimxrt1064_evk` even when hardware execution is unavailable.

Always report:

- files changed;
- commands executed;
- build/test result;
- unresolved issues;
- assumptions that remain unverified.

## Scope Control

Do not make unrelated refactors.

Do not proceed to PJLIB-UTIL, PJNATH, PJMEDIA, or PJSIP unless explicitly instructed.

When a task specifies a particular phase, stop at the end of that phase.