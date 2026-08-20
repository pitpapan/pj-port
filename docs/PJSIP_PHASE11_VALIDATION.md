# PJSIP Phase 11 Robustness and Resource Validation

## Result

Phase 11 passes on the QEMU `mps2/an385` baseline. The final pristine image
completed five full PJLIB/PJSIP initialization and shutdown lifecycles,
including a 30.9-second active signaling soak, without an assertion, deadlock,
stale callback, or live PJ pool after teardown.

Phase 12 product integration has not started. This result validates the
current IPv4 UDP signaling and registration profile on QEMU; it is not a
real-board, call-control, media, TLS, IPv6, or production-endurance result.

## Validated environment

| Item | Value |
| --- | --- |
| PJPROJECT | 2.16 |
| Zephyr | 4.4.0 |
| West | 1.5.0 |
| CMake | 4.4.2 from `.venv` |
| Zephyr SDK | 1.0.1 |
| ARM compiler | Zephyr SDK GCC 14.3.0 |
| Board | `mps2/an385` under QEMU |
| C library/C++ library | Picolibc and LLVM libc++ |
| Network | Zephyr IPv4 loopback; no Internet service |

The validation overlay is
`applications/pjsip_minimal/phase11_robustness.conf`. Assertions, initial
stack filling, and stack information are enabled so failures and stack
watermarks remain observable.

## Tests completed

The Phase 11 entry point first runs the existing Phase 7 and Phase 10 gates,
then runs the Phase 11 resource lifecycle:

| Test | Final result |
| --- | --- |
| Complete initialization/shutdown | Passed 5/5 lifecycles: two Phase 7, two Phase 10, one Phase 11 |
| Registration lifecycle | Passed two REGISTER -> 401 Digest -> 200, automatic refresh, unregister sequences |
| Registration failures | Invalid credential 403 and timeout/recovery 408 -> 200 passed twice |
| Active event-loop soak | 30,900 ms, 30 rounds, 240 successful OPTIONS requests |
| Concurrent transactions | 8 simultaneous UAC plus 8 UAS transactions; observed peak 16 |
| Shutdown cancellation | 8 pending UAC transactions terminated with 487 while polling was quiesced; client UDP shut down before queued destroy callbacks resumed; all callbacks drained |
| Malformed/boundary UDP | Controlled rejection at 3,999, 4,000, and 4,001 bytes, plus the Phase 7 69-byte malformed and 4,256-byte oversized cases |
| Fixed-pool exhaustion | A 1,024-byte fixed pool raised and caught `PJ_NO_MEMORY_EXCEPTION` |
| Socket/ioqueue capacity | Two SIP UDP sockets plus 14 test UDP sockets filled all 16 configured endpoint handles; the 17th registration returned `PJ_ETOOMANY` |
| Timer cleanup | Peak 24 active timers in the Phase 11 lifecycle; zero before teardown |
| Pool cleanup | Steady allocation remained constant through every soak round; zero live bytes, blocks, cached capacity, and checked-out pools after endpoint destruction |

All services are application-local and deterministic. No public registrar or
DNS server is contacted.

## Supported validation limits

These are the supported limits demonstrated by this profile, not generic
limits for every future PJSIP application:

| Resource | Validated value or policy |
| --- | --- |
| SIP transports in the usable profile | 2 IPv4 UDP transports, one client and one local registrar |
| Combined endpoint ioqueue handles | 16 maximum; controlled `PJ_ETOOMANY` on handle 17 |
| Application transaction concurrency | 8 simultaneous requests, producing a measured 16 UAC/UAS transaction peak |
| SIP packet ceiling | `PJSIP_MAX_PKT_LEN=4000`; malformed inputs immediately below, at, and above the boundary are rejected without corrupting the event loop |
| Initial timer capacity | 128 configured; 24 active at the measured peak |
| Zephyr network contexts/connections | 40/40 |
| Zephyr open descriptors/poll descriptors | 48/40 |
| Event threads | One PJLIB-created event thread in the usable profile |
| Event-thread stack | 8,192 B configured; at most 2,472 B observed used |
| Main stack | 24,576 B configured; at most 13,744 B observed used |
| Zephyr system heap | 393,216 B configured |
| PJ block allocation | 290,384 B/80 blocks peak; 55,888 B/11 pools steady after each drained soak batch |

`PJSIP_MAX_TSX_COUNT=31` is a transaction hash-table sizing value in
PJPROJECT 2.16, not a hard runtime transaction cap. The supported application
limit claimed here is therefore the measured concurrency of eight requests,
not 31. Applications needing more concurrency must repeat Phase 11 with the
intended traffic mix and heap.

The PJ heap measurement wraps the public pool-factory block-allocation hooks.
It measures PJ pool block sizes but excludes allocator metadata and unrelated
Zephyr heap users. The difference between the configured system heap and the
observed PJ peak is 102,832 B before those excluded costs, so the full 393,216
B heap remains the validated setting; it should not yet be reduced to the PJ
peak.

The stack numbers are QEMU watermarks obtained through
`k_thread_stack_space_get()`. Keep the current stack sizes until the same
traffic and malformed-input cases have been measured on the product board.

## Image size

The final Phase 11 image includes the Phase 7, Phase 10, and Phase 11 test
harnesses plus global assertions and stack instrumentation.

| Image | Flash | RAM | Flash delta | RAM delta |
| --- | ---: | ---: | ---: | ---: |
| PJLIB-only Stage 10 baseline | 105,904 B | 344,736 B | - | - |
| Phase 10 usable signaling | 207,204 B | 596,888 B | +101,300 B | +252,152 B |
| Phase 11 validation image | 255,280 B | 602,904 B | +149,376 B | +258,168 B |

Relative to Phase 10, the Phase 11 validation image adds 48,076 B flash and
6,016 B RAM. That delta is validation instrumentation and compiled test code;
it is not the production cost of the PJSIP libraries.

## Shutdown policy and PJPROJECT 2.16 finding

The supported shutdown sequence is:

1. stop accepting new application work;
2. quiesce the event pump;
3. terminate active transactions with `pjsip_tsx_terminate()` and an explicit
   final status such as 487;
4. initiate transport shutdown;
5. resume the event pump and wait for transaction, timer, transport, and
   application callbacks to drain;
6. destroy modules, endpoint, caching pool, and PJLIB in ownership order.

During failure-oriented testing, `pjsip_tsx_terminate_async()` in PJPROJECT
2.16 was found to validate its `code` argument without storing it. Its timer
later calls synchronous termination with the transaction's old zero status,
which asserts. No PJPROJECT source was patched. The Phase 11 supported path
uses the synchronous API while polling is quiesced, then resumes polling only
after transport shutdown begins. Applications on this exact version should
follow that path or independently validate an upstream upgrade before using
the asynchronous helper.

## Logging policy

Phase 7 and Phase 10 use PJ log level 3 to expose transport and registration
state. The sustained Phase 11 lifecycle sets runtime level 1, so 240 soak
requests do not flood the console or print authentication material.

For a production build, start with runtime level 2 or lower, never log full
Authorization headers or credentials, and rate-limit repeated parser/network
errors. Raising the level to 3 or above should be a temporary diagnostic
operation. Compile-time log removal and a product-specific log sink remain
Phase 12 decisions.

## Commands and observed results

The authoritative clean build was:

```sh
source .venv/bin/activate
source zephyr/zephyr-env.sh
CCACHE_DISABLE=1 CMAKE_BUILD_PARALLEL_LEVEL=1 \
west build -p always -b mps2/an385 applications/pjsip_minimal \
  -d build-pjsip-phase11 -- -DEXTRA_CONF_FILE=phase11_robustness.conf
```

It completed 334 build steps with no compiler or linker error and reported:

```text
FLASH: 255280 B / 4 MB
RAM:   602904 B / 4 MB
```

The authoritative run was:

```sh
source .venv/bin/activate
source zephyr/zephyr-env.sh
CCACHE_DISABLE=1 timeout --signal=TERM --kill-after=5s 120s \
west build -d build-pjsip-phase11 -t run
```

The application printed:

```text
PHASE 7 RESULT: PASSED (2/2 lifecycles)
PHASE 10 RESULT: PASSED (2/2 lifecycles)
PHASE 11 RESULT: PASSED (5 complete lifecycles; 30-second active soak)
```

The shell returned 124 only because `timeout` stopped QEMU after the completed
application had entered idle. QEMU reported termination by signal 15 and no
background QEMU process was intentionally left running.

Failure investigation also used the built ELF without reading Zephyr source:

```sh
/home/pitpapan/zephyr-sdk-1.0.1/gnu/arm-zephyr-eabi/bin/arm-zephyr-eabi-addr2line \
  -a -f -C -e build-pjsip-phase11/zephyr/zephyr.elf <fault-addresses>
```

The first two full runtime attempts passed their soaks but asserted in the
asynchronous cancellation helper. Short diagnostic runs with assertions and
PJ trace logging isolated the zero-status call described above. After the
shutdown policy was corrected, both the subsequent full run and the final
pristine full run passed.

## Known diagnostics and remaining gates

The pristine build retains the previously known diagnostics: inactive QEMU
icount assignment, test-only non-secure random generator, excluded empty
entropy target, and upstream unused/TODO/parser warnings. `CONFIG_ASSERT=y`
also deliberately announces that assertions are globally enabled.

The QEMU test random generator is not a production entropy source for SIP
tags, branch IDs, Call-IDs, nonces, or Digest cnonce values. A real entropy
policy, persistent/valid clock policy where needed, real-board memory and CPU
measurements, longer endurance testing, and product logging are still required
before deployment. Those belong to Phase 12 and were not started here.
