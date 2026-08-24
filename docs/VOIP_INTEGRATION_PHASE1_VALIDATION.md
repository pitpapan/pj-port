# VoIP Integration Phase 1 Facade Validation

Date: 2026-08-24

## Result

Phase 1 passes on `mps2/an385` under QEMU:

```text
VOIP INTEGRATION PHASE 1 RESULT: PASSED (5 facade lifecycles)
```

The new C++ headers compile without PJPROJECT headers or types.  A bounded fake
backend validates facade construction, initialization, one-account and one-call
state, callbacks, outgoing and incoming calls, hold/resume, shutdown, and five
complete lifecycles.  Neither PJPROJECT nor networking is enabled by this
application.

## Public contract

The public header is `voip/include/voip/VoipFacade.hpp`.  It defines:

- `VoipManager`, `Backend`, and `Observer`;
- `AccountConfig`, `CallInfo`, and `Status`;
- registration, call, codec, direction, and error enums; and
- fixed maximum lengths for copied URI, username, password, reason, and
  address fields.

Normal application callbacks receive copied `CallInfo` and `Status` values.
No pointer into a PJ pool, PJ socket address, SDP object, dialog, transaction,
stream, or transport is exposed.

The manager does not initialize PJPROJECT during construction.  Phase 2 will
provide the runtime-owning production backend.  The injected `Backend`
interface exists to keep Phase 1 tests independent of PJPROJECT; it is an SDK
extension point, not a raw PJPROJECT escape hatch.

## Ownership and threading contract

- `VoipManager` does not own the injected backend or observer.  Both must
  outlive the initialized manager.
- The production backend will own the entire shared PJPROJECT runtime and the
  single account/call slots.
- Configuration strings are borrowed only for the duration of
  `ConfigureAccount()` and must be copied by the backend.  The fake demonstrates
  this contract with bounded arrays.
- Callback arguments are valid copies for the duration of the callback.  An
  application retains data by copying the structures.
- Phase 2 will serialize callbacks on the facade event thread.  Callbacks must
  not block indefinitely and must not destroy the manager.  Commands issued
  reentrantly from a callback will be queued and will not execute inline.
- `Shutdown()` rejects/drains later production work before invalidating the
  observer.  After it returns, no callback is permitted.
- Public methods are `noexcept`; errors are returned as `voip::Error`.

## Legacy compatibility matrix

The legacy implementation headers are absent from this workspace.  The table
accounts for every operation documented in `docs/voip_stack.md`; exact
signature and namespace compatibility must be checked against the product
repository before it is claimed.

| Legacy API or behavior | Facade mapping | Phase 1 status |
| --- | --- | --- |
| `SIPSessionManager::CreateRegistrable()` | Configure the single account, initialize manager, then register | Represented; legacy adapter pending headers |
| `CreateOutgoingDirectCall()` | `StartOutgoingCall()` without registration after Phase 5 policy is finalized | Represented; direct-mode policy open |
| `CreateIncommingDirectCall()` | TCP listener plus `OnIncomingCall()` | Event represented; misspelled adapter pending |
| `DeleteSession()` | `EndCall()` followed by manager/backend shutdown | Represented by deterministic ownership |
| `OrderCall()` / `OrderMakeCall()` | `StartOutgoingCall(remote_uri)` | Implemented |
| `OrderAcceptCall()` | `AcceptCall()` | Implemented |
| `OrderAbortCall()` | `EndCall()`; production backend chooses CANCEL or BYE from state | Implemented contract |
| `PendingIncomingCall(userTag)` | `Observer::OnIncomingCall(CallInfo)` | Implemented with copied remote URI |
| `EstablishCall()` | Established `OnCallState()`, followed by `OnMediaState()` | Implemented contract |
| `CallEnd()` | Disconnected `OnCallState()` | Implemented contract |
| `SessionFail()` | Failed/connection-lost registration or call callback with `Status` | Represented by richer states |
| `ReInvite()` | Call/media callback with updated direction and codec | Represented; production re-INVITE is Phase 8 |
| `GetLocalSDP()` / `GetRemoteSDP()` | Copied negotiated fields in `CallInfo` | Replaced intentionally; raw SDP is not public |
| `GetSessionsInfo()` | Single `GetCallInfo()` snapshot | Implemented one-call equivalent |
| `GetSessionsSequences()` | No protocol sequence exposure | Compatibility need must be confirmed |
| `RTPAudioManager::RunAudioReceiver/Streamer()` | Automatic PJMEDIA start after confirmed negotiation | Removed from application responsibility |
| RTP stop operations | Automatic media stop before dialog teardown | Manager `EndCall()` contract; Phase 7 implementation |
| RTP hold operations | `SetHeld()` and media-direction callbacks | Implemented contract |

## Build and runtime

Pristine build:

```sh
source .venv/bin/activate
CCACHE_DISABLE=1 CMAKE_BUILD_PARALLEL_LEVEL=1 \
west build -p always -b mps2/an385 applications/voip_integration \
  -d build-voip-integration-phase1
```

Image size:

```text
FLASH: 16108 B / 4 MB
RAM:   21496 B / 4 MB
```

Bounded run:

```sh
timeout --signal=TERM --kill-after=3s 8s \
west build -d build-voip-integration-phase1 -t run
```

The command returned 124 only after the pass marker because QEMU idles after
the test completes.

Each lifecycle covers:

- rejection of use before initialization and duplicate initialization;
- bounded account configuration and register/unregister callbacks;
- outgoing PCMU establishment and second-call rejection;
- hold and resume;
- local teardown;
- incoming notification, delayed acceptance, PCMA establishment, and teardown;
- stable copied remote URI values;
- repeated and idempotent shutdown; and
- rejection of operations after shutdown.

## Source and dependency audit

The Phase 1 application uses only the `voip` module.  Its CMake file does not
add the PJPROJECT module, and its configuration contains no `PJPROJECT`,
`PJSIP`, or `PJMEDIA` selection.  Public facade headers contain no PJ include
or PJ-prefixed type.  No upstream PJPROJECT source was modified.

## Remaining Phase 1 limitation

The compile and behavioral facade boundary is complete, but exact legacy
source compatibility cannot be verified without the legacy public headers.
The compatibility adapter must be finalized when those headers or their
product repository are supplied.  This does not block Phase 2 runtime work
behind the stable `VoipManager`/`Backend` boundary.
