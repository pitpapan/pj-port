Short answer: yes, `voip` looks incomplete as a production replacement for `VOIP`. It is not empty or throwaway, though. It looks like a newer, phased PJPROJECT-based facade/SDK layer with tests and partial SIP/RTP/SRTP integration.

The older `VOIP` stack is the currently documented full stack: `voip_stack.md:1-4` says it is split into SIP signaling and RTP audio modules, and `CMakeLists.txt:19-20` includes both `VOIP/RTPAudio` and `VOIP/SIP`. It has concrete APIs like `SIPSessionManager.h:31-43` for registration/direct calls and `RTPAudioManager.hpp:16-27` for microphone/speaker RTP streaming.

By contrast, voip/ is gated behind `CONFIG_VOIP_FACADE`, default off, and its Kconfig text explicitly describes staged implementation. The help for `Kconfig:1-8` says Phase 1 is a deterministic fake backend. `CONFIG_VOIP_PJ_BACKEND` says account, call, and media behavior come from later integration phases: `Kconfig:52-60`. SRTP lifecycle text also says SIP SDP and facade media transport are “not secured until the subsequent SRTP call-path integration”: `Kconfig:89-98`.

Concrete signs that `voip` is still partial:

- It has a richer public service contract in `VoipService.hpp`, including handles, event delivery, `PcmSource`, and `PcmSink`, but the implementation is still effectively one account and one call through booleans in `VoipService.cpp:28-45`.
- `CallConfig` requires app audio source/sink, but `SipManager.cpp:78-92` only validates them and then calls `backend_.StartOutgoingCall(config.remote_uri)`. The source/sink are not wired into RTP audio.
- The PJ media backend is “headless”: it generates synthetic PCM and keeps sink stats in `PjHeadlessMedia.cpp:227-255`, instead of using board microphone/speaker devices like the old RTPAudio module.
- Many PJ backend operations are compile-time gated and return `invalid_state` when the phase/config is not enabled, for example registration, call control, and headless media in `PjVoipBackend.cpp:1256-1385`.
- The checked-in documentation `voip_stack.md` documents only `VOIP`, not the newer `voip` facade.

So my read is: `VOIP` is the older functional product stack; `voip` is a newer facade/SDK/PJPROJECT migration path that is partially implemented and validation-oriented. If your question is “can we remove `VOIP` and use `voip` instead today?”, I would say no, not without finishing device audio integration, multi-account/call behavior if needed, build integration, and production config coverage.

Yes, from a functional and architecture point of view, your suspicion is reasonable: the new `voip` is not at the same maturity level as `VOIP`, and it has several important pieces still left to design or split.

The main nuance is this: fewer classes in the new stack is not automatically bad, because PJPROJECT already provides many internal pieces that the old stack had to implement manually: SIP parser, message creator, SDP parser, transactions, dialogs, RTP/SRTP transport, timers, etc. So you would not expect a one-to-one replacement for `SIPMessageParser`, `SIPMessageCreator`, `SDP`, `RTPClient`, `RTPServer`, and so on.

But the new stack still needs clear SDK-level responsibilities, and right now a lot of them are concentrated in one place.

**Old Stack Shape**
The old stack has a clear, if imperfect, split of responsibilities:

- SIP session lifecycle: `SIPSession.h`
- Session creation/lifetime/thread ownership: `SIPSessionManager.h`
- App integration contract: `SIPSessionObserver.h`
- SIP message parsing/creation: `Message/*`
- SDP model: `SDP/*`
- RTP media ownership: `RTPAudioManager.hpp`
- RTP transmit path: `RTPAudioStreamer.hpp`
- RTP receive path: `RTPAudioReceiver`

That means the old stack has visible architecture boundaries: signaling, media, SDP, transport, observer bridge, and hardware audio are separate concepts.

**New Stack Shape**
The new `voip` has some structure:

- Public facade/API: `VoipFacade.hpp`
- SDK-style async service: `VoipService.hpp`
- Account/call handle manager: `SipManager.hpp`
- PJ runtime ownership: `PjRuntime.hpp`
- RTP wrapper: `RtpManager.hpp`
- PJ media implementation: `PjHeadlessMedia.cpp`
- PJ SIP/backend implementation: `PjVoipBackend.cpp`

But architecturally, `PjVoipBackend.cpp` is doing too much. Its `Impl` owns PJ runtime state, SIP endpoint setup, registration, retry timers, INVITE session handling, SDP generation/parsing glue, transport callbacks, media negotiation, call state, resource accounting, command queueing, event thread behavior, and validation hooks. That is a lot of unrelated responsibility in one class.

So yes: compared with the old stack, the new one is currently under-decomposed.

**Functional Gaps I See**
The biggest functional gaps are:

- Real audio integration is not done. `VoipService::Dial()` requires a `PcmSource` and `PcmSink`, but `SipManager.cpp:78-92` only validates them and then starts the backend call. Those objects are not actually connected to PJMEDIA.
- Current media is headless/test-oriented. `PjHeadlessMedia.cpp` generates synthetic PCM and stores sink stats. It does not use `Peripherals::Microphone`, `Peripherals::Speaker`, or the old codec layer.
- Multi-account/multi-call is not really implemented despite the config names. `VoipService.cpp:28-45` tracks `account_live` and `call_live` as single booleans.
- The new stack defaults to PCMU/PCMA in its local PJ SDP construction, while the old documented stack defaults to G.722 payload type `9`. That may be intentional, but it is a compatibility decision that needs to be explicit.
- SIP direct-call modes from the old stack are not clearly represented. The old stack has registrable, incoming direct, and outgoing direct session classes. The new stack appears centered around one PJ backend path.
- The application-facing event model exists, but it is thinner than the old observer-plus-SDP contract. The old app can inspect local/remote SDP and decide how to start RTP. The new app mostly receives copied `CallInfo`, which may not expose enough negotiation detail yet.
- Hold/re-INVITE exists in the PJ backend, but because real audio source/sink integration is missing, it is not equivalent to the old `RTPAudioManager::HoldAudioStreamer()` / `HoldAudioReceiver()` behavior.
- Build/documentation integration is incomplete. `voip_stack.md` documents `VOIP`, while `Kconfig` describes phased PJPROJECT work.

**Architecture Pieces Still Left To Split**
If the new `voip` is meant to become production architecture, I would expect these separate components to appear or become clearer:

- `PjEndpointRuntime`: PJLIB/PJSIP/PJMEDIA endpoint lifetime, pools, thread registration, logging.
- `PjAccountManager`: account config, registration, authentication, reconnect/refresh policy.
- `PjCallManager`: INVITE sessions, incoming/outgoing calls, call state, accept/reject/hangup.
- `PjSdpAdapter`: convert PJ SDP into SDK `CallInfo`/codec/media direction/ports.
- `PjMediaSession`: one call’s RTP/SRTP media state.
- `PjAudioBridge`: adapter between PJMEDIA and SDK `PcmSource`/`PcmSink`, or existing `Microphone`/`Speaker`.
- `VoipEventDispatcher`: command/event queue and operation-id mapping.
- `VoipResourceMonitor`: memory, stack, socket, timer counters if those are production features.

Right now several of those live inside `PjVoipBackend::Impl`, which makes it harder to reason about correctness.

**My Judgment**
I would not consider the new `voip` complete yet. It looks like a serious prototype or staged migration layer, not a finished replacement for the old stack.

I would be especially cautious about saying it “handles correctly” until these are proven:

1. Real microphone/speaker audio path works through PJMEDIA or a deliberate RTPAudio bridge.
2. Incoming and outgoing calls work against the target SIP server.
3. Registration, reconnect, failed auth, timeout, BYE, reject, hold/resume, and re-INVITE are covered by tests.
4. Codec negotiation matches product requirements, especially if G.722 or Opus are needed.
5. The architecture is split enough that SIP, media, SDP mapping, runtime lifetime, and app events can be tested separately.



