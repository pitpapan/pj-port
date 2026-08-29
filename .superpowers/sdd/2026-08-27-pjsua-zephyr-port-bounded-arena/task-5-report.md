# Task 5 report: PJSUA capacity proof

## Implementation summary

Added a focused PJSUA capacity application profile and harness. The test
creates five local accounts with distinct user-data sentinels, holds seven
incoming TCP INVITEs as call records without media, verifies that all seven
records remain independently addressable, and injects an eighth INVITE. The
eighth request follows PJSUA's native alloc_call_id() exhaustion path and
receives SIP 486 Busy Here.

The harness uses a bounded, controlled TCP peer and caller-driven
pjsua_handle_events() polling. It acknowledges each teardown 603 Decline
using the original INVITE transaction branch, waits for zero SIP transactions,
dialogs, and transient transports, and compares post-cleanup arena statistics
exactly with a post-start baseline. A bounded warm-up over-capacity exchange
stabilizes the same stateless 486 response path before the baseline is taken.

## Files changed

- applications/voip_integration/src/pjsua_capacity.c
- applications/voip_integration/pjsua_capacity.conf
- applications/voip_integration/Kconfig
- applications/voip_integration/CMakeLists.txt

## TDD RED evidence

The test/config integration was first attempted while
src/pjsua_capacity.c was absent:

~~~
PATH=/home/pitpapan/zephyrproject/.venv/bin:/usr/bin:/bin CCACHE_DISABLE=1 CMAKE_BUILD_PARALLEL_LEVEL=4 /home/pitpapan/zephyrproject/.venv/bin/west build -p always -b mps2/an385 /home/pitpapan/zephyrproject/.worktrees/voip-pjsua-plan1/applications/voip_integration -d /tmp/voip-plan1-task5-red -- -DEXTRA_CONF_FILE=pjsua_capacity.conf
~~~

The command exited 1 with:

~~~
CMake Error ... Cannot find source file:
  src/pjsua_capacity.c
...
ninja: error: rebuilding 'build.ninja': subcommand failed
~~~

This was the expected RED: the new Kconfig/CMake integration selected a
source file that had not yet been implemented.

## GREEN build and run evidence

Final fixed-profile build:

~~~
PATH=/home/pitpapan/zephyrproject/.venv/bin:/usr/bin:/bin CCACHE_DIR=/tmp/voip-ccache CCACHE_TEMPDIR=/tmp/voip-ccache-tmp /home/pitpapan/zephyrproject/.venv/bin/west build -d /tmp/voip-plan1-task5-green
~~~

The build exited 0. Final image accounting:

~~~
FLASH: 551656 B / 4 MB (13.15%)
RAM:   3734968 B / 4 MB (89.05%)
Generating files from /tmp/voip-plan1-task5-green/zephyr/zephyr.elf for board: mps2/an385
~~~

Final QEMU run:

~~~
PATH=/home/pitpapan/zephyrproject/.venv/bin:/usr/bin:/bin CCACHE_DIR=/tmp/voip-ccache CCACHE_TEMPDIR=/tmp/voip-ccache-tmp timeout 40s /home/pitpapan/zephyrproject/.venv/bin/west build -d /tmp/voip-plan1-task5-green -t run
~~~

The application emitted the required proof before the timeout stopped its
intentional idle loop:

~~~
No SIP worker threads created
Account <sip:capacity0@127.0.0.1> added with id 0
Account <sip:capacity1@127.0.0.1> added with id 1
Account <sip:capacity2@127.0.0.1> added with id 2
Account <sip:capacity3@127.0.0.1> added with id 3
Account <sip:capacity4@127.0.0.1> added with id 4
SIP/2.0 486 Busy Here
Unable to accept incoming call (too many calls)
PJSUA CAPACITY CLEANUP: baseline used=90192 live=32 cleanup used=90192 live=32
PJSUA CAPACITY ARENA: destroyed used=0 live=0 peak=274336
PJSUA CAPACITY RESULT: PASSED (5 accounts, 7 calls, eighth 486)
~~~

The shell return was 124 because timeout terminated QEMU after the PASS
marker; no capacity check failed.

## Cleanup and arena evidence

The harness:

- sends ACKs with each original z9hG4bK-capacity-N branch after the seven
  final 603 Decline responses;
- closes the controlled TCP peer;
- waits, with bounded polling, for zero PJSIP transactions and dialogs and for
  the transport count to return to its post-start value;
- deletes all five accounts;
- drains caller-driven events again; and
- requires exact equality of used_bytes and live_blocks.

The final equality was 90192/32 before and after cleanup. Final
pjsua_destroy() returned the arena to used=0, live=0.

## Configuration, security, and thread evidence

pjsua_capacity.conf fixes the accepted Task 2 profile:

~~~
CONFIG_PJSUA_MAX_ACCOUNTS=5
CONFIG_PJSUA_MAX_CALLS=7
CONFIG_PJSUA_MAX_CONF_PORTS=12
CONFIG_PJSUA_ARENA_BYTES=2097152
CONFIG_PJMEDIA_SRTP=n
CONFIG_PJMEDIA_SRTP_TRANSPORT=n
CONFIG_PJMEDIA_SRTP_SDES=n
~~~

The harness additionally sets ua_cfg.thread_cnt=0,
media_cfg.thread_cnt=0, media_cfg.has_ioqueue=PJ_FALSE,
media_cfg.enable_ice=PJ_FALSE, media_cfg.enable_turn=PJ_FALSE,
ua_cfg.stun_srv_cnt=0, and ua_cfg.enable_upnp=PJ_FALSE. Compile-time
assertions keep TLS and SRTP disabled. Only a TCP signaling transport is
created at runtime; UDP remains enabled in the profile solely because the
existing PJSUA transport-link closure references the UDP attach symbol even
when no UDP transport is instantiated.

## Self-review

- The fixed limits are asserted both at compile time and in the profile.
- Account and call IDs are checked through enumeration and user-data
  sentinels.
- Incoming calls have no media ports and are held without answering.
- The eighth call is checked by both callback count and the native SIP 486
  response.
- Teardown is deterministic and does not depend on PJSUA worker threads.
- The arena comparison is exact; no offset or tolerance is used.
- Temporary diagnostic prints were removed. git diff --check is clean.

## Concerns

The application remains alive after printing PASS, so the documented run uses
an external timeout and returns 124 after the proof. Also, the PJSIP UDP
configuration symbol must remain enabled for the current PJSUA link closure,
although this test never creates or uses a UDP transport.

## Fix round 1

Review findings were addressed in the capacity harness:

- A separate unconditional callback counter now proves exactly seven incoming
  callback invocations. The eighth response is fully processed first, then the
  harness asserts that callback count, call count, enumeration, call IDs,
  active state, sentinels, and no-media state are unchanged.
- All TCP response polling uses bounded 10 ms select waits.
- The 486 response parser accumulates bounded TCP fragments, handles multiple
  complete zero-body SIP messages, and accepts only a complete response with
  status 486, Call-ID
  pjsua-capacity-8@127.0.0.1, and CSeq: 8 INVITE.

Focused verification command:

~~~
PATH=/home/pitpapan/zephyrproject/.venv/bin:/usr/bin:/bin CCACHE_DIR=/tmp/voip-ccache CCACHE_TEMPDIR=/tmp/voip-ccache-tmp /home/pitpapan/zephyrproject/.venv/bin/west build -d /tmp/voip-plan1-task5-green
~~~

The build exited 0 with FLASH 551872 B and RAM 3734968 B. The corresponding
runtime command was:

~~~
PATH=/home/pitpapan/zephyrproject/.venv/bin:/usr/bin:/bin CCACHE_DIR=/tmp/voip-ccache CCACHE_TEMPDIR=/tmp/voip-ccache-tmp timeout 40s /home/pitpapan/zephyrproject/.venv/bin/west build -d /tmp/voip-plan1-task5-green -t run
~~~

The run emitted:

~~~
PJSUA CAPACITY CLEANUP: baseline used=90192 live=32 cleanup used=90192 live=32
PJSUA CAPACITY ARENA: destroyed used=0 live=0 peak=274528
PJSUA CAPACITY RESULT: PASSED (5 accounts, 7 calls, eighth 486)
~~~

The external timeout returned 124 after the PASS marker because the
application intentionally remains alive.

For a focused negative proof, the expected eighth Call-ID string was
temporarily mutated to pjsua-capacity-mutated@127.0.0.1 and the same fixed
profile was run. The harness rejected the actual 486 rather than accepting an
unrelated response, emitting:

~~~
PJSUA CAPACITY CHECK FAILED: warm up busy response
PJSUA CAPACITY CHECK FAILED: warm-up leaves zero calls
PJSUA CAPACITY CHECK FAILED: warm-up SIP state quiesces
~~~

The mutation was restored before the passing build and runtime verification;
no production PJPROJECT source was changed.
