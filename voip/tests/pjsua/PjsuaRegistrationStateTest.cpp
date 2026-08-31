#include "FakePjsuaApi.hpp"
#include "../../src/core/AgentRegistry.hpp"
#include "../../src/pjsua/PjsuaAccountManager.hpp"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <limits>
#include <zephyr/sys/printk.h>

namespace voip::test {
namespace {
class Source final : public PcmSource { public: PcmFormat Format() const noexcept override { return {8000, 160, 1, SampleFormat::signed_16}; } Error Read(std::int16_t *, std::size_t, std::uint64_t) noexcept override { return Error::ok; } };
class Sink final : public PcmSink { public: PcmFormat Format() const noexcept override { return {8000, 160, 1, SampleFormat::signed_16}; } Error Write(const std::int16_t *, std::size_t, std::uint64_t) noexcept override { return Error::ok; } void Flush() noexcept override {} };
struct Fixture {
    Source sources[5]{}; Sink sinks[5]{}; AgentConfig agents[5]{};
    ServiceConfig config{agents, 5, 1000, 5000, {8000, 160, 1, SampleFormat::signed_16}, {SignalingSecurity::none, MediaSecurity::none}};
    Fixture() noexcept {
        for (std::size_t i = 0; i < 5; ++i)
            agents[i] = {{i == 0 ? "sip:zero@example.test" : i == 1 ? "sip:one@example.test" : i == 2 ? "sip:two@example.test" : i == 3 ? "sip:three@example.test" : "sip:four@example.test", "sip:registrar.example.test", "user", "password"}, {&sources[i], &sinks[i]}, i != 4};
    }
    void Initialize(AgentRegistry &registry) noexcept { assert(registry.Initialize(config) == Error::ok); }
};

PjsuaRegistrationRecord Record(pjsua_acc_id account, pj_status_t native_status,
                               int sip_code, const char *reason, bool renew,
                               bool unregistration, unsigned expiration) noexcept {
    PjsuaRegistrationRecord record{};
    record.account = account;
    record.native_status = native_status;
    record.sip_status = sip_code;
    record.renew = renew;
    record.unregistration = unregistration;
    record.expiration = expiration;
    std::strncpy(record.reason, reason, max_reason_length);
    return record;
}

void Expect(PjsuaAccountManager &accounts, RegistrationState state,
            Error error, std::uint16_t sip, const char *reason) {
    PjsuaRegistrationNotification notification{};
    assert(accounts.TryGetNotification(&notification));
    assert(notification.registration == state);
    assert(notification.status.error == error && notification.status.sip_status == sip);
    assert(std::strcmp(notification.status.reason, reason) == 0);
}

void Drain(PjsuaAccountManager &accounts) {
    PjsuaRegistrationNotification notification{};
    while (accounts.TryGetNotification(&notification)) {}
}
}

void RunPjsuaRegistrationStateTests() {
    Fixture fixture; AgentRegistry registry; fixture.Initialize(registry);
    FakePjsuaApi fake; PjsuaAccountManager accounts(fake.Api());
    assert(accounts.Initialize(registry, 4) == Error::ok);

    accounts.OnRegistrationStarted(Record(2, PJ_SUCCESS, 0, "", true, false, 0));
    Expect(accounts, RegistrationState::registering, Error::ok, 0, "");
    accounts.OnRegistrationStarted(Record(2, PJ_SUCCESS, 0, "", false, true, 0));
    Expect(accounts, RegistrationState::unregistering, Error::ok, 0, "");

    struct Transition { PjsuaRegistrationRecord record; RegistrationState state; Error error; std::uint16_t sip; const char *reason; bool retry; };
    const Transition cases[] = {
        {Record(2, PJ_SUCCESS, 200, "OK", true, false, 20), RegistrationState::registered, Error::ok, 200, "OK", false},
        {Record(2, PJ_SUCCESS, 200, "OK", true, false, 20), RegistrationState::registered, Error::ok, 200, "OK", false},
        {Record(2, PJ_SUCCESS, 200, "OK", false, true, 0), RegistrationState::disabled, Error::ok, 200, "OK", false},
        {Record(2, PJ_SUCCESS, 401, "Unauthorized", true, false, 0), RegistrationState::authentication_failed, Error::authentication_failed, 401, "Unauthorized", false},
        {Record(2, PJ_SUCCESS, 407, "Proxy Authentication Required", true, false, 0), RegistrationState::authentication_failed, Error::authentication_failed, 407, "Proxy Authentication Required", false},
        {Record(2, PJ_SUCCESS, 403, "Forbidden", true, false, 0), RegistrationState::authentication_failed, Error::authentication_failed, 403, "Forbidden", false},
        {Record(2, PJ_SUCCESS, 408, "Request Timeout", true, false, 0), RegistrationState::transport_failed, Error::signaling_failed, 408, "Request Timeout", true},
        {Record(2, PJ_SUCCESS, 480, "Temporarily Unavailable", true, false, 0), RegistrationState::transport_failed, Error::signaling_failed, 480, "Temporarily Unavailable", true},
        {Record(2, PJ_EUNKNOWN, 0, "Transport", true, false, 0), RegistrationState::transport_failed, Error::signaling_failed, 0, "Transport", true},
        {Record(2, PJ_SUCCESS, 503, "Unavailable", true, false, 0), RegistrationState::transport_failed, Error::signaling_failed, 503, "Unavailable", true},
        {Record(2, PJ_SUCCESS, 603, "Decline", true, false, 0), RegistrationState::transport_failed, Error::signaling_failed, 603, "Decline", true},
        {Record(2, PJ_SUCCESS, 404, "Not Found", true, false, 0), RegistrationState::transport_failed, Error::remote_rejected, 404, "Not Found", false},
    };
    for (const Transition &test : cases) {
        accounts.OnRegistrationState(test.record);
        Expect(accounts, test.state, test.error, test.sip, test.reason);
        if (test.retry) Expect(accounts, RegistrationState::retry_wait, test.error, test.sip, test.reason);
        else assert(!accounts.Resolve(2)->retry.scheduled);
    }

    // A full bounded queue may discard coalescible progress records, but never
    // the failure that must precede this agent's retry-wait snapshot.
    for (unsigned i = 0; i < 32; ++i)
        accounts.OnRegistrationStarted(Record(2, PJ_SUCCESS, 0, "", true, false, 0));
    accounts.OnRegistrationState(Record(2, PJ_EUNKNOWN, 0, "full", true, false, 0));
    bool saw_failure = false;
    bool saw_retry_after_failure = false;
    PjsuaRegistrationNotification queued{};
    while (accounts.TryGetNotification(&queued)) {
        if (queued.registration == RegistrationState::transport_failed) saw_failure = true;
        if (saw_failure && queued.registration == RegistrationState::retry_wait)
            saw_retry_after_failure = true;
    }
    assert(saw_failure && saw_retry_after_failure);
    Drain(accounts);

    PjsuaAccountContext *context = accounts.Resolve(2); assert(context != nullptr);
    const std::uint64_t bases[] = {1000, 2000, 4000, 8000, 16000, 30000, 30000};
    context->retry = {};
    std::uint64_t due = 0;
    for (std::size_t attempt = 0; attempt < 7; ++attempt) {
        assert(accounts.Pump(due) == Error::ok);
        accounts.OnRegistrationState(Record(2, PJ_EUNKNOWN, 0, "Transport", true, false, 0));
        Expect(accounts, RegistrationState::transport_failed, Error::signaling_failed, 0, "Transport");
        Expect(accounts, RegistrationState::retry_wait, Error::signaling_failed, 0, "Transport");
        assert(context->retry.attempt == attempt &&
               context->retry.due_ms == due + bases[attempt] + 2 * 50);
        due = context->retry.due_ms;
    }
    Drain(accounts);

    for (std::uint8_t slot = 0; slot < 5; ++slot) {
        AgentHandle handle{}; assert(registry.GetAgentHandle(slot, &handle) == Error::ok);
        pjsua_acc_id id{}; assert(accounts.NativeId(handle, &id) == Error::ok);
        PjsuaAccountContext *slot_context = accounts.Resolve(id); assert(slot_context != nullptr);
        slot_context->retry = {};
        assert(accounts.Pump(10000) == Error::ok);
        accounts.OnRegistrationState(Record(id, PJ_EUNKNOWN, 0, "jitter", true, false, 0));
        Expect(accounts, RegistrationState::transport_failed, Error::signaling_failed, 0, "jitter");
        Expect(accounts, RegistrationState::retry_wait, Error::signaling_failed, 0, "jitter");
        assert(slot_context->retry.due_ms == 11000 + static_cast<std::uint64_t>(slot) * 50);
        Drain(accounts);
    }

    PjsuaAccountContext *one = accounts.Resolve(0); PjsuaAccountContext *three = accounts.Resolve(1);
    PjsuaAccountContext *zero = accounts.Resolve(2); PjsuaAccountContext *two = accounts.Resolve(4); PjsuaAccountContext *four = accounts.Resolve(3);
    zero->retry = {4, 50000, true};
    two->retry = {4, 50000, true};
    four->retry = {4, 50000, true};
    const RetryState zero_retry = zero->retry, two_retry = two->retry, four_retry = four->retry;
    Drain(accounts);
    accounts.OnRegistrationState(Record(one->account_id, PJ_EUNKNOWN, 0, "one", true, false, 0));
    accounts.OnRegistrationState(Record(three->account_id, PJ_EUNKNOWN, 0, "three", true, false, 0));
    Drain(accounts);
    const std::size_t zero_calls = fake.RegistrationCount(zero->account_id);
    const std::size_t two_calls = fake.RegistrationCount(two->account_id);
    const std::size_t four_calls = fake.RegistrationCount(four->account_id);
    assert(accounts.Pump(one->retry.due_ms) == Error::ok);
    Drain(accounts);
    assert(accounts.Pump(three->retry.due_ms) == Error::ok);
    Drain(accounts);
    assert(zero->retry.attempt == zero_retry.attempt && zero->retry.due_ms == zero_retry.due_ms);
    assert(two->retry.attempt == two_retry.attempt && two->retry.due_ms == two_retry.due_ms);
    assert(four->retry.attempt == four_retry.attempt && four->retry.due_ms == four_retry.due_ms);
    assert(fake.RegistrationCount(zero->account_id) == zero_calls &&
           fake.RegistrationCount(two->account_id) == two_calls &&
           fake.RegistrationCount(four->account_id) == four_calls);

    context->retry = {};
    assert(accounts.Pump(std::numeric_limits<std::uint64_t>::max() - 10) == Error::ok);
    accounts.OnRegistrationState(Record(2, PJ_EUNKNOWN, 0, "saturate", true, false, 0));
    Expect(accounts, RegistrationState::transport_failed, Error::signaling_failed, 0, "saturate");
    Expect(accounts, RegistrationState::retry_wait, Error::signaling_failed, 0, "saturate");
    assert(context->retry.due_ms == std::numeric_limits<std::uint64_t>::max());
    Drain(accounts);

    assert(accounts.Pump(10000) == Error::ok);
    accounts.OnRegistrationState(Record(2, PJ_SUCCESS, 200, "OK", true, false, 20));
    Expect(accounts, RegistrationState::registered, Error::ok, 200, "OK");
    assert(context->retry.attempt == 0 && !context->retry.scheduled && context->retry.due_ms == 0);
    assert(context->refresh_due_ms == 29000);
    assert(accounts.Pump(29000) == Error::ok);
    Expect(accounts, RegistrationState::refreshing, Error::ok, 0, "");
    context->retry = {0, 30000, true};
    fake.FailRegistration(PJ_EUNKNOWN);
    const std::size_t retries_before = fake.RegistrationCount();
    assert(accounts.Pump(30000) == Error::ok && fake.RegistrationCount() == retries_before + 1);
    Expect(accounts, RegistrationState::registering, Error::ok, 0, "");
    Expect(accounts, RegistrationState::transport_failed, Error::signaling_failed, 0, "registration start failed");
    Expect(accounts, RegistrationState::retry_wait, Error::signaling_failed, 0, "registration start failed");
    assert(context->retry.attempt == 1 && context->retry.due_ms == 32000);
    fake.FailRegistration(PJ_SUCCESS);
    const std::size_t calls_before_disabled = fake.RegistrationCount();
    assert(accounts.StartInitialRegistration() == Error::ok && fake.RegistrationCount() == calls_before_disabled + 4);
    assert(!accounts.TryGetNotification(nullptr));
    printk("PjsuaRegistrationStateTest PASSED\n");
}
} // namespace voip::test
