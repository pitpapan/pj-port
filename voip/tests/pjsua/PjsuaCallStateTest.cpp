#include "FakePjsuaApi.hpp"
#include "../../src/core/AgentRegistry.hpp"
#include "../../src/pjsua/PjsuaAccountManager.hpp"
#include "../../src/pjsua/PjsuaCallManager.hpp"

#include <cassert>
#include <zephyr/sys/printk.h>

namespace voip::test {
namespace {
pjsip_inv_state state = PJSIP_INV_STATE_CALLING;
unsigned code = 0;
pjmedia_dir media = PJMEDIA_DIR_NONE;

pj_status_t StateInfo(pjsua_call_id id, pjsua_call_info *info) {
    if (info == nullptr) return PJ_EINVAL;
    *info = {};
    info->id = id;
    info->state = state;
    info->last_status = code;
    info->media_status = media == PJMEDIA_DIR_NONE ? PJSUA_CALL_MEDIA_NONE
                                                     : PJSUA_CALL_MEDIA_ACTIVE;
    info->media_dir = media;
    static char remote[] = "sip:remote@example.test";
    info->remote_info = {remote, static_cast<pj_ssize_t>(sizeof(remote) - 1)};
    return PJ_SUCCESS;
}

class Source final : public PcmSource { public: PcmFormat Format() const noexcept override { return {8000, 160, 1, SampleFormat::signed_16}; } Error Read(std::int16_t *, std::size_t, std::uint64_t) noexcept override { return Error::ok; } };
class Sink final : public PcmSink { public: PcmFormat Format() const noexcept override { return {8000, 160, 1, SampleFormat::signed_16}; } Error Write(const std::int16_t *, std::size_t, std::uint64_t) noexcept override { return Error::ok; } void Flush() noexcept override {} };
}

void RunPjsuaCallStateTests() {
    Source source; Sink sink;
    AgentConfig agent{{"sip:a@example.test", "sip:registrar.example.test", "a", "p"}, {&source, &sink}, true};
    ServiceConfig config{&agent, 1, 1000, 5000, {8000, 160, 1, SampleFormat::signed_16}, {SignalingSecurity::none, MediaSecurity::none}};
    AgentRegistry registry; assert(registry.Initialize(config) == Error::ok);
    FakePjsuaApi fake; fake.Reset();
    PjsuaAccountManager accounts(fake.Api()); assert(accounts.Initialize(registry, 4) == Error::ok);
    PjsuaApi api = fake.Api(); api.call_get_info = &StateInfo;
    PjsuaCallManager calls(accounts, api);
    pjsua_call_setting setting{}; api.call_setting_default(&setting);
    char uri[] = "sip:remote@example.test"; pj_str_t destination{uri, static_cast<pj_ssize_t>(sizeof(uri) - 1)};
    pjsua_call_id id = PJSUA_INVALID_ID;
    assert(api.call_make_call(0, &destination, &setting, nullptr, nullptr, &id) == PJ_SUCCESS);
    calls.OnIncomingCall(2, id);
    RuntimeNotification incoming{}; assert(calls.TryGetNotification(&incoming));
    assert(calls.AdmitIncoming({1, 1}, incoming.agent, incoming.token) == Error::ok);

    // Removing semantic state translation leaves this first callback silent.
    state = PJSIP_INV_STATE_CONFIRMED; media = PJMEDIA_DIR_ENCODING_DECODING;
    calls.OnCallState(id);
    RuntimeNotification notification{};
    assert(calls.TryGetNotification(&notification));
    assert(notification.type == RuntimeNotification::Type::call_accepted &&
           notification.token == incoming.token && notification.sip_status == 0);

    state = PJSIP_INV_STATE_DISCONNECTED; code = 487; media = PJMEDIA_DIR_NONE;
    calls.OnCallState(id);
    assert(calls.TryGetNotification(&notification));
    assert(notification.type == RuntimeNotification::Type::call_finished &&
           notification.token == incoming.token && notification.sip_status == 487);
    assert(calls.Count() == 0 && fake.CallUserData(id) == nullptr);

    // A duplicate terminal callback or a callback after user-data invalidation
    // must neither create a second event nor resurrect the native lease.
    calls.OnCallState(id);
    assert(!calls.TryGetNotification(&notification) && calls.Count() == 0);

    // Confirmation is a one-shot semantic event; media direction, rather than
    // a second public state store, completes hold/resume controls exactly once.
    state = PJSIP_INV_STATE_CALLING; code = 0; media = PJMEDIA_DIR_NONE;
    calls.OnIncomingCall(2, id);
    assert(calls.TryGetNotification(&incoming));
    assert(calls.AdmitIncoming({2, 1}, incoming.agent, incoming.token) == Error::ok);
    state = PJSIP_INV_STATE_EARLY; calls.OnCallState(id);
    assert(!calls.TryGetNotification(&notification));
    state = PJSIP_INV_STATE_CONFIRMED; media = PJMEDIA_DIR_ENCODING_DECODING;
    calls.OnCallState(id);
    assert(calls.TryGetNotification(&notification) &&
           notification.type == RuntimeNotification::Type::call_accepted);
    calls.OnCallState(id);
    assert(!calls.TryGetNotification(&notification));
    // Out-of-order provisional callbacks cannot roll an established native
    // phase backward or make the later terminal look like a remote rejection.
    state = PJSIP_INV_STATE_EARLY; calls.OnCallState(id);
    state = PJSIP_INV_STATE_CALLING; calls.OnCallState(id);
    assert(!calls.TryGetNotification(&notification));
    state = PJSIP_INV_STATE_CONFIRMED;
    media = PJMEDIA_DIR_ENCODING;
    calls.OnCallMediaState(id);
    assert(calls.TryGetNotification(&notification) &&
           notification.type == RuntimeNotification::Type::call_held);
    calls.OnCallMediaState(id);
    assert(!calls.TryGetNotification(&notification));
    media = PJMEDIA_DIR_NONE;
    calls.OnCallMediaState(id);
    assert(!calls.TryGetNotification(&notification));
    media = PJMEDIA_DIR_ENCODING_DECODING;
    calls.OnCallMediaState(id);
    assert(calls.TryGetNotification(&notification) &&
           notification.type == RuntimeNotification::Type::call_resumed);
    state = PJSIP_INV_STATE_DISCONNECTED; media = PJMEDIA_DIR_NONE;
    calls.OnCallState(id);
    assert(calls.TryGetNotification(&notification) &&
           notification.type == RuntimeNotification::Type::call_finished);
    assert(calls.Count() == 0 && fake.CallUserData(id) == nullptr);

    // A final 3xx–6xx before establishment has a semantic rejection followed
    // by exactly one final finish, which lets the core commit terminal state.
    state = PJSIP_INV_STATE_CALLING; code = 0;
    calls.OnIncomingCall(2, id);
    assert(calls.TryGetNotification(&incoming));
    assert(calls.AdmitIncoming({3, 1}, incoming.agent, incoming.token) == Error::ok);
    state = PJSIP_INV_STATE_EARLY; calls.OnCallState(id);
    state = PJSIP_INV_STATE_DISCONNECTED; code = 603; media = PJMEDIA_DIR_NONE;
    calls.OnCallState(id);
    assert(calls.TryGetNotification(&notification) &&
           notification.type == RuntimeNotification::Type::call_rejected &&
           notification.error == Error::remote_rejected && notification.sip_status == 603);
    assert(calls.TryGetNotification(&notification) &&
           notification.type == RuntimeNotification::Type::call_finished);
    assert(!calls.TryGetNotification(&notification) && calls.Count() == 0);

    // A transport/SDP terminal with no final SIP status is copied as a
    // signaling failure, never silently converted to a normal finish.
    state = PJSIP_INV_STATE_CALLING; code = 0;
    calls.OnIncomingCall(2, id);
    assert(calls.TryGetNotification(&incoming));
    assert(calls.AdmitIncoming({4, 1}, incoming.agent, incoming.token) == Error::ok);
    state = PJSIP_INV_STATE_EARLY; calls.OnCallState(id);
    state = PJSIP_INV_STATE_DISCONNECTED; media = PJMEDIA_DIR_NONE;
    calls.OnCallState(id);
    assert(calls.TryGetNotification(&notification) &&
           notification.type == RuntimeNotification::Type::call_finished &&
           notification.error == Error::signaling_failed);

    // Every native context can produce accepted, held, resumed, and finished
    // before an actor poll. The fixed public ring retains all 28 records.
    FakePjsuaApi pressure_fake; pressure_fake.Reset();
    PjsuaAccountManager pressure_accounts(pressure_fake.Api());
    assert(pressure_accounts.Initialize(registry, 4) == Error::ok);
    PjsuaApi pressure_api = pressure_fake.Api(); pressure_api.call_get_info = &StateInfo;
    PjsuaCallManager pressure(pressure_accounts, pressure_api);
    std::uint32_t pressure_tokens[7]{};
    for (unsigned index = 0; index < 7; ++index) {
        pjsua_call_id native = PJSUA_INVALID_ID;
        assert(pressure_api.call_make_call(0, &destination, &setting, nullptr, nullptr, &native) == PJ_SUCCESS);
        state = PJSIP_INV_STATE_CALLING; code = 200; media = PJMEDIA_DIR_NONE;
        pressure.OnIncomingCall(2, native);
        assert(pressure.TryGetNotification(&incoming));
        pressure_tokens[index] = incoming.token;
        assert(pressure.AdmitIncoming({static_cast<std::uint8_t>(index), 2}, incoming.agent,
                                     incoming.token) == Error::ok);
        state = PJSIP_INV_STATE_CONFIRMED; media = PJMEDIA_DIR_ENCODING_DECODING;
        pressure.OnCallState(native);
    }
    for (unsigned index = 0; index < 7; ++index) {
        state = PJSIP_INV_STATE_CONFIRMED; media = PJMEDIA_DIR_ENCODING;
        pressure.OnCallMediaState(static_cast<pjsua_call_id>(index));
    }
    for (unsigned index = 0; index < 7; ++index) {
        state = PJSIP_INV_STATE_CONFIRMED; media = PJMEDIA_DIR_ENCODING_DECODING;
        pressure.OnCallMediaState(static_cast<pjsua_call_id>(index));
    }
    for (unsigned index = 0; index < 7; ++index) {
        state = PJSIP_INV_STATE_DISCONNECTED; media = PJMEDIA_DIR_NONE;
        pressure.OnCallState(static_cast<pjsua_call_id>(index));
    }
    for (unsigned index = 0; index < 7; ++index) {
        assert(pressure.TryGetNotification(&notification));
        assert(notification.type == RuntimeNotification::Type::call_accepted &&
               notification.token == pressure_tokens[index]);
    }
    for (unsigned index = 0; index < 7; ++index) {
        assert(pressure.TryGetNotification(&notification));
        assert(notification.type == RuntimeNotification::Type::call_held &&
               notification.token == pressure_tokens[index]);
    }
    for (unsigned index = 0; index < 7; ++index) {
        assert(pressure.TryGetNotification(&notification));
        assert(notification.type == RuntimeNotification::Type::call_resumed &&
               notification.token == pressure_tokens[index]);
    }
    for (unsigned index = 0; index < 7; ++index) {
        assert(pressure.TryGetNotification(&notification));
        assert(notification.type == RuntimeNotification::Type::call_finished &&
               notification.token == pressure_tokens[index]);
    }
    assert(!pressure.TryGetNotification(&notification));
    printk("PjsuaCallStateTest PASSED\n");
}
} // namespace voip::test
