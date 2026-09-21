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
    printk("PjsuaCallStateTest PASSED\n");
}
} // namespace voip::test
