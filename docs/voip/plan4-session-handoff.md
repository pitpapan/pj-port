# Plan 4 Subagent-Driven Execution Handoff

**Prepared:** 2026-09-13

## Purpose

Continue from the completed Plan 3 PJSUA registration foundation and execute
Plan 4, **PJSUA Call Management and Two-Slot Promotion**, using the
subagent-driven-development workflow. Do not redesign the approved product
architecture during implementation. Resolve plan defects against the approved
spec, record the decision as a ledger ruling, and continue.

## Repository and integration state

The canonical workspace is `/home/pitpapan/zephyrproject`. Its Git metadata was
read-only during this handoff, so a writable local clone was reconstructed at:

```text
/tmp/voip-plan3-finisher
```

That clone has local `sip_rtp` at:

```text
108553b64 docs(voip): refresh plan4 call management steps
c348eac85 docs(voip): record plan3 acceptance
c6a9a97b9 test(voip): verify plan3 acceptance boundaries
a6e629d67 test(voip): harden capacity response drain
2232c9c73 test(voip): validate five pjsua registrations
```

The merge from the last known `sip_rtp` revision `45d56116e` through the Plan 3
feature history is a fast-forward. GitHub was unreachable because DNS could not
resolve `github.com`; therefore no claim is made that remote `sip_rtp` or
`origin/codex/pjsua-plan3` contains these final commits. `/tmp` may disappear
between sessions, so the revised Plan 4 and this handoff are also present in
the persistent Plan 3 worktree under `docs/`.

Before Plan 4 implementation, reconcile Git in this order:

1. Confirm `.git` is writable and network resolution works.
2. Fetch `origin/sip_rtp` and `origin/codex/pjsua-plan3`.
3. If `/tmp/voip-plan3-finisher` still exists, verify remote `sip_rtp` is an
   ancestor of its local `sip_rtp`. If it disappeared, recreate the two doc
   commits from the persistent files on top of `c6a9a97b9`.
4. If remote `sip_rtp` moved, merge its new commits into the local merged branch
   without force-pushing and rerun the host suite.
5. Push the completed `sip_rtp`, then update the canonical local `sip_rtp` with
   `git pull --ff-only`.
6. Create `codex/pjsua-plan4` from that exact `sip_rtp` and create an isolated
   worktree at `/home/pitpapan/zephyrproject/.worktrees/pjsua-plan4`.

Do not delete the Plan 3 worktree or `/tmp/voip-plan3-finisher` until all unique
documents are committed and pushed.

## Binding documents

Read these before dispatching Task 1:

1. `AGENTS.md`
2. `docs/superpowers/specs/2026-08-27-multi-agent-pjsua-voip-architecture-design.md`
3. `docs/superpowers/specs/state_machine.uml`
4. `docs/superpowers/plans/2026-08-27-pjsua-call-management-promotion.md`
5. `docs/voip/pjsua-five-account-registration.md`
6. `designIdead.md`
7. `reviews.md`

The architecture spec is the binding authority. The revised Plan 4 is its
implementation argument. `designIdead.md` and `reviews.md` explain the product
intent and the responsibility split that must not regress.

## Non-negotiable constraints

- Never inspect, search, index, or modify `zephyr/`. Treat Zephyr as an external
  platform and use documented `west` commands and compiler/build output.
- One to five agents are configured only at successful initialization. Each
  agent has one independent account and immutable audio bindings.
- Keep seven logical/native call contexts, two global promoted leases, at most
  one promoted call per agent, and one strict shared five-entry FIFO.
- Incoming and outgoing requests share FIFO. A blocked head is never bypassed.
- Two eligible FIFO entries may promote in one capacity pass when two leases
  are free; FIFO orders them but does not serialize them.
- Queued outgoing calls have no PJSUA ID and send no INVITE. Queued incoming
  calls retain their early PJSUA ID and receive 180 only after transactional
  logical/event admission.
- `CallScheduler` and `CallStateMachine` remain product-policy owners.
  `PjsuaCallManager` owns native call lifecycle only and stores no mutable
  `CallState`, `HoldReason`, FIFO policy, or application audio pointer.
- Only the actor mutates PJSUA/domain state. PJSUA/PJMEDIA worker counts stay
  zero and callbacks never call the application.
- Copy callback values synchronously. Never retain PJSIP/PJSUA callback-local
  pointers or string views.
- The queue timer begins at admission. The answer/establishment timer begins or
  restarts at actual promotion.
- Commit one guaranteed terminal event before invalidating the logical handle.
  Retain native context/user data and promoted lease until signaling, media,
  callback entry, and future security cleanup are quiescent.
- No C++ or general system-heap allocation after initialization. PJPROJECT
  allocations use only the fixed VoIP arena.
- Use SIP TCP and plain RTP/RTCP. TLS and SRTP remain disabled. Codec policy,
  custom device audio, and two media bridges remain Plan 5 scope.
- Do not use PJSUA2 or remove the legacy uppercase stack.
- RT1064 is a compile/link and memory-report gate in Plan 4, not hardware,
  network, audio, or runtime acceptance.

User authorization: removing Plan-owned `/tmp/*` files, `git add`, and
`git commit` do not require a permission question. Push, merge, or another
external/shared side effect still follows the active session’s safety policy.

## Known Plan 3 evidence

The stable Plan 3 source was independently reviewed. Evidence includes:

- two complete host-suite runs with all 11 markers;
- clean fake Plan 3 QEMU completion after fixing a 69,328-byte test frame that
  exceeded the 65,536-byte main stack;
- five real registration lifecycles with independent auth, 408 recovery,
  refresh, pending-retry shutdown, credential erasure, and arena wipe;
- zero general-heap allocations after initialization in the real registration
  profile;
- zero PJSUA/PJMEDIA workers and actor-owned event pumping;
- selected-service TLS/SRTP rejection before native side effects; and
- Plan 1 capacity result for five accounts, seven calls, and eighth-call 486.

Do not rerun Plan 3 implementation tasks. Run Plan 3 profiles only where the
Plan 4 task or final gate requires regressions.

## Risks the controller must actively track

1. **Fake-QEMU RAM:** the final Plan 3 fake profile used 98.34% of its configured
   QEMU RAM. Keep Plan 4 fake fixtures static/reused and stack frames small. Use
   a separate Plan 4 component profile. If it cannot fit, shard tests rather
   than enlarging product limits or hiding usage.
2. **Incoming transaction boundary:** current runtime has no explicit
   `AdmitIncoming` adapter handshake. Plan 4 must reserve incoming and terminal
   events plus a logical context before sending 180, then roll back invisibly
   if 180 fails.
3. **Timer epoch:** current core derives promoted timeout from admission time.
   Plan 4 must give a queued call a full answer/establishment window starting
   at promotion.
4. **Dial operation semantics:** current core may retain a queued Dial operation
   until network acceptance. The spec requires completion when durable local
   admission succeeds; later establishment/rejection is a call event.
5. **Ordered double promotion:** after head-of-line blocking releases with two
   free slots, the first and second eligible FIFO entries may both promote in
   the same actor turn, in FIFO order.
6. **Native lifetime:** never clear PJSUA user data, return a native context, or
   release a promoted lease merely because DISCONNECTED arrived. Media and
   callback quiescence are also required.
7. **RT1064 scope:** build and report memory only. Do not invent an Ethernet
   interface or claim target runtime qualification.

## Required subagent-driven workflow

Use `superpowers:subagent-driven-development` exactly:

1. Verify/create the Plan 4 worktree with `superpowers:using-git-worktrees`.
2. Resolve the SDD workspace:

   ```sh
   /home/pitpapan/.codex/plugins/cache/openai-curated-remote/superpowers/6.3.0/skills/subagent-driven-development/scripts/sdd-workspace \
     docs/superpowers/plans/2026-08-27-pjsua-call-management-promotion.md
   ```

3. Create `progress.md` whose first line identifies the exact plan path.
4. Build the required preflight conflict table: every task self-row and every
   pair sharing a file/interface. Record plan corrections as
   `Ruling: decision — reason — cost if wrong`.
5. Execute sequentially: Task 1, 2, 3, 4, 5, 6, 7A, 7B, 7C. There is no Task 8.
6. Before each task, record BASE and generate its brief with `scripts/task-brief`.
7. Dispatch one fresh implementer; never run multiple implementers in parallel.
   The implementer reads only its brief and named prerequisite interfaces,
   writes its full report in the SDD workspace, tests, commits, and self-reviews.
8. Use `gpt-5.6-terra` medium for the multi-file implementation tasks and normal
   task reviews. Escalate only when evidence shows the task needs more
   reasoning. Use the most capable available model for the whole-branch review.
9. Generate a BASE..HEAD review package and dispatch a separate task reviewer.
   Require both spec-compliance and code-quality approval.
10. Critical/Important findings enter the five-round fix loop. Rounds 1–3
    resume the implementer; rounds 4–5 use a fresh, more capable implementer.
    Every fix gets covering tests and scoped re-review. The controller never
    edits implementation code itself.
11. Record deferred Minor findings and every ruling in the ledger. Do not pause
    between clean tasks merely to ask whether to continue.
12. After 7C, package the full merge-base..HEAD diff and dispatch a broad final
    reviewer on the most capable model. Use one consolidated final fix wave and
    one scoped re-review if required.
13. Run `superpowers:verification-before-completion`, report every ledger ruling
    to the user, and use `superpowers:finishing-a-development-branch`.

## Task map

| Task | Deliverable | Primary review risk |
|---|---|---|
| 1 | Fakeable PJSUA call API seam and isolated Plan 4 component profile | Exact native signatures; bounded fake state |
| 2 | Seven native contexts and callback routing | No second business state; copied callback data |
| 3 | Transactional incoming admission and 180 handshake | Invisible admission and rollback correctness |
| 4 | Outgoing promotion and corrected Dial operation semantics | No INVITE while queued; ordered effects |
| 5 | Controls and promotion-relative timers | Exactly-once operations; timer epoch |
| 6 | Native state translation and quiescent teardown | Late callbacks, user-data lifetime, release order |
| 7A | Two real calls with scripted SIP peer | Real signaling and failure mapping |
| 7B | Two promoted plus five FIFO over five lifecycles | Head blocking, double promotion, heap/arena cleanup |
| 7C | Full regressions, RT1064 compile-only gate, acceptance record | Stable revision and no overclaiming |

## First execution action

After Git reconciliation and worktree creation, do not edit code immediately.
Run the SDD workspace script, create the ledger/preflight table, generate the
Task 1 brief, then dispatch the Task 1 implementer. Task 1 begins with a failing
component test for the new PJSUA call function-table entries.

## Definition of done

Plan 4 is complete only when every plan task has an independently reviewed
commit, both host runs and all fresh Plan 1–4 QEMU profiles pass, the exact
five-lifecycle multi-call marker is emitted, zero general-heap and bounded-arena
evidence pass, RT1064 compile/link and memory reports exist, final whole-branch
review has no unresolved load-bearing finding, and the acceptance record is
committed. Only then push/merge and remove the Plan 4 worktree.
