#include "../test_support/ScriptedRegistrar.hpp"

#include <voip/VoipService.hpp>
#include "../../../voip/src/pjsua/PjsuaCallbackRouter.hpp"
#include "../../../voip/src/core/CoreActor.hpp"
#include <pj_zephyr_pool_arena.h>
#include <pjsua-lib/pjsua.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_heap.h>
#include <zephyr/sys/printk.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" void voip_general_heap_tracking_begin() noexcept;
extern "C" std::size_t voip_general_heap_tracking_end() noexcept;

namespace {
constexpr voip::PcmFormat format{8000, 80, 1, voip::SampleFormat::signed_16};
constexpr unsigned event_wait_ms = 50;
constexpr unsigned settle_wait_ms = 9000;
int failures;
unsigned heap_clean_lifecycles;
#define CHECK(condition) do { if (!(condition)) { printk("[PJSUA registration] FAIL line %d: %s\n", __LINE__, #condition); ++failures; } } while (false)

class StubSource final : public voip::PcmSource {
public:
    voip::PcmFormat Format() const noexcept override { return format; }
    voip::Error Read(std::int16_t *, std::size_t, std::uint64_t) noexcept override { ++calls_; return voip::Error::ok; }
    unsigned Calls() const noexcept { return calls_; }
private: unsigned calls_{};
};
class StubSink final : public voip::PcmSink {
public:
    voip::PcmFormat Format() const noexcept override { return format; }
    voip::Error Write(const std::int16_t *, std::size_t, std::uint64_t) noexcept override { ++calls_; return voip::Error::ok; }
    void Flush() noexcept override { ++flushes_; }
    unsigned Calls() const noexcept { return calls_; }
    unsigned Flushes() const noexcept { return flushes_; }
private: unsigned calls_{}; unsigned flushes_{};
};
struct Seen {
    unsigned registered[5]{}, refreshing[5]{}, transport_failed[5]{}, retry_wait[5]{}, authentication_failed[5]{}, authentication_failed_403[5]{};
    voip::RegistrationState transitions[5][16]{};
    unsigned transition_count[5]{};
    bool transition_overflow{};
    unsigned stopped_count{};
    bool stopped{}, after_stopped{};
};
void Record(const voip::Event &event, Seen &seen) {
    if (seen.stopped) seen.after_stopped = true;
    if (event.type == voip::EventType::service_stopped) { seen.stopped = true; ++seen.stopped_count; return; }
    if (event.type != voip::EventType::agent_snapshot || event.agent.slot >= 5) return;
    const unsigned slot = event.agent.slot;
    if (seen.transition_count[slot] < 16)
        seen.transitions[slot][seen.transition_count[slot]++] =
            event.agent_snapshot.registration;
    else
        seen.transition_overflow = true;
    switch (event.agent_snapshot.registration) {
    case voip::RegistrationState::registered: ++seen.registered[slot]; break;
    case voip::RegistrationState::refreshing: ++seen.refreshing[slot]; break;
    case voip::RegistrationState::transport_failed: ++seen.transport_failed[slot]; break;
    case voip::RegistrationState::retry_wait: ++seen.retry_wait[slot]; break;
    case voip::RegistrationState::authentication_failed:
        ++seen.authentication_failed[slot];
        if (event.status.sip_status == 403) ++seen.authentication_failed_403[slot];
        break;
    default: break;
    }
}
bool RetrySequence(const Seen &seen, unsigned slot) {
    for (unsigned index = 0; index + 3 < seen.transition_count[slot]; ++index) {
        if (seen.transitions[slot][index] == voip::RegistrationState::transport_failed &&
            seen.transitions[slot][index + 1] == voip::RegistrationState::retry_wait &&
            seen.transitions[slot][index + 2] == voip::RegistrationState::registering) {
            unsigned next = index + 3;
            while (next < seen.transition_count[slot] &&
                   seen.transitions[slot][next] == voip::RegistrationState::registering)
                ++next;
            if (next < seen.transition_count[slot] &&
                seen.transitions[slot][next] == voip::RegistrationState::registered)
                return true;
        }
    }
    return false;
}
bool RefreshSequence(const Seen &seen, unsigned slot) {
    for (unsigned index = 0; index + 1 < seen.transition_count[slot]; ++index) {
        if (seen.transitions[slot][index] == voip::RegistrationState::refreshing &&
            seen.transitions[slot][index + 1] == voip::RegistrationState::registered)
            return true;
    }
    return false;
}
bool RetryFailuresObserved(const Seen &s) {
    return s.registered[0] && s.registered[2] && s.authentication_failed[4] &&
           s.transport_failed[1] && s.transport_failed[3] &&
           s.retry_wait[1] && s.retry_wait[3];
}
void DrainUntil(voip::VoipService &service, Seen &seen, bool (*complete)(const Seen &), unsigned timeout_ms) {
    for (unsigned elapsed = 0; elapsed < timeout_ms && !complete(seen); elapsed += event_wait_ms) {
        voip::Event event{};
        if (service.WaitForEvent(&event, event_wait_ms) == voip::Error::ok) Record(event, seen);
    }
}
void Wipe(char (&value)[96]) { std::memset(value, 'X', sizeof(value)); }

void CheckTrackedAllocation(void *memory) {
    const std::size_t observed = voip_general_heap_tracking_end();
    CHECK(memory != nullptr);
    CHECK(observed != 0);
}

void VerifyGeneralHeapTracker() {
    void *memory;

    voip_general_heap_tracking_begin();
    void *(*volatile allocate)(std::size_t) = std::malloc;
    memory = allocate(8);
    CheckTrackedAllocation(memory);
    std::free(memory);

    voip_general_heap_tracking_begin();
    void *(*volatile allocate_zeroed)(std::size_t, std::size_t) = std::calloc;
    memory = allocate_zeroed(1, 8);
    CheckTrackedAllocation(memory);
    std::free(memory);

    void *original = std::malloc(8);
    voip_general_heap_tracking_begin();
    void *(*volatile resize)(void *, std::size_t) = std::realloc;
    memory = resize(original, 16);
    CheckTrackedAllocation(memory);
    std::free(memory != nullptr ? memory : original);

    voip_general_heap_tracking_begin();
    void *(*volatile allocate_aligned)(std::size_t, std::size_t) =
        ::aligned_alloc;
    memory = allocate_aligned(8, 8);
    CheckTrackedAllocation(memory);
    std::free(memory);

    voip_general_heap_tracking_begin();
    memory = k_malloc(8);
    CheckTrackedAllocation(memory);
    k_free(memory);

    voip_general_heap_tracking_begin();
    memory = k_calloc(1, 8);
    CheckTrackedAllocation(memory);
    k_free(memory);

    original = k_malloc(8);
    voip_general_heap_tracking_begin();
    memory = k_realloc(original, 16);
    CheckTrackedAllocation(memory);
    k_free(memory != nullptr ? memory : original);

    voip_general_heap_tracking_begin();
    memory = k_aligned_alloc(8, 8);
    CheckTrackedAllocation(memory);
    k_free(memory);

    alignas(8) static std::uint8_t heap_memory[512];
    static sys_heap test_heap;
    sys_heap_init(&test_heap, heap_memory, sizeof(heap_memory));

    voip_general_heap_tracking_begin();
    memory = sys_heap_alloc(&test_heap, 8);
    CheckTrackedAllocation(memory);
    sys_heap_free(&test_heap, memory);

    voip_general_heap_tracking_begin();
    memory = sys_heap_noalign_alloc(&test_heap, 8, 8);
    CheckTrackedAllocation(memory);

    original = memory;
    voip_general_heap_tracking_begin();
    memory = sys_heap_realloc(&test_heap, original, 16);
    CheckTrackedAllocation(memory);
    sys_heap_free(&test_heap, memory != nullptr ? memory : original);

    voip_general_heap_tracking_begin();
    memory = sys_heap_aligned_alloc(&test_heap, 8, 8);
    CheckTrackedAllocation(memory);

    original = memory;
    voip_general_heap_tracking_begin();
    memory = sys_heap_aligned_realloc(&test_heap, original, 8, 16);
    CheckTrackedAllocation(memory);
    sys_heap_free(&test_heap, memory != nullptr ? memory : original);
}

voip::VoipService &ServiceUnderTest() noexcept {
    static voip::VoipService service;
    return service;
}

bool WaitForRegistered(voip::VoipService &service, unsigned slot,
                       unsigned timeout_ms) {
    for (unsigned elapsed = 0; elapsed < timeout_ms; elapsed += event_wait_ms) {
        voip::Event event{};
        if (service.WaitForEvent(&event, event_wait_ms) != voip::Error::ok) continue;
        if (event.type == voip::EventType::agent_snapshot &&
            event.agent.slot == slot &&
            event.agent_snapshot.registration == voip::RegistrationState::registered)
            return true;
    }
    return false;
}

void RunOneAccountSmoke() {
    const int failures_before = failures;
    auto &registrar = voip_integration::ScriptedRegistrar::Instance();
    registrar.Reset();
    auto &service = ServiceUnderTest();
    StubSource source;
    StubSink sink;
    char identity[96]{};
    char registrar_uri[96]{};
    char username[96]{};
    char password[96]{};
    std::snprintf(identity, sizeof(identity), "<sip:agent0@plan3.test>");
    std::snprintf(registrar_uri, sizeof(registrar_uri),
                  "sip:127.0.0.1:%u;transport=tcp",
                  voip_integration::ScriptedRegistrar::listener_port);
    std::snprintf(username, sizeof(username), "agent0");
    std::snprintf(password, sizeof(password), "valid-0");
    const voip::AgentConfig agent{{identity, registrar_uri, username, password},
                                  {&source, &sink}, true};
    const voip::ServiceConfig config{&agent, 1, 1000, 1000, format,
                                     {voip::SignalingSecurity::none,
                                      voip::MediaSecurity::none}};
    CHECK(service.Initialize(config) == voip::Error::ok);
    CHECK(WaitForRegistered(service, 0, settle_wait_ms));
    CHECK(registrar.Stats().requests != 0);
    CHECK(registrar.Stats().requests == registrar.Stats().tcp_requests);
    CHECK(service.Shutdown() == voip::Error::ok);
    // The public API deliberately retains its terminal event until consumers
    // drain it; consume that lifecycle before beginning the next smoke stage.
    for (;;) {
        voip::Event event{};
        if (service.WaitForEvent(&event, event_wait_ms) != voip::Error::ok) break;
    }
    if (failures == failures_before)
        printk("VOIP PJSUA REGISTRATION SMOKE: PASSED (1 account, TCP Digest, actor polling)\n");
}

bool FiveAccountAuthenticationComplete(const Seen &seen) {
    return seen.registered[0] == 1 && seen.registered[1] == 1 &&
           seen.registered[2] == 1 && seen.registered[3] == 1 &&
           seen.authentication_failed[4] == 1 &&
           seen.authentication_failed_403[4] == 1;
}
// A complete acceptance lifecycle.  The focused fake test retains the
// early-in-flight REGISTER regression; the real profile deliberately folds no
// second shutdown diagnostic into this count, because every counted lifecycle
// must instead finish the prescribed pending-refresh-retry shutdown.
void RunCompleteLifecycle(unsigned lifecycle,
                          unsigned &completed_lifecycles,
                          unsigned &previous_reset_wipe_count) {
    const int failures_before = failures;
    CHECK(completed_lifecycles == lifecycle);
    auto &registrar = voip_integration::ScriptedRegistrar::Instance();
    registrar.Reset();
    registrar.ArmOne408(1);
    registrar.ArmOne408(3);
    registrar.UseShortSuccessfulExpiration(0);
    registrar.UseShortSuccessfulExpiration(1);
    registrar.UseShortSuccessfulExpiration(2);
    auto &service = ServiceUnderTest();
    StubSource sources[5];
    StubSink sinks[5];
    char identity[5][96]{};
    char registrar_uri[5][96]{};
    char username[5][96]{};
    char password[5][96]{};
    voip::AgentConfig agents[5]{};
    for (unsigned index = 0; index < 5; ++index) {
        std::snprintf(identity[index], sizeof(identity[index]),
                      "<sip:agent%u@plan3.test>", index);
        std::snprintf(registrar_uri[index], sizeof(registrar_uri[index]),
                      "sip:127.0.0.1:%u;transport=tcp",
                      voip_integration::ScriptedRegistrar::listener_port);
        std::snprintf(username[index], sizeof(username[index]), "agent%u", index);
        std::snprintf(password[index], sizeof(password[index]), "valid-%u", index);
        agents[index] = {
            {identity[index], registrar_uri[index], username[index], password[index]},
            {&sources[index], &sinks[index]}, true};
        for (unsigned prior = 0; prior < index; ++prior) {
            CHECK(agents[prior].audio.source != agents[index].audio.source);
            CHECK(agents[prior].audio.sink != agents[index].audio.sink);
        }
    }
    std::strcpy(password[4], "wrong-password");
    const voip::ServiceConfig config{
        agents, 5, 1000, 1000, format,
        {voip::SignalingSecurity::none, voip::MediaSecurity::none}};
    const voip::Error initialize_result = service.Initialize(config);
    CHECK(initialize_result == voip::Error::ok);
    if (initialize_result == voip::Error::ok)
        voip_general_heap_tracking_begin();
    voip::AgentHandle handles[5]{};
    for (unsigned index = 0; index < 5; ++index) {
        CHECK(service.GetAgentHandle(index, &handles[index]) == voip::Error::ok);
        CHECK(handles[index].slot == index);
        Wipe(identity[index]);
        Wipe(registrar_uri[index]);
        Wipe(username[index]);
        Wipe(password[index]);
        CHECK(identity[index][0] == 'X' && registrar_uri[index][0] == 'X' &&
              username[index][0] == 'X' && password[index][0] == 'X');
    }

    Seen seen{};
    DrainUntil(service, seen, RetryFailuresObserved, settle_wait_ms);
    CHECK(RetryFailuresObserved(seen));
    CHECK(!seen.stopped && !seen.after_stopped);
    CHECK(seen.authentication_failed[0] == 0 &&
          seen.authentication_failed[1] == 0 &&
          seen.authentication_failed[2] == 0 &&
          seen.authentication_failed[3] == 0);
    CHECK(seen.transport_failed[0] == 0 && seen.transport_failed[2] == 0 &&
          seen.transport_failed[4] == 0);
    CHECK(seen.retry_wait[0] == 0 && seen.retry_wait[2] == 0 &&
          seen.retry_wait[4] == 0);
    const Seen non_target_before_retry = seen;
    const voip_integration::RegistrarStats before_retry = registrar.Stats();

    DrainUntil(service, seen, [](const Seen &value) {
        return RetrySequence(value, 1) && RetrySequence(value, 3) &&
               FiveAccountAuthenticationComplete(value);
    }, settle_wait_ms);
    CHECK(FiveAccountAuthenticationComplete(seen));
    CHECK(RetrySequence(seen, 1) && RetrySequence(seen, 3));
    CHECK(!seen.transition_overflow);
    CHECK(seen.transport_failed[1] == 1 && seen.retry_wait[1] == 1);
    CHECK(seen.transport_failed[3] == 1 && seen.retry_wait[3] == 1);
    CHECK(seen.authentication_failed[0] == 0 && seen.authentication_failed[1] == 0 &&
          seen.authentication_failed[2] == 0 && seen.authentication_failed[3] == 0);
    CHECK(seen.authentication_failed[4] == 1 && seen.authentication_failed_403[4] == 1);
    CHECK(seen.registered[4] == 0);
    CHECK(seen.transport_failed[0] == 0 && seen.transport_failed[2] == 0 &&
          seen.transport_failed[4] == 0);
    CHECK(seen.retry_wait[0] == 0 && seen.retry_wait[2] == 0 && seen.retry_wait[4] == 0);
    CHECK(seen.registered[0] == non_target_before_retry.registered[0]);
    CHECK(seen.registered[2] == non_target_before_retry.registered[2]);
    CHECK(seen.registered[4] == non_target_before_retry.registered[4]);
    CHECK(seen.authentication_failed[4] ==
          non_target_before_retry.authentication_failed[4]);
    CHECK(seen.authentication_failed_403[4] ==
          non_target_before_retry.authentication_failed_403[4]);
    const voip_integration::RegistrarStats after_retry = registrar.Stats();
    CHECK(after_retry.registrations_by_agent[0] ==
          before_retry.registrations_by_agent[0]);
    CHECK(after_retry.registrations_by_agent[1] ==
          before_retry.registrations_by_agent[1] + 1);
    CHECK(after_retry.registrations_by_agent[2] ==
          before_retry.registrations_by_agent[2]);
    CHECK(after_retry.registrations_by_agent[3] ==
          before_retry.registrations_by_agent[3] + 1);
    CHECK(after_retry.registrations_by_agent[4] ==
          before_retry.registrations_by_agent[4]);
    CHECK(after_retry.registrations == 4 && after_retry.unregistrations == 0);

    const Seen before_refresh = seen;
    // Arm the next request only after the initial recovery.  Agent 1 has its
    // own short registrar expiry, so this is the required pending retry at
    // shutdown rather than a synthetic second service lifecycle.
    registrar.ArmOne408(1);
    DrainUntil(service, seen, [](const Seen &value) {
        return RefreshSequence(value, 0) && RefreshSequence(value, 2) &&
               value.retry_wait[1] >= 2;
    }, settle_wait_ms);
    CHECK(RefreshSequence(seen, 0) && RefreshSequence(seen, 2));
    CHECK(seen.retry_wait[1] == before_refresh.retry_wait[1] + 1);
    CHECK(seen.transport_failed[1] == before_refresh.transport_failed[1] + 1);
    CHECK(seen.retry_wait[0] == before_refresh.retry_wait[0] &&
          seen.retry_wait[2] == before_refresh.retry_wait[2] &&
          seen.retry_wait[3] == before_refresh.retry_wait[3] &&
          seen.retry_wait[4] == before_refresh.retry_wait[4]);
    CHECK(seen.transport_failed[0] == before_refresh.transport_failed[0] &&
          seen.transport_failed[2] == before_refresh.transport_failed[2] &&
          seen.transport_failed[3] == before_refresh.transport_failed[3] &&
          seen.transport_failed[4] == before_refresh.transport_failed[4]);
    for (unsigned index = 0; index < 5; ++index) {
        CHECK(seen.authentication_failed[index] == before_refresh.authentication_failed[index]);
    }
    CHECK(seen.registered[0] == before_refresh.registered[0] + 1);
    CHECK(seen.registered[1] == before_refresh.registered[1]);
    CHECK(seen.registered[2] == before_refresh.registered[2] + 1);
    CHECK(seen.registered[3] == before_refresh.registered[3]);
    CHECK(seen.registered[4] == before_refresh.registered[4]);
    CHECK(!seen.transition_overflow);
    const voip_integration::RegistrarStats before_shutdown = registrar.Stats();
    CHECK(before_shutdown.registrations == after_retry.registrations + 2);
    CHECK(before_shutdown.unregistrations == after_retry.unregistrations);
    CHECK(before_shutdown.registrations_by_agent[0] ==
          after_retry.registrations_by_agent[0] + 1);
    CHECK(before_shutdown.registrations_by_agent[1] ==
          after_retry.registrations_by_agent[1]);
    CHECK(before_shutdown.registrations_by_agent[2] ==
          after_retry.registrations_by_agent[2] + 1);
    CHECK(before_shutdown.registrations_by_agent[3] ==
          after_retry.registrations_by_agent[3]);
    CHECK(before_shutdown.registrations_by_agent[4] ==
          after_retry.registrations_by_agent[4]);
    voip::AgentSnapshot pending{};
    CHECK(service.GetAgentSnapshot(handles[1], &pending) == voip::Error::ok);
    CHECK(pending.registration == voip::RegistrationState::retry_wait);
    CHECK(service.Shutdown() == voip::Error::ok);
    CHECK(service.Shutdown() == voip::Error::ok);
    voip::Error drain_result{};
    for (;;) {
        voip::Event event{};
        drain_result = service.WaitForEvent(&event, event_wait_ms);
        if (drain_result != voip::Error::ok) break;
        Record(event, seen);
    }
    CHECK(drain_result == voip::Error::timed_out);
    CHECK(seen.stopped_count == 1 && !seen.after_stopped && !seen.transition_overflow);
    CHECK(seen.registered[0] == 2 && seen.registered[1] == 1 &&
          seen.registered[2] == 2 && seen.registered[3] == 1 &&
          seen.registered[4] == 0);
    CHECK(seen.transport_failed[0] == 0 && seen.transport_failed[1] == 2 &&
          seen.transport_failed[2] == 0 && seen.transport_failed[3] == 1 &&
          seen.transport_failed[4] == 0);
    CHECK(seen.retry_wait[0] == 0 && seen.retry_wait[1] == 2 &&
          seen.retry_wait[2] == 0 && seen.retry_wait[3] == 1 &&
          seen.retry_wait[4] == 0);
    CHECK(seen.authentication_failed[0] == 0 &&
          seen.authentication_failed[1] == 0 &&
          seen.authentication_failed[2] == 0 &&
          seen.authentication_failed[3] == 0 &&
          seen.authentication_failed[4] == 1 &&
          seen.authentication_failed_403[4] == 1);
    CHECK(pjsua_acc_get_count() == 0 && pjsua_get_state() == PJSUA_STATE_NULL);
    CHECK(voip::PjsuaCallbackRouter::ActiveForTest() == nullptr);
    pj_zephyr_pool_arena_stats arena{};
    pj_zephyr_pool_arena_get_stats(&arena);
    CHECK(arena.live_blocks == 0 && arena.used_bytes == 0 && arena.reset_in_progress == 0);
    CHECK(arena.last_reset_wiped_bytes > 0 && arena.last_reset_wiped_bytes <= arena.capacity_bytes);
    const unsigned reset_wipe_count_before = previous_reset_wipe_count;
    CHECK(arena.reset_wipe_count > reset_wipe_count_before);
    for (unsigned index = 0; index < 5; ++index) {
        voip::AgentSnapshot snapshot{};
        CHECK(service.GetAgentSnapshot(handles[index], &snapshot) == voip::Error::invalid_handle);
        CHECK(sources[index].Calls() == 0 && sinks[index].Calls() == 0 && sinks[index].Flushes() == 0);
    }
    const auto resources = service.GetResourceSnapshot();
    CHECK(resources.active_agents == 0 && resources.active_calls == 0);
    CHECK(voip::CoreActor::RegistrationTestTlsProbePassed());
    CHECK(voip::CoreActor::RegistrationTestMinimumUnusedStackBytes() != 0);
    const auto stats = registrar.Stats();
    CHECK(stats.callback_failures == 0 && stats.non_tcp_requests == 0);
    CHECK(stats.requests != 0 && stats.requests == stats.tcp_requests);
    CHECK(stats.challenges >= 5 && stats.authorized >= 4 && stats.forbidden == 1);
    CHECK(stats.registrations == before_shutdown.registrations);
    CHECK(stats.unregistrations == before_shutdown.unregistrations + 3);
    const std::size_t general_heap_allocations = initialize_result == voip::Error::ok
        ? voip_general_heap_tracking_end() : 1;
    CHECK(general_heap_allocations == 0);
    if (general_heap_allocations == 0)
        ++heap_clean_lifecycles;
    if (failures == failures_before) {
        previous_reset_wipe_count = arena.reset_wipe_count;
        ++completed_lifecycles;
        CHECK(completed_lifecycles == lifecycle + 1);
        printk("VOIP PJSUA LIFECYCLE %u: PASSED (auth, 408 recovery, refresh, pending retry shutdown)\n",
               lifecycle + 1);
    }
}
} // namespace
int main() {
    printk("VoIP PJSUA five-account TCP Digest registration validation\n");
    VerifyGeneralHeapTracker();
    RunOneAccountSmoke();
    pj_zephyr_pool_arena_stats arena{};
    pj_zephyr_pool_arena_get_stats(&arena);
    unsigned previous_reset_wipe_count = arena.reset_wipe_count;
    unsigned completed_lifecycles = 0;
    for (unsigned lifecycle = 0; lifecycle < 5; ++lifecycle)
        RunCompleteLifecycle(lifecycle, completed_lifecycles,
                             previous_reset_wipe_count);
    if (failures != 0 || completed_lifecycles != 5 ||
        heap_clean_lifecycles != 5) {
        printk("VOIP PJSUA REGISTRATION RESULT: FAILED (%d checks)\n", failures);
    } else {
        printk("VOIP PJSUA GENERAL HEAP: PASSED (0 allocations after initialization)\n");
        printk("VOIP PJSUA REGISTRATION RESULT: PASSED (5 independent accounts, 5 lifecycles)\n");
    }
    return failures == 0 ? 0 : 1;
}
