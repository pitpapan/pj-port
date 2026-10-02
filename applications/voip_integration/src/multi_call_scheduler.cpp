#include "../test_support/ScriptedSipPeer.hpp"
#include <voip/VoipService.hpp>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <pj_zephyr_pool_arena.h>
#include <pjsua-lib/pjsua.h>
#include "../../../voip/src/pjsua/PjsuaCallbackRouter.hpp"
#include "../../../voip/src/core/AgentRegistry.hpp"
#include "../../../voip/src/core/CoreActor.hpp"
#include <zephyr/sys/atomic.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

namespace {
constexpr voip::PcmFormat format{8000, 80, 1, voip::SampleFormat::signed_16};
int failures;
atomic_t credential_resets{}, credential_failures{};
pj_zephyr_pool_arena_stats arena_before_reset{};
#define CHECK(x) do { if (!(x)) { printk("[multi-call] FAIL %d: %s\n", __LINE__, #x); ++failures; } } while (false)
extern "C" void voip_general_heap_tracking_begin() noexcept;
extern "C" std::size_t voip_general_heap_tracking_end() noexcept;
class Source final : public voip::PcmSource { public:
    voip::PcmFormat Format() const noexcept override { return format; }
    voip::Error Read(std::int16_t *samples, std::size_t count, std::uint64_t) noexcept override {
        std::memset(samples, 0, count * sizeof(*samples)); return voip::Error::ok;
    }
};
class Sink final : public voip::PcmSink { public:
    voip::PcmFormat Format() const noexcept override { return format; }
    voip::Error Write(const std::int16_t *, std::size_t, std::uint64_t) noexcept override { return voip::Error::ok; }
    void Flush() noexcept override {}
};
bool Same(voip::CallHandle a, voip::CallHandle b) { return a.slot == b.slot && a.generation == b.generation; }
struct CallEvidence {
    voip::CallHandle handle{}; voip::AgentHandle agent{}; char remote[256]{};
    voip::OperationId operation{};
    unsigned established{}, terminals{}, timeouts{}; voip::Error error{}; unsigned sip{};
    voip::CallState states[12]{}; voip::CallTransition transitions[12]{}; unsigned trace_count{};
};
struct Seen {
    CallEvidence calls[16]{}; unsigned count{}, incoming{}, accepted{}, finished{};
    struct OperationEvidence {
        voip::OperationId id{}; voip::Error expected{}; voip::Error actual{}; unsigned completions{};
        unsigned sip{};
    } operations[64]{};
    unsigned operation_count{}, observed_operations{}, full_capacity_proofs{};
    bool registered[5]{}; voip::CallHandle last_incoming{};
};
void Submitted(Seen &seen, voip::Error submission, voip::OperationId id,
               voip::Error expected) {
    CHECK(submission == voip::Error::ok);
    if (submission != voip::Error::ok) return;
    CHECK(id != 0 && seen.operation_count < 64);
    for (unsigned i = 0; i < seen.operation_count; ++i) CHECK(seen.operations[i].id != id);
    if (id != 0 && seen.operation_count < 64)
        seen.operations[seen.operation_count++] = {id, expected, {}, 0};
}
void Record(const voip::Event &event, Seen &seen) {
    if (event.type == voip::EventType::agent_snapshot && event.agent.slot < 5)
        seen.registered[event.agent.slot] = event.agent_snapshot.registration == voip::RegistrationState::registered;
    if (event.type == voip::EventType::incoming_call) { ++seen.incoming; seen.last_incoming = event.call; }
    if (event.type == voip::EventType::operation_terminal) {
        for (unsigned i = 0; i < seen.operation_count; ++i) {
            auto &operation = seen.operations[i];
            if (operation.id != event.operation) continue;
            CHECK(operation.completions == 0);
            CHECK(event.status.error == operation.expected);
            operation.actual = event.status.error;
            operation.sip = event.status.sip_status;
            ++operation.completions;
            ++seen.observed_operations;
            return;
        }
        CHECK(false); // an operation terminal event had no submitted public command
        return;
    }
    if (event.type != voip::EventType::call_state) return;
    CHECK(event.call.IsValid()); CHECK(Same(event.call, event.call_snapshot.handle));
    CallEvidence *call = nullptr;
    for (unsigned i = 0; i < seen.count; ++i) if (Same(seen.calls[i].handle, event.call)) call = &seen.calls[i];
    if (!call) {
        CHECK(seen.count < 16); if (seen.count == 16) return;
        call = &seen.calls[seen.count++]; call->handle = event.call; call->operation = event.operation;
        std::snprintf(call->remote, sizeof(call->remote), "%s", event.call_snapshot.remote_uri);
    }
    CHECK(std::strcmp(call->remote, event.call_snapshot.remote_uri) == 0);
    if (call->trace_count < 12) {
        if (call->trace_count != 0)
            CHECK(event.source_state == call->states[call->trace_count - 1]);
        call->states[call->trace_count] = event.destination_state;
        call->transitions[call->trace_count++] = event.transition;
    } else CHECK(false);
    if (event.destination_state == voip::CallState::established) {
        call->agent = event.call_snapshot.agent;
        if (call->established++ == 0) ++seen.accepted;
    }
    if (event.transition == voip::CallTransition::timeout) {
        ++call->timeouts;
    }
    if (event.destination_state == voip::CallState::terminated) {
        ++call->terminals; ++seen.finished; call->error = event.status.error; call->sip = event.status.sip_status;
        printk("[multi-call] terminal call=%u/%u remote=%s error=%u sip=%u reason=%s\n",
               event.call.slot, event.call.generation, call->remote, static_cast<unsigned>(call->error), call->sip, event.status.reason);
    }
}
bool OperationDone(const Seen &seen, voip::OperationId id, voip::Error error) {
    for (unsigned i = 0; i < seen.operation_count; ++i)
        if (seen.operations[i].id == id)
            return seen.operations[i].completions == 1 && seen.operations[i].actual == error;
    return false;
}
void Poll(voip::VoipService &service, Seen &seen);
bool WaitOperation(voip::VoipService &service, Seen &seen, voip::OperationId id,
                   voip::Error error) {
    const int64_t deadline = k_uptime_get() + 3000;
    while (k_uptime_get() < deadline) {
        if (OperationDone(seen, id, error)) return true;
        Poll(service, seen);
    }
    return OperationDone(seen, id, error);
}
void Poll(voip::VoipService &service, Seen &seen) {
    voip::Event event{}; if (service.WaitForEvent(&event, 5) == voip::Error::ok) Record(event, seen);
    k_sleep(K_MSEC(5));
}
bool Wait(voip::VoipService &service, Seen &seen, const unsigned &value, unsigned want) {
    const int64_t deadline = k_uptime_get() + 3000;
    while (k_uptime_get() < deadline && value < want) Poll(service, seen);
    return value >= want;
}
bool Registered(const Seen &seen) { for (bool value : seen.registered) if (!value) return false; return true; }
CallEvidence *Find(Seen &seen, const char *remote) {
    for (unsigned i = 0; i < seen.count; ++i) if (std::strcmp(seen.calls[i].remote, remote) == 0) return &seen.calls[i];
    return nullptr;
}
bool WaitState(voip::VoipService &service, Seen &seen, voip::CallHandle call,
               voip::CallState desired, unsigned milliseconds = 3000) {
    const int64_t deadline = k_uptime_get() + milliseconds;
    while (k_uptime_get() < deadline) {
        voip::CallSnapshot snapshot{};
        if (service.GetCallSnapshot(call, &snapshot) == voip::Error::ok &&
            snapshot.state == desired) return true;
        Poll(service, seen);
    }
    return false;
}
bool WaitTransition(voip::VoipService &service, Seen &seen, voip::CallHandle call,
                    voip::CallTransition desired, unsigned milliseconds = 3000) {
    const int64_t deadline = k_uptime_get() + milliseconds;
    while (k_uptime_get() < deadline) {
        for (unsigned i = 0; i < seen.count; ++i) if (Same(seen.calls[i].handle, call))
            for (unsigned j = 0; j < seen.calls[i].trace_count; ++j)
                if (seen.calls[i].transitions[j] == desired) return true;
        Poll(service, seen);
    }
    return false;
}
bool WaitResources(voip::VoipService &service, Seen &seen, unsigned promoted,
                   unsigned queued, unsigned milliseconds = 3000) {
    const int64_t deadline = k_uptime_get() + milliseconds;
    while (k_uptime_get() < deadline) {
        const auto resources = service.GetResourceSnapshot();
        if (resources.promoted_calls == promoted && resources.queued_calls == queued) return true;
        Poll(service, seen);
    }
    return false;
}
voip::CallHandle Dial(voip::VoipService &service, Seen &seen, voip::AgentHandle agent,
                      const char *uri) {
    const unsigned first_new_call = seen.count;
    voip::OperationId operation{};
    const auto result = service.Dial(agent, {uri}, &operation);
    Submitted(seen, result, operation, voip::Error::ok);
    const int64_t deadline = k_uptime_get() + 3000;
    while (k_uptime_get() < deadline) {
        for (unsigned i = first_new_call; i < seen.count; ++i)
            if (std::strcmp(seen.calls[i].remote, uri) == 0)
                return seen.calls[i].handle;
        Poll(service, seen);
    }
    CHECK(false);
    return {};
}
voip::CallHandle Incoming(voip::VoipService &service, Seen &seen,
                          voip_integration::ScriptedSipPeer &peer,
                          voip_integration::SipPeerScript script, unsigned agent) {
    const unsigned want = seen.incoming + 1;
    CHECK(peer.StartIncoming(script, agent));
    CHECK(Wait(service, seen, seen.incoming, want));
    return seen.last_incoming;
}
void EndCall(voip::VoipService &service, Seen &seen, voip::CallHandle call) {
    const unsigned want = seen.finished + 1;
    voip::OperationId operation{};
    const auto result = service.Hangup(call, &operation);
    Submitted(seen, result, operation, voip::Error::ok);
    CHECK(Wait(service, seen, seen.finished, want));
    CHECK(WaitOperation(service, seen, operation, voip::Error::ok));
}
void CheckPool(voip::VoipService &service, unsigned promoted, unsigned queued) {
    const auto pool = service.GetResourceSnapshot();
    CHECK(pool.promoted_calls == promoted); CHECK(pool.queued_calls == queued);
    CHECK(pool.active_calls == promoted + queued);
    CHECK(pool.available_promoted_calls == 2 - promoted);
    CHECK(pool.available_fifo_entries == 5 - queued);
    CHECK(pool.available_logical_calls == 7 - promoted - queued);
}
void ExpectTrace(const CallEvidence &call,
                 std::initializer_list<voip::CallTransition> transitions,
                 std::initializer_list<voip::CallState> states) {
    CHECK(call.trace_count == transitions.size());
    CHECK(call.trace_count == states.size());
    unsigned i = 0;
    for (auto transition : transitions) CHECK(call.transitions[i++] == transition);
    i = 0;
    for (auto state : states) CHECK(call.states[i++] == state);
}
void RunFullCapacityCycle(voip::VoipService &service, Seen &seen,
                          voip_integration::ScriptedSipPeer &peer,
                          const voip::AgentHandle *agents, unsigned lifecycle) {
    using T = voip::CallTransition; using S = voip::CallState;
    using voip_integration::SipPeerScript;
    const int failures_before = failures;
    const unsigned first_call = seen.count;
    const unsigned accepted_before = seen.accepted;
    const unsigned finished_before = seen.finished;
    const auto flows_before = peer.Stats();
    const char *a_uri = "sip:promoted-a@127.0.0.1:5098;transport=tcp";
    peer.SetNextOutgoing(SipPeerScript::answer);
    const auto a = Dial(service, seen, agents[0], a_uri);
    CHECK(Wait(service, seen, seen.accepted, accepted_before + 1));
    const unsigned b_sequence = peer.Stats().incoming_invites + 1;
    const auto b = Incoming(service, seen, peer, SipPeerScript::answer, 1);
    voip::OperationId operation{};
    auto submission = service.Answer(b, &operation);
    Submitted(seen, submission, operation, voip::Error::ok);
    CHECK(Wait(service, seen, seen.accepted, accepted_before + 2));
    CheckPool(service, 2, 0);
    CHECK(!Same(a, b));
    CHECK(seen.calls[first_call].agent.slot != seen.calls[first_call + 1].agent.slot);
    char b_remote[64]{};
    std::snprintf(b_remote, sizeof(b_remote), "<sip:remote-in-%u@peer.test>", b_sequence);
    CHECK(Same(seen.calls[first_call].handle, a) &&
          std::strcmp(seen.calls[first_call].remote, a_uri) == 0);
    CHECK(Same(seen.calls[first_call + 1].handle, b) &&
          std::strcmp(seen.calls[first_call + 1].remote, b_remote) == 0);
    const int64_t handshake_deadline = k_uptime_get() + 1000;
    while (peer.Stats().reinvite_acks < flows_before.reinvite_acks + 1 && k_uptime_get() < handshake_deadline)
        Poll(service, seen);
    const auto flows = peer.Stats();
    CHECK(flows.invites == flows_before.invites + 1);
    CHECK(flows.trying == flows_before.trying + 1 && flows.ringing == flows_before.ringing + 1);
    CHECK(flows.incoming_trying >= flows_before.incoming_trying + 1);
    CHECK(flows.incoming_ringing >= flows_before.incoming_ringing + 1);
    // Task 7A's generated re-INVITE/200/ACK remains proved in this first
    // lifecycle and in the separate lifecycle-2/3 smoke setup. A prior held
    // dialog can legitimately answer its overlapping re-INVITE with 491.
    if (lifecycle == 1) {
        CHECK(flows.reinvites == flows_before.reinvites + 1);
        CHECK(flows.reinvite_responses == flows_before.reinvite_responses + 1);
        CHECK(flows.reinvite_acks == flows_before.reinvite_acks + 1 && flows.acks >= flows_before.acks + 3);
    }

    const unsigned queued_a_sequence = peer.Stats().incoming_invites + 1;
    const auto queued_a = Incoming(service, seen, peer, SipPeerScript::shared_stream, 0);
    submission = service.Answer(queued_a, &operation);
    Submitted(seen, submission, operation, voip::Error::ok);
    const unsigned invites_before = peer.Stats().invites;
    char c_uri[96]{};
    CHECK(peer.IncomingPort(queued_a_sequence) != 0);
    // C uses A's peer endpoint, yielding an actual order on one TCP stream.
    std::snprintf(c_uri, sizeof(c_uri), "sip:queued-c@127.0.0.1:%u;transport=tcp", peer.IncomingPort(queued_a_sequence));
    const auto c = Dial(service, seen, agents[2], c_uri);
    const auto d = Incoming(service, seen, peer, SipPeerScript::answer, 3);
    const auto e = Dial(service, seen, agents[4], "sip:queued-e@127.0.0.1:5098;transport=tcp");
    const auto queued_b = Incoming(service, seen, peer, SipPeerScript::answer, 1);
    CHECK(WaitResources(service, seen, 2, 5)); CheckPool(service, 2, 5);
    CHECK(pjsua_call_get_count() == 5); // C/E are native-free while queued
    CHECK(peer.Stats().invites == invites_before);
    for (unsigned i = 0; i < 3; ++i) {
        const int64_t ringing_deadline = k_uptime_get() + 1000;
        while (peer.IncomingResponseOrder(queued_a_sequence + i, 180) == 0 &&
               k_uptime_get() < ringing_deadline) Poll(service, seen);
        CHECK(peer.IncomingResponseOrder(queued_a_sequence + i, 180) != 0);
    }
    EndCall(service, seen, b);
    CHECK(WaitResources(service, seen, 1, 5));
    CHECK(peer.Stats().invites == invites_before);
    EndCall(service, seen, a);
    CHECK(WaitState(service, seen, queued_a, voip::CallState::established));
    CHECK(WaitState(service, seen, c, voip::CallState::established));
    CHECK(WaitResources(service, seen, 2, 3));
    const unsigned answer_order = peer.IncomingResponseOrder(queued_a_sequence, 200);
    const unsigned invite_order = peer.OutgoingInviteOrder(invites_before + 1);
    CHECK(answer_order != 0 && invite_order != 0 && answer_order < invite_order);
    printk("[multi-call] lifecycle=%u FIFO head action=%u C INVITE=%u\n", lifecycle, answer_order, invite_order);

    submission = service.Reject(d, 486, &operation);
    Submitted(seen, submission, operation, voip::Error::ok);
    submission = service.Cancel(e, &operation);
    Submitted(seen, submission, operation, voip::Error::ok);
    submission = service.Reject(queued_b, 486, &operation);
    Submitted(seen, submission, operation, voip::Error::ok);
    CHECK(WaitResources(service, seen, 2, 0));
    EndCall(service, seen, queued_a);
    EndCall(service, seen, c);
    CHECK(WaitResources(service, seen, 0, 0));
    CHECK(Wait(service, seen, seen.finished, finished_before + 7));
    const int64_t native_deadline = k_uptime_get() + 3000;
    while (pjsua_call_get_count() != 0 && k_uptime_get() < native_deadline)
        Poll(service, seen);
    CHECK(pjsua_call_get_count() == 0);
    CHECK(seen.count == first_call + 7);
    for (unsigned i = 0; i < 2; ++i)
        ExpectTrace(seen.calls[first_call + i], {T::initiation, T::acceptance, T::finish},
                    {S::initiated, S::established, S::terminated});
    for (unsigned i = 2; i < 4; ++i)
        ExpectTrace(seen.calls[first_call + i], {T::wait, T::acceptance, T::finish},
                    {S::hold, S::established, S::terminated});
    for (unsigned i = 4; i < 7; ++i)
        ExpectTrace(seen.calls[first_call + i], {T::wait, T::rejection},
                    {S::hold, S::terminated});
    if (failures == failures_before) ++seen.full_capacity_proofs;
}
void CheckTraces(const Seen &seen, unsigned lifecycle) {
    using T = voip::CallTransition; using S = voip::CallState;
    const unsigned base_counts[] = {0, 7, 6, 5, 6};
    CHECK(seen.count == base_counts[lifecycle - 1] + 7);
    for (unsigned i = 0; i < base_counts[lifecycle - 1]; ++i) {
        const auto &call = seen.calls[i];
        if (lifecycle <= 3 && i < 2)
            ExpectTrace(call, {T::initiation, T::acceptance, T::finish}, {S::initiated, S::established, S::terminated});
        else if (lifecycle == 2)
            ExpectTrace(call, {T::wait, T::timeout}, {S::hold, S::terminated});
        else if (lifecycle == 3 && i == 2)
            ExpectTrace(call, {T::wait, T::acceptance, T::hold, T::resume, T::finish},
                        {S::hold, S::established, S::hold, S::established, S::terminated});
        else if (lifecycle <= 3)
            ExpectTrace(call, {T::wait, T::rejection}, {S::hold, S::terminated});
        else if (lifecycle == 4 && i<2)
            ExpectTrace(call, {T::initiation, T::timeout}, {S::initiated, S::idle});
        else if (lifecycle == 4)
            ExpectTrace(call, {T::initiation, T::acceptance, T::finish}, {S::initiated, S::established, S::terminated});
        else if (i < 3)
            ExpectTrace(call, {T::initiation, T::rejection}, {S::initiated, S::terminated});
        else
            ExpectTrace(call, {T::initiation, T::acceptance, T::finish}, {S::initiated, S::established, S::terminated});
    }
}
void Trace(const Seen &seen) {
    for (unsigned i = 0; i < seen.count; ++i) {
        const auto &call = seen.calls[i];
        printk("[multi-call] trace %u/%u %s:", call.handle.slot, call.handle.generation, call.remote);
        for (unsigned j = 0; j < call.trace_count; ++j)
            printk(" %u/%u", static_cast<unsigned>(call.transitions[j]), static_cast<unsigned>(call.states[j]));
        printk("\n");
    }
}
void AuditShutdown(voip::VoipService &service, Seen &seen, const voip::AgentHandle *agents,
                   const voip::CallHandle *old_calls, unsigned old_count, unsigned lifecycle) {
    pj_zephyr_pool_arena_stats before_shutdown{};
    pj_zephyr_pool_arena_get_stats(&before_shutdown);
    CHECK(before_shutdown.peak_bytes != 0);
    if (lifecycle == 5)
        CHECK(before_shutdown.peak_bytes * 10 < before_shutdown.capacity_bytes * 9);
    else
        CHECK(before_shutdown.peak_bytes * 4 <= before_shutdown.capacity_bytes * 3);
    printk("[multi-call] lifecycle=%u arena pre-shutdown peak=%u capacity=%u used=%u live=%u failed=%u\n",
           lifecycle, static_cast<unsigned>(before_shutdown.peak_bytes),
           static_cast<unsigned>(before_shutdown.capacity_bytes), static_cast<unsigned>(before_shutdown.used_bytes),
           before_shutdown.live_blocks, before_shutdown.failed_allocations);
    const auto resets_before = atomic_get(&credential_resets);
    const auto shutdown = service.Shutdown();
    CHECK(shutdown == voip::Error::ok);
    CHECK(service.Shutdown() == voip::Error::ok);
    voip::Event event{};
    unsigned drained_events = 0;
    while (service.TryGetEvent(&event) == voip::Error::ok) { Record(event, seen); ++drained_events; }
    CHECK(seen.observed_operations == seen.operation_count);
    for (unsigned i = 0; i < seen.operation_count; ++i)
        CHECK(seen.operations[i].completions == 1 &&
              seen.operations[i].actual == seen.operations[i].expected);
    CHECK(seen.full_capacity_proofs == 1);
    const auto resources = service.GetResourceSnapshot();
    CHECK(resources.active_agents == 0 && resources.active_calls == 0);
    CHECK(resources.promoted_calls == 0 && resources.queued_calls == 0);
    CHECK(resources.available_operations == 16 && resources.available_fifo_entries == 5);
    CHECK(resources.available_commands == 16);
    // The stopped actor's snapshot predates this consumer's final drain.
    // Account for those records to prove no event reservation remains.
    CHECK(resources.available_events + drained_events == 31); // one slot is reserved for stopped
    CHECK(resources.available_logical_calls == 7 && resources.available_promoted_calls == 2);
    CHECK(resources.available_media_bridges == 2);
    CHECK(pjsua_call_get_count() == 0 && pjsua_acc_get_count() == 0);
    CHECK(pjsua_get_state() == PJSUA_STATE_NULL);
    CHECK(voip::PjsuaCallbackRouter::ActiveForTest() == nullptr);
    CHECK(atomic_get(&credential_resets) > resets_before);
    CHECK(atomic_get(&credential_failures) == 0);
    for (unsigned i = 0; i < 5; ++i) {
        voip::AgentSnapshot snapshot{};
        CHECK(service.GetAgentSnapshot(agents[i], &snapshot) == voip::Error::invalid_handle);
    }
    for (unsigned i = 0; i < old_count; ++i) {
        voip::CallSnapshot snapshot{};
        CHECK(service.GetCallSnapshot(old_calls[i], &snapshot) == voip::Error::invalid_handle);
    }
    pj_zephyr_pool_arena_stats arena{};
    pj_zephyr_pool_arena_get_stats(&arena);
    CHECK(arena.used_bytes == 0 && arena.live_blocks == 0 && arena.reset_in_progress == 0);
    CHECK(arena.failed_allocations == 0);
    CHECK(arena_before_reset.peak_bytes >= before_shutdown.peak_bytes);
    CHECK(arena_before_reset.failed_allocations == 0);
    if (lifecycle == 5)
        CHECK(arena_before_reset.peak_bytes * 10 < arena_before_reset.capacity_bytes * 9);
    else
        CHECK(arena_before_reset.peak_bytes * 4 <= arena_before_reset.capacity_bytes * 3);
    printk("[multi-call] lifecycle=%u lifetime arena peak=%u capacity=%u\n", lifecycle,
           static_cast<unsigned>(arena_before_reset.peak_bytes), static_cast<unsigned>(arena_before_reset.capacity_bytes));
    CHECK(arena.last_reset_wiped_bytes > 0 && arena.last_reset_wiped_bytes <= arena.capacity_bytes);
    CHECK(arena.reset_wipe_count == lifecycle);
    printk("[multi-call] lifecycle=%u arena peak=%u capacity=%u used=%u live=%u failed=%u resets=%u\n",
           lifecycle, static_cast<unsigned>(arena.peak_bytes), static_cast<unsigned>(arena.capacity_bytes),
           static_cast<unsigned>(arena.used_bytes), arena.live_blocks, arena.failed_allocations, arena.reset_wipe_count);
    const std::size_t heap_allocations = voip_general_heap_tracking_end();
    CHECK(heap_allocations == 0);
    printk("[multi-call] lifecycle=%u general_heap_allocations=%u\n", lifecycle, static_cast<unsigned>(heap_allocations));
    Trace(seen);
    CheckTraces(seen, lifecycle);
}
}
extern "C" void __real__ZN4voip13AgentRegistry5ResetEv(voip::AgentRegistry *);
extern "C" void __wrap__ZN4voip13AgentRegistry5ResetEv(voip::AgentRegistry *registry) {
    __real__ZN4voip13AgentRegistry5ResetEv(registry);
    if (!registry->CredentialsErasedForTest()) atomic_inc(&credential_failures);
    atomic_inc(&credential_resets);
}
extern "C" pj_status_t __real_pj_zephyr_pool_arena_reset();
extern "C" pj_status_t __wrap_pj_zephyr_pool_arena_reset() {
    pj_zephyr_pool_arena_get_stats(&arena_before_reset);
    return __real_pj_zephyr_pool_arena_reset();
}
int main() {
    using voip_integration::ScriptedSipPeer; using voip_integration::SipPeerScript;
    auto &peer = ScriptedSipPeer::Instance();
    CHECK(peer.FramingChecks()); CHECK(peer.Start());
    voip_general_heap_tracking_begin();
    void *(*volatile allocate)(std::size_t) = std::malloc;
    void *probe = allocate(8);
    CHECK(probe != nullptr); std::free(probe);
    CHECK(voip_general_heap_tracking_end() != 0);
    Source sources[5]; Sink sinks[5]; char identities[5][64]{}; voip::AgentConfig config[5]{};
    for (unsigned i = 0; i < 5; ++i) {
        std::snprintf(identities[i], sizeof(identities[i]), "<sip:agent%u@127.0.0.1>", i);
        config[i] = {{identities[i], "sip:127.0.0.1:5098;transport=tcp", "agent", "password"}, {&sources[i], &sinks[i]}, true};
    }
    static voip::VoipService service;
    voip::CallHandle previous_calls[16]{}; unsigned previous_count = 0;
    voip::AgentHandle previous_agents[5]{};
    for (unsigned lifecycle = 1; lifecycle <= 5; ++lifecycle) {
        const unsigned queue_timeout = lifecycle == 2 ? 3000 : 5000;
        const unsigned answer_timeout = 3000;
        CHECK(service.Initialize({config, 5, queue_timeout, answer_timeout, format,
              {voip::SignalingSecurity::none, voip::MediaSecurity::none}}) == voip::Error::ok);
        voip_general_heap_tracking_begin();
        for (unsigned i = 0; i < previous_count; ++i) {
            voip::CallSnapshot stale{};
            CHECK(service.GetCallSnapshot(previous_calls[i], &stale) == voip::Error::invalid_handle);
        }
        if (lifecycle > 1) for (auto agent : previous_agents) {
            voip::AgentSnapshot stale{};
            CHECK(service.GetAgentSnapshot(agent, &stale) == voip::Error::invalid_handle);
        }
        Seen seen{}; voip::AgentHandle agents[5]{};
        for (unsigned i = 0; i < 5; ++i)
            CHECK(service.GetAgentHandle(i, &agents[i]) == voip::Error::ok);
        const int64_t registration_deadline = k_uptime_get() + 3000;
        while (k_uptime_get() < registration_deadline && !Registered(seen)) Poll(service, seen);
        CHECK(Registered(seen)); CHECK(peer.ZeroWorkerConfiguration());
        CHECK(peer.Stats().registrations >= lifecycle * 5);

        if (lifecycle == 2 || lifecycle == 3) {
            const auto flows_before = peer.Stats();
            const char *a_uri = "sip:promoted-a@127.0.0.1:5098;transport=tcp";
            peer.SetNextOutgoing(SipPeerScript::answer);
            const voip::CallHandle a = Dial(service, seen, agents[0], a_uri);
            CHECK(Wait(service, seen, seen.accepted, 1));
            const unsigned incoming_b_sequence = peer.Stats().incoming_invites + 1;
            const voip::CallHandle b = Incoming(service, seen, peer,
                lifecycle == 3 ? SipPeerScript::offerless : SipPeerScript::answer, 1);
            voip::OperationId operation{};
            voip::Error submission = service.Answer(b, &operation);
            Submitted(seen, submission, operation, voip::Error::ok);
            CHECK(Wait(service, seen, seen.accepted, 2));
            CheckPool(service, 2, 0);
            CHECK(!Same(a, b));
            CHECK(seen.calls[0].agent.slot != seen.calls[1].agent.slot);
            char b_remote[64]{};
            std::snprintf(b_remote, sizeof(b_remote), "<sip:remote-in-%u@peer.test>", incoming_b_sequence);
            CHECK(Find(seen, a_uri) != nullptr && Find(seen, b_remote) != nullptr);
            const int64_t handshake_deadline = k_uptime_get() + 1000;
            while (peer.Stats().reinvite_acks < flows_before.reinvite_acks + 1 && k_uptime_get() < handshake_deadline)
                Poll(service, seen);
            const auto flows = peer.Stats();
            CHECK(flows.invites == flows_before.invites + 1);
            CHECK(flows.trying == flows_before.trying + 1 && flows.ringing == flows_before.ringing + 1);
            CHECK(flows.incoming_trying >= flows_before.incoming_trying + 1);
            CHECK(flows.incoming_ringing >= flows_before.incoming_ringing + 1);
            CHECK(flows.reinvites == flows_before.reinvites + 1);
            CHECK(flows.reinvite_responses == flows_before.reinvite_responses + 1);
            CHECK(flows.reinvite_acks == flows_before.reinvite_acks + 1 && flows.acks >= flows_before.acks + 3);
            printk("[multi-call] lifecycle=%u INVITE/100/180/200/ACK and re-INVITE/200/ACK checked\n", lifecycle);
            if (lifecycle == 2) {
                voip::CallHandle queued[5]{};
                const unsigned first_queued_sequence = peer.Stats().incoming_invites + 1;
                const unsigned media_before_queue = peer.MediaTransportCreations();
                for (unsigned i = 0; i < 5; ++i) {
                    queued[i] = Incoming(service, seen, peer,
                        i == 4 ? SipPeerScript::offerless : SipPeerScript::answer, i);
                    // Offered INVITEs and offerless 180 admission must both
                    // remain signaling-only while the two leases are occupied.
                    CHECK(peer.MediaTransportCreations() == media_before_queue);
                }
                CHECK(WaitResources(service, seen, 2, 5)); CheckPool(service, 2, 5);
                CHECK(pjsua_call_get_count() == 7);
                for (unsigned i = 0; i < 5; ++i) {
                    const int64_t ringing_deadline = k_uptime_get() + 1000;
                    while (peer.IncomingResponseOrder(first_queued_sequence + i, 180) == 0 &&
                           k_uptime_get() < ringing_deadline) Poll(service, seen);
                    CHECK(peer.IncomingResponseOrder(first_queued_sequence + i, 180) != 0);
                }
                const unsigned eighth_sequence = peer.Stats().incoming_invites + 1;
                CHECK(peer.StartIncoming(SipPeerScript::answer, 2));
                const int64_t busy_deadline = k_uptime_get() + 2000;
                while (k_uptime_get() < busy_deadline && peer.IncomingResponseOrder(eighth_sequence, 486) == 0)
                    Poll(service, seen);
                CHECK(peer.IncomingResponseOrder(eighth_sequence, 486) != 0);
                CheckPool(service, 2, 5);
                CHECK(pjsua_call_get_count() == 7);
                const unsigned invites_before = peer.Stats().invites;
                submission = service.Dial(agents[3], {"sip:overflow@127.0.0.1:5098;transport=tcp"}, &operation);
                Submitted(seen, submission, operation, voip::Error::queue_full);
                CHECK(WaitOperation(service, seen, operation, voip::Error::queue_full));
                CHECK(peer.Stats().invites == invites_before);
                CHECK(pjsua_call_get_count() == 7);
                CHECK(Wait(service, seen, seen.finished, 5));
                CHECK(peer.IncomingResponseOrder(first_queued_sequence, 480) != 0);
                voip::CallSnapshot stale{};
                CHECK(service.GetCallSnapshot(queued[0], &stale) == voip::Error::invalid_handle);
                EndCall(service, seen, b);
                EndCall(service, seen, a);
                CHECK(WaitResources(service, seen, 0, 0));
            } else {
                const voip::CallHandle queued_a = Incoming(service, seen, peer, SipPeerScript::answer, 0);
                submission = service.Answer(queued_a, &operation);
                Submitted(seen, submission, operation, voip::Error::ok);
                CHECK(WaitOperation(service, seen, operation, voip::Error::ok));
                const voip::CallHandle queued_c = Dial(service, seen, agents[2], "sip:cancel-c@127.0.0.1:5098;transport=tcp");
                const unsigned reject_sequence = peer.Stats().incoming_invites + 1;
                const voip::CallHandle queued_d = Incoming(service, seen, peer, SipPeerScript::answer, 3);
                const unsigned remote_cancel_before = peer.Stats().cancels;
                const voip::CallHandle remote_cancel = Incoming(service, seen, peer, SipPeerScript::cancel_incoming, 4);
                submission = service.Reject(queued_d, 486, &operation);
                Submitted(seen, submission, operation, voip::Error::ok);
                submission = service.Cancel(queued_c, &operation);
                Submitted(seen, submission, operation, voip::Error::ok);
                CHECK(Wait(service, seen, seen.finished, 2));
                CHECK(peer.IncomingResponseOrder(reject_sequence, 486) != 0);
                CHECK(Wait(service, seen, seen.finished, 3));
                CHECK(peer.Stats().cancels == remote_cancel_before + 1);
                for (unsigned i = 0; i < seen.count; ++i) if (Same(seen.calls[i].handle, remote_cancel)) {
                    CHECK(seen.calls[i].terminals == 1 && seen.calls[i].error == voip::Error::cancelled && seen.calls[i].sip == 487);
                }
                CHECK(peer.Stats().cancel_responses != 0);
                EndCall(service, seen, b); EndCall(service, seen, a);
                CHECK(WaitState(service, seen, queued_a, voip::CallState::established));
                const int64_t reinvite_deadline = k_uptime_get() + 1000;
                while (peer.Stats().reinvite_acks < peer.Stats().reinvites && k_uptime_get() < reinvite_deadline)
                    Poll(service, seen);
                CHECK(peer.Stats().reinvite_acks == peer.Stats().reinvites);
                peer.SetNextOutgoing(SipPeerScript::reject_488);
                submission = service.SetHeld(queued_a, true, &operation);
                Submitted(seen, submission, operation, voip::Error::negotiation_failed);
                CHECK(WaitOperation(service, seen, operation, voip::Error::negotiation_failed));
                CHECK(WaitState(service, seen, queued_a, voip::CallState::established));
                for (unsigned i=0; i<seen.operation_count; ++i)
                    if (seen.operations[i].id==operation) CHECK(seen.operations[i].sip==488);
                for (auto script : {SipPeerScript::reject_500, SipPeerScript::unusable_sdp}) {
                    const auto expected = script==SipPeerScript::reject_500 ?
                        voip::Error::remote_rejected : voip::Error::negotiation_failed;
                    peer.SetNextOutgoing(script);
                    submission = service.SetHeld(queued_a, true, &operation);
                    Submitted(seen, submission, operation, expected);
                    CHECK(WaitOperation(service, seen, operation, expected));
                    CHECK(WaitState(service, seen, queued_a, voip::CallState::established));
                }
                peer.SetNextOutgoing(SipPeerScript::answer);
                submission = service.SetHeld(queued_a, true, &operation);
                Submitted(seen, submission, operation, voip::Error::ok);
                CHECK(WaitState(service, seen, queued_a, voip::CallState::hold));
                CHECK(WaitOperation(service, seen, operation, voip::Error::ok));
                peer.SetNextOutgoing(SipPeerScript::reject_488);
                submission = service.SetHeld(queued_a, false, &operation);
                Submitted(seen, submission, operation, voip::Error::negotiation_failed);
                CHECK(WaitOperation(service, seen, operation, voip::Error::negotiation_failed));
                CHECK(WaitState(service, seen, queued_a, voip::CallState::hold));
                for (unsigned i=0; i<seen.operation_count; ++i)
                    if (seen.operations[i].id==operation) CHECK(seen.operations[i].sip==488);
                peer.SetNextOutgoing(SipPeerScript::answer);
                submission = service.SetHeld(queued_a, false, &operation);
                Submitted(seen, submission, operation, voip::Error::ok);
                CHECK(WaitState(service, seen, queued_a, voip::CallState::established));
                CHECK(WaitOperation(service, seen, operation, voip::Error::ok));
                EndCall(service, seen, queued_a);
            }
        } else if (lifecycle == 4) {
            peer.SetNextOutgoing(SipPeerScript::no_final);
            const voip::CallHandle outgoing = Dial(service, seen, agents[0], "sip:timeout-out@127.0.0.1:5098;transport=tcp");
            const unsigned incoming_sequence = peer.Stats().incoming_invites + 1;
            const voip::CallHandle incoming = Incoming(service, seen, peer, SipPeerScript::answer, 1);
            CHECK(WaitResources(service, seen, 2, 0));
            CHECK(WaitTransition(service, seen, outgoing, voip::CallTransition::timeout));
            CHECK(WaitTransition(service, seen, incoming, voip::CallTransition::timeout));
            const auto *out = Find(seen, "sip:timeout-out@127.0.0.1:5098;transport=tcp");
            CHECK(out != nullptr && out->timeouts == 1 && out->trace_count == 2 &&
                  out->states[0] == voip::CallState::initiated && out->states[1] == voip::CallState::idle);
            // Dial completes on durable admission; timeout cannot complete it
            // a second time. An unanswered incoming call has no operation.
            CHECK(out != nullptr && OperationDone(seen, out->operation, voip::Error::ok));
            char incoming_remote[64]{};
            std::snprintf(incoming_remote, sizeof(incoming_remote),
                          "<sip:remote-in-%u@peer.test>", incoming_sequence);
            const auto *inc = Find(seen, incoming_remote);
            CHECK(inc != nullptr && inc->timeouts == 1 && inc->trace_count == 2 &&
                  inc->states[0] == voip::CallState::initiated && inc->states[1] == voip::CallState::idle);
            CHECK(inc != nullptr && inc->operation == 0);
            CHECK(WaitResources(service, seen, 0, 0));
            CHECK(pjsua_call_get_count() == 0);
            CHECK(peer.IncomingResponseOrder(incoming_sequence, 180) != 0);
        } else if (lifecycle == 5) {
            struct Fault { SipPeerScript script; const char *uri; voip::Error error; unsigned sip; };
            const Fault faults[] = {
                {SipPeerScript::malformed_sdp, "sip:malformed@127.0.0.1:5098;transport=tcp", voip::Error::negotiation_failed, 415},
                {SipPeerScript::reject_486, "sip:busy@127.0.0.1:5098;transport=tcp", voip::Error::remote_rejected, 486},
                {SipPeerScript::close_transport, "sip:eof@127.0.0.1:5098;transport=tcp", voip::Error::signaling_failed, 503},
                {SipPeerScript::bye_after_ack, "sip:bye@127.0.0.1:5098;transport=tcp", voip::Error::ok, 200}};
            for (const auto &fault : faults) {
                const unsigned want = seen.finished + 1;
                peer.SetNextOutgoing(fault.script);
                Dial(service, seen, agents[0], fault.uri);
                CHECK(Wait(service, seen, seen.finished, want));
                const auto *call = Find(seen, fault.uri);
                CHECK(call != nullptr && call->terminals == 1 && call->error == fault.error && call->sip == fault.sip);
                CHECK(WaitResources(service, seen, 0, 0));
            }
            // The Task 7A recovery gate: both slots work together after the
            // malformed SDP, rejection, transport EOF and remote BYE cases.
            peer.SetNextOutgoing(SipPeerScript::answer);
            const unsigned accepted = seen.accepted;
            const auto reused_a = Dial(service, seen, agents[0], "sip:reuse-a@127.0.0.1:5098;transport=tcp");
            const auto reused_b = Incoming(service, seen, peer, SipPeerScript::answer, 1);
            voip::OperationId answer{};
            const auto submission = service.Answer(reused_b, &answer);
            Submitted(seen, submission, answer, voip::Error::ok);
            CHECK(Wait(service, seen, seen.accepted, accepted + 2));
            CHECK(!Same(reused_a, reused_b));
            CheckPool(service, 2, 0);
            EndCall(service, seen, reused_b);
            EndCall(service, seen, reused_a);
            CHECK(WaitResources(service, seen, 0, 0));
        }
        if (lifecycle==4) {
            struct DirectionCase { SipPeerScript script; int local; };
            const DirectionCase directions[]={{SipPeerScript::sendonly,PJMEDIA_DIR_DECODING},
                {SipPeerScript::recvonly,PJMEDIA_DIR_ENCODING},{SipPeerScript::inactive,PJMEDIA_DIR_NONE}};
            for (unsigned i=0;i<3;++i) {
                peer.SetNextOutgoing(directions[i].script);
                char uri[96]{};
                std::snprintf(uri,sizeof(uri),"sip:direction-%u@127.0.0.1:5098;transport=tcp",i);
                const auto call=Dial(service,seen,agents[0],uri);
                CHECK(WaitState(service,seen,call,voip::CallState::established));
                const auto deadline=k_uptime_get()+3000;
                while (k_uptime_get()<deadline &&
                       (!peer.MediaReady(0) || peer.MediaDirection(0)!=directions[i].local)) Poll(service,seen);
                printk("[multi-call] remote SDP case=%u local direction=%d ready=%u\n",i,peer.MediaDirection(0),peer.MediaReady(0));
                CHECK(peer.MediaReady(0) && peer.MediaDirection(0)==directions[i].local);
                EndCall(service,seen,call);
            }
            peer.SetNextOutgoing(SipPeerScript::answer);
        }
        RunFullCapacityCycle(service, seen, peer, agents, lifecycle);
        voip::CallHandle old_calls[16]{};
        const unsigned old_count = seen.count;
        for (unsigned i = 0; i < old_count; ++i) old_calls[i] = seen.calls[i].handle;
        AuditShutdown(service, seen, agents, old_calls, old_count, lifecycle);
        previous_count = old_count;
        std::memcpy(previous_calls, old_calls, sizeof(previous_calls));
        std::memcpy(previous_agents, agents, sizeof(previous_agents));
        CHECK(peer.Stats().parse_failures == 0);
    }
    CHECK(peer.Stats().minimum_stack_unused >= 512);
    printk("[multi-call] peer minimum unused stack=%u bytes\n", peer.Stats().minimum_stack_unused);
    CHECK(voip::CoreActor::RegistrationTestMinimumUnusedStackBytes()>=512);
    printk("[multi-call] actor minimum unused stack=%u bytes\n",
           static_cast<unsigned>(voip::CoreActor::RegistrationTestMinimumUnusedStackBytes()));
    const auto clock=peer.Stats();
    CHECK(clock.clock_ticks!=0);
    printk("[multi-call] conference clock ticks=%u skipped=%u max-lateness=%u ms\n",
           clock.clock_ticks,clock.clock_skipped,clock.clock_max_lateness_ms);
    if (failures == 0) printk("VOIP MULTI CALL RESULT: PASSED (2 promoted, 5 FIFO, 5 lifecycles)\n");
    else printk("VOIP MULTI CALL RESULT: FAILED (%d checks)\n", failures);
    return failures == 0 ? 0 : 1;
}
