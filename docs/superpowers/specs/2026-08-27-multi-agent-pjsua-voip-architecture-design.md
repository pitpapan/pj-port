# Multi-Agent PJSUA VoIP Architecture Design

**Date:** 2026-08-27

**Status:** Approved

## 1. Purpose

This document defines the replacement architecture for the lowercase `voip/`
service. The replacement may break the existing `VoipService`, `VoipManager`,
and `Backend` APIs.

The architecture supports up to five configured agent endpoints, no more than
two promoted calls globally, no more than one promoted call per agent, and a
five-entry FIFO for additional incoming and outgoing calls. It uses
PJPROJECT's PJSUA-LIB rather than continuing to implement account and call
machinery directly with low-level PJSIP-UA APIs.

The design targets Zephyr-based embedded systems with bounded storage and no
use of the general system heap after successful initialization. It does not
require reading or modifying Zephyr implementation sources.

## 2. Product Decisions

The following decisions are fixed for the first implementation:

- Between one and five agent endpoints are configured during initialization.
- The configured topology is immutable until shutdown.
- SIP identity, credentials, microphone, and speaker bindings are fixed until
  shutdown.
- Each agent owns one independent PJSUA account and registration lifecycle.
- At most two calls are promoted globally.
- At most one call is promoted for a particular agent.
- Five additional calls may wait in one global FIFO.
- Incoming and outgoing requests share the same FIFO.
- FIFO order is strict. A busy agent at the head causes head-of-line blocking;
  later entries do not bypass it.
- A queued incoming call retains a lightweight PJSUA call/dialog record and
  receives `180 Ringing`.
- A queued outgoing request does not create a PJSUA call until promotion.
- The application may answer or reject an incoming call while it is queued.
- `Answer()` on a queued call records an answer-on-promotion decision.
- PJSUA-LIB is used through its C API. PJSUA2 is not used initially.
- One actor thread owns PJSUA and all mutable signaling state.
- PJSUA worker threads are disabled; the actor drives `pjsua_handle_events()`.
- Application events are consumed through a fixed polling queue, not callbacks.
- No C++ allocation and no general Zephyr/system heap allocation occur after
  initialization.
- PJPROJECT runtime allocations after initialization come only from a dedicated
  fixed-capacity VoIP arena.
- Initial signaling uses SIP over TCP.
- Initial media uses plain RTP/RTCP over UDP.
- TLS and SRTP retain explicit policy and adapter boundaries but are disabled
  initially.
- Codec selection and codec-priority policy are outside this architecture
  revision. The media boundary remains format-aware.

## 3. Goals

- Replace the single-account, single-call compatibility facade with an honest
  multi-agent service.
- Use PJSUA-LIB for standard account, registration, call, SDP, hold, RTP/RTCP,
  and media lifecycle behavior.
- Keep product admission and FIFO policy independent of PJPROJECT.
- Split the current `PjVoipBackend` responsibilities into focused components.
- Make ownership, callback routing, teardown, and handle lifetime explicit.
- Provide deterministic behavior under queue, event, operation, and memory
  pressure.
- Connect each promoted call to the microphone and speaker assigned to its
  agent without cross-agent audio mixing.
- Preserve room for TLS and SRTP without enabling or pretending to support
  them in the first implementation.
- Keep the legacy uppercase `modules/VOIP` stack available until product
  acceptance of the replacement.

## 4. Non-Goals

- Preserving source compatibility with the current lowercase `voip/` API.
- Runtime addition, removal, or reconfiguration of agents.
- More than five configured agents.
- More than two promoted calls.
- More than one promoted call for one agent.
- Priority scheduling or queue bypass.
- PJSUA2.
- Selecting the production codec set.
- Enabling TLS or SRTP in the first implementation.
- Retiring the legacy uppercase stack as part of initial development.

## 5. Terminology

### Agent endpoint

A logical product endpoint containing one SIP identity, one registration
lifecycle, and immutable application-owned audio bindings.

### Logical call

A public call identified by `CallHandle`. It may be queued without a PJSUA call
ID, queued with a lightweight incoming PJSUA call ID, or promoted with signaling
and optionally media resources.

### Promoted call

A call occupying one of the two global call-processing slots. The slot is held
from promotion through signaling and complete teardown, including outgoing,
incoming, early, established, held, and disconnecting states.

### Queued call

A logical call in the five-entry global FIFO. It consumes no media slot. A
queued incoming call consumes a PJSUA signaling record; a queued outgoing call
does not.

### Actor thread

The only thread allowed to mutate agent, scheduler, operation, logical call, or
PJSUA signaling state.

## 6. High-Level Architecture

```text
Application threads
    | bounded commands
    v
VoipService
    | fixed command mailbox
    v
VoipRuntime actor thread
    +-- AgentRegistry[5]
    +-- CallScheduler
    |     +-- PromotedCallPool[2]
    |     +-- PendingCallQueue[5]
    +-- OperationTable[16]
    +-- VoipEventQueue[32]
    +-- VoipResourceGuard
    +-- PjsuaRuntime
          +-- PjsuaAccountManager
          |     +-- PjsuaAccountContext[5]
          +-- PjsuaCallManager
          |     +-- PjsuaCallContext[7]
          +-- PjsuaMediaManager
          |     +-- PjsuaMediaBridge[2]
          +-- PjsuaTransportManager
          +-- optional PjsuaDiagnostics

PJMEDIA media threads
    | bounded PCM callbacks and atomic reports
    v
PjsuaMediaBridge[2]
    | application-owned interfaces
    v
Agent PcmSource / PcmSink
```

The service owns downward. No account, call, or media component outlives the
runtime. No PJPROJECT type appears in a public header.

## 7. Component Responsibilities

### 7.1 VoipService

- Owns all fixed production storage.
- Validates and copies initialization configuration.
- Starts and stops the actor.
- Validates public handles before command admission.
- Copies commands into the fixed mailbox.
- Exposes the event polling and snapshot APIs.
- Does not invoke mutable PJSUA APIs from application threads.

### 7.2 VoipRuntime

- Orchestrates initialization and ordered shutdown.
- Runs the actor loop.
- Processes application commands, media-control messages, timers, and PJSUA
  events.
- Composes managers without absorbing their behavior.

### 7.3 AgentRegistry

- Stores one to five copied, PJ-independent agent definitions.
- Assigns generation-protected `AgentHandle` values.
- Maps configuration-array indexes to public handles.
- Rejects duplicate SIP identities and duplicate audio bindings.
- Owns stable `AgentContext` records containing the copied SIP configuration
  and immutable borrowed `AgentAudioBinding` pointers.
- Tracks copied public registration and active-call snapshots.

### 7.4 CallScheduler

- Owns the two-slot admission policy and five-entry strict FIFO.
- Enforces one promoted call per agent.
- Allocates and invalidates logical call handles.
- Applies queue cancellation and timeout behavior.
- Promotes calls only after previous call cleanup is complete.
- Contains no PJPROJECT types.

### 7.5 OperationTable

- Stores up to 16 independently pending operations.
- Assigns nonzero `OperationId` values.
- Reserves terminal-event capacity before accepting a command.
- Completes every accepted operation exactly once.
- Never stores a pointer to an application stack object.

### 7.6 VoipEventQueue

- Stores 32 copied public events in fixed storage.
- Has one producer, the actor, and one application polling consumer.
- Uses a semaphore to implement bounded waiting.
- Guarantees terminal events and selected admission notifications.
- Coalesces repetitive intermediate snapshots for the same handle.
- Remains readable after the actor stops and until the service is destroyed.

### 7.7 PjsuaRuntime

- Owns `pjsua_create()`, `pjsua_init()`, `pjsua_start()`,
  `pjsua_handle_events()`, and `pjsua_destroy()`.
- Configures PJSUA with zero worker threads.
- Installs and removes the one process-global callback router.
- Owns the dedicated PJ pool arena integration.
- Enforces exactly one initialized production `VoipService` because PJSUA is
  process-global.

### 7.8 PjsuaAccountManager

- Owns five stable `PjsuaAccountContext` records.
- Maps `AgentHandle` to `pjsua_acc_id` and back.
- Keeps only PJ-specific account/registration data plus an `AgentHandle`
  back-reference; it does not own or directly store audio devices.
- Adds accounts, credentials, registrar settings, and user data.
- Controls independent registration, refresh, retry, and unregister behavior.
- Routes `on_reg_state2` to the correct agent.
- Erases copied credentials during rollback and shutdown.

### 7.9 PjsuaCallManager

- Owns seven stable signaling contexts.
- Maps `CallHandle` to `pjsua_call_id` and back.
- Routes incoming-call, call-state, SDP, and media callbacks.
- Executes scheduler decisions through PJSUA call APIs.
- Translates PJSUA and SIP results into public status categories.
- Does not decide admission order.

### 7.10 PjsuaMediaManager

- Owns two fixed media bridges.
- Allocates a bridge only to a promoted call.
- Starts, changes direction, stops, and releases media.
- Reports bounded failure messages to the actor.
- Prevents one call from acquiring another agent's audio binding.

### 7.11 PjsuaTransportManager

- Owns the initial SIP TCP transport.
- Carries the signaling-security policy through the architecture.
- Reserves an adapter boundary for future TLS without enabling it.
- Routes transport failures to affected accounts and calls.

### 7.12 VoipResourceGuard and PjsuaDiagnostics

`VoipResourceGuard` is always enabled and contains only correctness counters:

- Arena allocation failure
- Available command, operation, event, queue, and media capacity
- Active and queued call counts
- Audio callback failure

It has no thread, timer, queue, or dynamic storage.

`PjsuaDiagnostics` is compiled only with `CONFIG_VOIP_DIAGNOSTICS=y`. It adds
arena peaks and fragmentation, stack high-water marks, queue peaks, PJSUA
timer/transaction/dialog/socket counts, and detailed RTP/audio statistics.

## 8. PJSUA-LIB Port Boundary

The current Zephyr PJPROJECT port compiles PJSIP core, selected PJSIP-UA files,
and selected PJMEDIA files. It does not currently compile PJSUA-LIB. A new
`CONFIG_PJSUA` boundary will add the required `pjsip/src/pjsua-lib` source
closure. PJSUA2 and the sample application stay disabled.

Compile-time PJSUA limits are:

```text
PJSUA_MAX_ACC   = 5
PJSUA_MAX_CALLS = 7
PJSUA_MAX_CONF_PORTS = 12
```

Seven PJSUA call records support the worst case of two promoted calls and five
queued incoming calls. Twelve conference-port records cover PJSUA's call-port
bound plus two custom application audio ports without retaining the upstream
254-entry arrays. The product still limits active media sessions to two.

PJSUA is configured with:

```text
thread_cnt = 0
initial transport = PJSIP_TRANSPORT_TCP
initial media security = PJMEDIA_SRTP_DISABLED
platform sound device = disabled
```

The actor calls `pjsua_handle_events()` after draining bounded application and
media messages. PJSUA callbacks therefore execute on the actor thread. PJSUA
account and call user-data APIs route callbacks to stable preallocated
contexts.

The static callback router exists solely because PJSUA's callback table is
process-global. It is installed only after complete runtime construction and
cleared only after PJSUA callbacks have been drained during shutdown. It cannot
be overwritten by a second service instance.

## 9. Public API

The following declarations show the intended contract. Exact declarations may
be split among focused public headers without changing these semantics.

```cpp
namespace voip {

struct AgentHandle {
    std::uint8_t slot;
    std::uint16_t generation;
};

struct CallHandle {
    std::uint8_t slot;
    std::uint16_t generation;
};

using OperationId = std::uint32_t;

enum class SignalingSecurity : std::uint8_t {
    none,
    tls,
};

enum class MediaSecurity : std::uint8_t {
    none,
    srtp_sdes,
};

struct SecurityPolicy {
    SignalingSecurity signaling;
    MediaSecurity media;
};

class PcmSource;
class PcmSink;

struct AgentAudioBinding {
    PcmSource *source;
    PcmSink *sink;
};

enum class SampleFormat : std::uint8_t {
    signed_16,
};

struct PcmFormat {
    std::uint32_t sample_rate_hz;
    std::uint16_t samples_per_frame;
    std::uint8_t channels;
    SampleFormat sample_format;
};

class PcmSource {
public:
    virtual PcmFormat Format() const noexcept = 0;
    virtual Error Read(std::int16_t *samples, std::size_t sample_count,
                       std::uint64_t timestamp) noexcept = 0;
};

class PcmSink {
public:
    virtual PcmFormat Format() const noexcept = 0;
    virtual Error Write(const std::int16_t *samples,
                        std::size_t sample_count,
                        std::uint64_t timestamp) noexcept = 0;
    virtual void Flush() noexcept = 0;
};

struct SipAccountConfig {
    const char *identity_uri;
    const char *registrar_uri;
    const char *auth_username;
    const char *auth_password;
};

struct AgentConfig {
    SipAccountConfig sip;
    AgentAudioBinding audio;
    bool register_on_start;
};

struct ServiceConfig {
    const AgentConfig *agents;
    std::uint8_t agent_count;
    std::uint32_t queue_timeout_ms;
    std::uint32_t answer_timeout_ms;
    PcmFormat conference_format;
    SecurityPolicy security;
};

class VoipService final {
public:
    Error Initialize(const ServiceConfig &) noexcept;
    Error Shutdown() noexcept;

    Error GetAgentHandle(std::uint8_t config_index,
                         AgentHandle *) const noexcept;

    Error Dial(AgentHandle, const DialRequest &,
               CallHandle *, OperationId *) noexcept;
    Error Answer(CallHandle, OperationId *) noexcept;
    Error Reject(CallHandle, std::uint16_t sip_status,
                 OperationId *) noexcept;
    Error Cancel(CallHandle, OperationId *) noexcept;
    Error Hangup(CallHandle, OperationId *) noexcept;
    Error SetHeld(CallHandle, bool, OperationId *) noexcept;

    Error TryGetEvent(Event *) noexcept;
    Error WaitForEvent(Event *, std::uint32_t timeout_ms) noexcept;

    Error GetAgentSnapshot(AgentHandle,
                           AgentSnapshot *) const noexcept;
    Error GetCallSnapshot(CallHandle,
                          CallSnapshot *) const noexcept;
    ResourceSnapshot GetResourceSnapshot() const noexcept;
};

} // namespace voip
```

Initialization copies all strings and credentials. Application string buffers
need to remain valid only for the duration of `Initialize()`. Audio objects are
application-owned and must remain valid until `Shutdown()` completes.

The internal ownership split is:

```cpp
struct AgentContext {
    AgentHandle handle;
    OwnedSipAccountConfig sip;
    AgentAudioBinding audio;
    CallHandle promoted_call;
};

struct PjsuaAccountContext {
    pjsua_acc_id account_id;
    AgentHandle agent;
    RegistrationState registration;
    RetryState retry;
};
```

`AgentContext` belongs to `AgentRegistry`. `PjsuaAccountContext` belongs to
`PjsuaAccountManager` and reaches the product agent only through its stable
handle. On call promotion, `PjsuaMediaManager` resolves that handle through
`AgentRegistry` and binds a free media bridge to `AgentContext::audio`.

```text
pjsua_acc_id
    -> PjsuaAccountContext
    -> AgentHandle
    -> AgentRegistry / AgentContext
    -> AgentAudioBinding
    -> PjsuaMediaBridge
```

Only the `{SignalingSecurity::none, MediaSecurity::none}` policy is accepted in
the first implementation. Selecting TLS or SRTP while the corresponding
Kconfig option is disabled returns `unsupported_configuration` during
initialization.

## 10. Handles and Operations

- Agent handles are assigned in configuration-array order.
- Agent handles remain valid from successful initialization through shutdown.
- Call handles are allocated when an incoming or outgoing request is admitted,
  including queued requests.
- Handle generations are incremented on release and initialization boundaries.
- A delayed command containing a stale generation is rejected.
- Public PJSUA account and call IDs do not exist.
- Immediate validation or capacity failure returns without allocating an
  operation ID.
- An accepted asynchronous command receives one nonzero operation ID and one
  terminal operation event.
- Operation completion means the command was applied to local state or sent to
  PJSUA. Network-driven call establishment is reported by later call events.

## 11. Event Model

The application uses one polling thread to consume events. Multiple application
threads may submit commands.

Guaranteed events are:

- Incoming-call admission
- Operation completion or failure
- Terminal call state
- Registration failure
- Service stopped

Coalescible events are:

- Intermediate registration changes
- Queue-position changes
- Early call changes
- Media statistics
- Resource-pressure diagnostics

Every event contains a monotonically increasing sequence number. When a
coalescible event for the same handle is already pending, the newest full
snapshot replaces it.

An operation is accepted only after reserving capacity for its terminal event.
If an external incoming call cannot reserve its incoming-call notification, it
is rejected with `486 Busy Here` rather than becoming invisible. Capacity for
the final `service_stopped` event is reserved for the entire initialized
lifecycle and is unavailable to ordinary events.

`TryGetEvent()` returns immediately. `WaitForEvent()` blocks the polling thread
on a semaphore for at most the requested number of milliseconds. Neither API
invokes PJPROJECT.

## 12. Scheduling

Promotion requires:

```text
promoted_call_count < 2
AND
target agent has no promoted call
```

Scheduling steps are:

1. Admit a new request only if a logical call slot and required guaranteed-event
   capacity exist.
2. Promote immediately if both eligibility conditions hold and there is no
   earlier FIFO entry.
3. Otherwise append to the five-entry FIFO.
4. Inspect only the FIFO head when capacity changes.
5. If the head's agent is busy, preserve strict FIFO and leave any other global
   slot idle.
6. Remove entries on local cancel/reject, remote CANCEL, or queue timeout.
7. Promote repeatedly after removal or complete call cleanup until capacity is
   full, the queue is empty, or the head is ineligible.

If the FIFO is full, an outgoing `Dial()` returns `queue_full` and an incoming
call receives `486 Busy Here`. This matches PJSUA-LIB's stateless response when
all seven PJSUA call records are already occupied.

Outgoing dialing is admitted only for a currently registered agent. A queued
outgoing request holds copied URI and dial options but no PJSUA call ID.

## 13. Incoming Call Policy

- PJSUA maps an incoming call to an account ID.
- Unknown or unconfigured identities receive `404 Not Found`.
- An admitted incoming call receives a logical handle and `180 Ringing`.
- If promotion is unavailable, it enters `queued_incoming` with a queue timer.
- `Answer()` while queued sets `answer_on_promotion`.
- `Reject()` while queued sends the selected response and removes the entry.
- On promotion, a pre-answered call sends `200 OK`; otherwise it enters
  `incoming` and starts its answer timer.
- A queue timeout sends `480 Temporarily Unavailable`.
- A promoted answer timeout rejects the call and releases the promoted slot.
- Remote CANCEL wins over a pending local answer and removes the call.

## 14. Call State Machines

Outgoing:

```text
queued_outgoing
    -> promoting
    -> outgoing
    -> early
    -> established <-> held
    -> disconnecting
    -> disconnected | failed | cancelled | timed_out
```

Incoming:

```text
queued_incoming
    -> promoting
    -> incoming or immediate answer
    -> established <-> held
    -> disconnecting
    -> disconnected | failed | cancelled | timed_out
```

The scheduler releases a promoted slot only after signaling teardown, media
stop, callback quiescence, future security-context erasure, and resource
accounting complete.

The terminal event contains the complete final snapshot. The logical handle is
then invalidated by generation increment. A delayed event may contain the old
handle, but that handle cannot control a reused slot.

## 15. Registration

Each configured agent owns an independent account context and registration
state:

```text
disabled
registering
registered
refreshing
retry_wait
unregistering
authentication_failed
transport_failed
```

Initialization validates the full configuration before adding any PJSUA
account. Adding accounts is transactional: if any account add fails, the
manager removes previously added accounts, destroys the runtime, erases copied
credentials, and returns an initialization error.

Agents marked `register_on_start` begin registration after all accounts and the
actor are ready. A server-side registration failure becomes an independent
agent event and does not shut down other agents.

Recoverable failures retry after 1, 2, 4, 8, 16, and then 30 seconds. Later
retries stay at 30 seconds. A bounded per-agent jitter prevents synchronized
retry bursts. Successful registration resets retry state. Authentication
failure does not retry automatically.

Incoming calls may still arrive for a configured account while registration is
refreshing if they arrive through a valid existing transport.

## 16. Media and Audio

PJSUA runs without a platform sound device by calling
`pjsua_set_no_snd_dev()`. Two preallocated media bridges connect promoted calls
to application audio.

Each `PjsuaMediaBridge` owns a custom `pjmedia_port`:

- `get_frame()` reads from the bound agent's `PcmSource`.
- `put_frame()` writes to the bound agent's `PcmSink`.
- `pjsua_conf_add_port()` registers the custom port.
- One conference connection carries source audio to the call.
- One conference connection carries call audio to the sink.
- Calls are connected directly and are never mixed together.

The audio interface is format-aware:

```cpp
struct PcmFormat {
    std::uint32_t sample_rate_hz;
    std::uint16_t samples_per_frame;
    std::uint8_t channels;
    SampleFormat sample_format;
};
```

The first implementation supports signed 16-bit PCM at the service-configured
conference format. Initialization rejects incompatible source/sink bindings.

PJMEDIA may invoke media callbacks from media threads. Those callbacks use
stable bridge storage, atomic running/stopping flags, bounded nonblocking audio
operations, and a bounded mailbox to report control failures to the actor.

Teardown order is:

1. Mark the bridge stopping.
2. Disconnect conference directions.
3. Stop the PJSUA media stream.
4. Wait for media callback quiescence.
5. Remove the custom conference port.
6. Flush the sink.
7. Release the bridge to the two-entry pool.

Queued calls never allocate RTP sockets, conference ports, codecs, or audio
bridges.

Hold and remote direction changes follow negotiated SDP:

```text
sendrecv -> transmit and receive
sendonly -> receive only
recvonly -> transmit only
inactive -> neither direction
```

A local hold/resume operation completes only after its re-INVITE succeeds. A
failed operation preserves the previous media state. Resume flushes stale sink
data before restoring reception.

## 17. Security Extension Points

Security remains part of the type and component architecture:

```text
PjsuaTransportManager
    +-- TCP, initially enabled
    +-- TLS adapter, reserved behind CONFIG_VOIP_SIP_TLS

MediaSecurityPolicy
    +-- plain RTP/RTCP, initially enabled
    +-- SRTP/SDES adapter, reserved behind CONFIG_VOIP_SRTP
```

Initial builds use:

```text
CONFIG_VOIP_SIP_TLS=n
CONFIG_VOIP_SRTP=n
SignalingSecurity::none
MediaSecurity::none
```

The first implementation does not compile or activate TLS/SRTP behavior. The
architecture carries policy into transport, SDP, call, and media boundaries so
secure implementations can be added later without redesigning ownership or the
public API.

The initial plain configuration provides no signaling confidentiality, media
encryption, or protection from an on-path attacker. Deployment must account for
that limitation.

## 18. Memory and Resource Model

Fixed product capacities are:

```text
Agent contexts                  5
Logical/PJSUA call contexts     7
Promoted call slots             2
Media bridges                   2
Pending FIFO entries            5
Command records                16
Operation records              16
Event records                  32
Registration retry records      5
```

All C++ component storage, command/event rings, audio control buffers, and the
PJ pool arena are allocated before initialization succeeds.

After initialization:

- No C++ `new` or `delete` is permitted.
- The general system heap is not used.
- PJ pools allocate only from the dedicated fixed arena.
- Arena exhaustion returns `resource_exhausted` and never falls back.

The PJSUA port may add one narrow hook that installs the external fixed arena
pool factory. It must not spread product allocator logic through upstream
PJSUA implementation files.

Arena qualification starts with a two-megabyte QEMU arena. The five-account,
two-promoted, five-queued worst-case matrix records current usage, peak usage,
largest free block, fragmentation, and failures. Normal soak must remain at or
below 75% usage; failure scenarios must remain below 90%. Target-board size is
the smallest aligned fixed arena that keeps the measured worst-case peak below
75%. If that size exceeds the approved target RAM budget, PJSUA adoption stops
for architectural reconsideration rather than falling back to unbounded heap.

## 19. Initial Kconfig Model

```text
CONFIG_VOIP_SERVICE=y
CONFIG_VOIP_PJSUA=y
CONFIG_VOIP_MAX_AGENTS=5
CONFIG_VOIP_MAX_PROMOTED_CALLS=2
CONFIG_VOIP_PENDING_CALL_CAPACITY=5
CONFIG_VOIP_COMMAND_CAPACITY=16
CONFIG_VOIP_OPERATION_CAPACITY=16
CONFIG_VOIP_EVENT_CAPACITY=32

CONFIG_VOIP_DIAGNOSTICS=n
CONFIG_VOIP_SIP_TLS=n
CONFIG_VOIP_SRTP=n
CONFIG_VOIP_PJSUA2=n
```

Build-time assertions ensure that service capacities and PJSUA maxima agree.
Old and new VoIP stacks are mutually exclusive in a product image.

## 20. Concurrency Rules

- Application APIs may be called concurrently unless documented as polling-only.
- Exactly one application thread consumes the event queue.
- Only the actor mutates domain, scheduler, operation, account, call, and PJSUA
  state.
- Snapshot readers copy from a separately synchronized public cache.
- PJSUA callbacks route to the actor-owned managers and never invoke application
  code.
- Media threads access only stable audio bridges, audio objects, ring state, and
  atomic counters.
- Media threads report lifecycle failures through a bounded actor mailbox.
- No queued command references caller-owned stack completion state.

## 21. Error Model

Public failure categories are:

```text
invalid_argument
invalid_handle
invalid_state
unsupported_configuration
agent_unavailable
busy
queue_full
resource_exhausted
authentication_failed
signaling_failed
remote_rejected
negotiation_failed
media_failed
cancelled
timed_out
shutting_down
shutdown_timeout
internal_failure
```

`Status` also carries the SIP response code and a sanitized diagnostic code.
PJSUA/PJPROJECT pointers and credential-bearing strings are never exposed.
Remote SIP rejection is not classified as a transport failure.

## 22. Shutdown

`Shutdown()` is synchronous and must not be called from the event polling
consumer while that same thread is blocked inside `WaitForEvent()`.

Ordered shutdown is:

1. Stop command and incoming-call admission.
2. Post a shutdown command using service-owned synchronization storage.
3. Cancel queued outgoing requests.
4. Reject queued incoming calls.
5. Tear down promoted calls and media.
6. Wait for media callback quiescence.
7. Unregister all accounts.
8. Drain PJSUA callbacks and timers required for teardown.
9. Remove accounts and transport.
10. Destroy PJSUA and its fixed arena pools.
11. Erase credentials.
12. Invalidate handles.
13. Enqueue `service_stopped` as the final event.
14. Stop the actor and return.

Events already queued remain readable after shutdown until drained or until the
service object is destroyed. No new event is produced after `service_stopped`.

A bounded shutdown timeout returns `shutdown_timeout` while preserving all live
storage in a shutting-down state. It never frees storage still reachable by a
callback.

## 23. Testing Strategy

### 23.1 Host unit tests

Pure C++ tests cover:

- Handle generations and stale-handle rejection
- Configuration validation for one through five agents
- Two-slot admission
- One promoted call per agent
- Five-entry FIFO ordering
- Strict head-of-line blocking
- Mixed incoming/outgoing ordering
- Cancellation and timeout removal
- Answer while queued
- Operation completion exactly once
- Guaranteed event reservation
- Intermediate event coalescing
- Shutdown state-machine order

### 23.2 PJSUA port tests

QEMU tests cover:

- PJSUA create/init/start/event handling/destroy
- Zero PJSUA worker threads
- Five accounts
- Seven signaling call records
- TCP signaling and Digest registration
- Plain RTP/RTCP
- Custom no-sound-device media port
- Fixed-arena allocation and exhaustion
- Initialization rollback at every stage
- Repeated independent boot lifecycles

The PJSUA port milestone must pass before production service integration.

### 23.3 Service integration tests

- Five independent registrations
- One account authentication failure isolated from the other four
- Two simultaneous calls on different agents
- A second call for the same agent remaining queued
- Two promoted plus five queued calls
- Full-queue incoming and outgoing behavior
- Strict FIFO head-of-line behavior
- Mixed incoming/outgoing FIFO
- Queued answer, reject, cancel, timeout, and remote CANCEL
- BYE, remote rejection, transport loss, and malformed SDP
- Hold/resume and remote direction changes
- Correct per-agent audio routing with no cross-agent audio
- One-call media failure isolation
- Registration refresh during calls
- Multi-threaded command submission
- Slow event consumer and coalescing
- Shutdown with calls, queue entries, retries, and pending events
- No event after `service_stopped`
- Disabled TLS/SRTP policies returning `unsupported_configuration`

### 23.4 Qualification

Diagnostic builds add arena and stack high-water checks, fragmentation,
operation/event pressure, packet loss/reordering/jitter, audio underrun/overrun,
100 repeated QEMU lifecycles, active-call soak, real registrar/PBX
interoperability, and target-board audio/CPU/latency/DMA qualification.

Loopback QEMU success is reported as QEMU integration success, not production
completion.

## 24. Source Layout

```text
voip/
|-- include/voip/
|   |-- VoipService.hpp
|   |-- VoipTypes.hpp
|   |-- VoipEvents.hpp
|   `-- PcmAudio.hpp
|-- src/
|   |-- core/
|   |   |-- AgentRegistry.hpp/.cpp
|   |   |-- AgentContext.hpp
|   |   |-- CallScheduler.hpp/.cpp
|   |   |-- CallContext.hpp
|   |   |-- OperationTable.hpp/.cpp
|   |   |-- VoipEventQueue.hpp/.cpp
|   |   |-- HandlePool.hpp
|   |   `-- VoipResourceGuard.hpp/.cpp
|   |-- pjsua/
|   |   |-- PjsuaRuntime.hpp/.cpp
|   |   |-- PjsuaCallbackRouter.hpp/.cpp
|   |   |-- PjsuaAccountManager.hpp/.cpp
|   |   |-- PjsuaAccountContext.hpp
|   |   |-- PjsuaCallManager.hpp/.cpp
|   |   |-- PjsuaCallContext.hpp
|   |   |-- PjsuaMediaManager.hpp/.cpp
|   |   |-- PjsuaMediaBridge.hpp/.cpp
|   |   |-- PjsuaAudioPort.hpp/.cpp
|   |   |-- PjsuaPoolArena.hpp/.cpp
|   |   |-- PjsuaTransportManager.hpp/.cpp
|   |   |-- SignalingTransportPolicy.hpp
|   |   |-- MediaSecurityPolicy.hpp
|   |   `-- PjsuaDiagnostics.hpp/.cpp
|   `-- VoipService.cpp
|-- tests/
|   |-- unit/
|   |-- pjsua/
|   `-- integration/
`-- zephyr/
    |-- CMakeLists.txt
    `-- Kconfig
```

PJPROJECT port changes stay under `pjproject/`, primarily its Kconfig, Zephyr
source manifest, PJSUA configuration boundary, and fixed-arena initialization
hook.

## 25. Migration

1. Port and qualify PJSUA-LIB without changing the current service.
2. Implement the new PJ-independent service contract and scheduler using fakes.
3. Integrate five-account PJSUA registration.
4. Integrate seven signaling contexts and two-slot promotion.
5. Integrate the two custom media bridges.
6. Complete robustness, shutdown, pressure, and soak qualification.
7. Migrate one application to the breaking API.
8. Complete parity and real-network acceptance.
9. Remove the lowercase compatibility backend and its headless validation media
   only after replacement tests pass.
10. Keep the legacy uppercase `modules/VOIP` stack buildable through product
    acceptance.
11. Retire the legacy stack only in a separate approved change.

The implementation work is divided into six detailed plans:

1. PJSUA-LIB Zephyr port and bounded arena.
2. Core service API, handles, operations, event queue, and FIFO scheduler.
3. Five-account registration integration.
4. Seven-context call management and two-slot promotion.
5. Custom PJSUA audio/media bridge.
6. Robustness, migration, and product qualification.
