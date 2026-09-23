#include "FakePjsuaApi.hpp"
#include "../../src/core/AgentRegistry.hpp"
#include "../../src/pjsua/PjsuaAccountManager.hpp"
#include "../../src/pjsua/PjsuaCallManager.hpp"
#include "../../src/pjsua/PjsuaRuntimeAdapter.hpp"
#include <cassert>
#include <cstring>
#ifdef __ZEPHYR__
#include <zephyr/sys/printk.h>
#else
#include <cstdio>
#define printk std::printf
#endif

namespace voip::test {
namespace {
pjsua_call_info snapshot[7]{};
PjsuaCallManager *nested_manager{};
bool nested_disconnect{};
FakePjsuaApi *nested_fake{};
pj_status_t StateInfo(pjsua_call_id id, pjsua_call_info *info) {
    *info = snapshot[id];
    if (nested_disconnect) {
        nested_disconnect = false;
        snapshot[id].state = PJSIP_INV_STATE_DISCONNECTED;
        snapshot[id].media_status = PJSUA_CALL_MEDIA_NONE;
        nested_manager->OnCallState(id);
        assert(nested_manager->Count() == 1);
        assert(nested_fake->CallUserData(id) != nullptr);
        RuntimeNotification n{}; assert(!nested_manager->TryGetNotification(&n));
    }
    return PJ_SUCCESS;
}
class Source final : public PcmSource { public: PcmFormat Format() const noexcept override { return {8000,160,1,SampleFormat::signed_16}; } Error Read(std::int16_t *,std::size_t,std::uint64_t) noexcept override { return Error::ok; } };
class Sink final : public PcmSink { public: PcmFormat Format() const noexcept override { return {8000,160,1,SampleFormat::signed_16}; } Error Write(const std::int16_t *,std::size_t,std::uint64_t) noexcept override { return Error::ok; } void Flush() noexcept override {} };
struct Fixture {
    Source source; Sink sink;
    AgentConfig agent{{"sip:a@example.test","sip:registrar.example.test","a","p"},{&source,&sink},true};
    ServiceConfig config{&agent,1,1000,5000,{8000,160,1,SampleFormat::signed_16},{SignalingSecurity::none,MediaSecurity::none}};
    AgentRegistry registry;
    FakePjsuaApi fake;
    PjsuaAccountManager accounts{fake.Api()};
    PjsuaApi api{fake.Api()};
    PjsuaCallManager calls{accounts,api};
    AgentHandle agent_handle{};
    char remote[max_uri_length + 20]{};
    char address[max_address_length + 20]{};
    char reason[max_reason_length + 20]{};
    Fixture() {
        fake.Reset(); api.call_get_info = StateInfo;
        assert(registry.Initialize(config) == Error::ok);
        assert(registry.GetAgentHandle(0,&agent_handle) == Error::ok);
        assert(accounts.Initialize(registry,4) == Error::ok);
        std::memset(remote,'u',sizeof(remote));
        std::memset(address,'a',sizeof(address));
        std::memset(reason,'r',sizeof(reason)); reason[1]='\r'; reason[2]='\n'; reason[3]='\x01';
        for (auto &info : snapshot) {
            info = {}; info.state = PJSIP_INV_STATE_CALLING;
            info.remote_info = {remote,static_cast<pj_ssize_t>(sizeof(remote))};
            info.remote_contact = {address,static_cast<pj_ssize_t>(sizeof(address))};
            info.last_status_text = {reason,static_cast<pj_ssize_t>(sizeof(reason))};
        }
    }
    std::uint32_t Outgoing(unsigned index) {
        std::uint32_t token{};
        assert(calls.PromoteOutgoing({static_cast<std::uint8_t>(index),1},agent_handle,"sip:r@example.test",&token) == Error::ok);
        return token;
    }
    void State(unsigned id, pjsip_inv_state state, unsigned code=200, bool active=false) {
        snapshot[id].state=state; snapshot[id].last_status=static_cast<pjsip_status_code>(code);
        snapshot[id].media_status=active ? PJSUA_CALL_MEDIA_ACTIVE : PJSUA_CALL_MEDIA_NONE;
        calls.OnCallState(id);
    }
    void Media(unsigned id,pjmedia_dir direction) {
        snapshot[id].media_status=direction==PJMEDIA_DIR_NONE ? PJSUA_CALL_MEDIA_NONE : PJSUA_CALL_MEDIA_ACTIVE;
        snapshot[id].media_dir=direction; calls.OnCallMediaState(id);
    }
    RuntimeNotification Pop(RuntimeNotification::Type type,std::uint32_t token) {
        RuntimeNotification n{}; assert(calls.TryGetNotification(&n));
        assert(n.type==type && n.token==token); return n;
    }
    void Empty() { RuntimeNotification n{}; assert(!calls.TryGetNotification(&n)); }
};
void UnsolicitedMediaCannotOverflow() {
    Fixture f; const auto token=f.Outgoing(0);
    f.State(0,PJSIP_INV_STATE_CONFIRMED);
    for (unsigned i=0;i<100;++i) { f.Media(0,PJMEDIA_DIR_ENCODING); f.Media(0,PJMEDIA_DIR_ENCODING_DECODING); }
    f.State(0,PJSIP_INV_STATE_DISCONNECTED);
    f.Pop(RuntimeNotification::Type::call_accepted,token);
    f.Pop(RuntimeNotification::Type::call_finished,token);
    f.Empty(); assert(f.calls.Count()==0 && f.fake.CallUserData(0)==nullptr);
}
void LifecycleAndControlMatrix() {
    Fixture f; const auto token=f.Outgoing(0);
    f.State(0,PJSIP_INV_STATE_CALLING); f.State(0,PJSIP_INV_STATE_EARLY); f.Empty();
    f.State(0,PJSIP_INV_STATE_CONFIRMED); f.Pop(RuntimeNotification::Type::call_accepted,token);
    f.State(0,PJSIP_INV_STATE_EARLY); f.State(0,PJSIP_INV_STATE_CALLING);
    f.State(0,PJSIP_INV_STATE_CONFIRMED); f.Empty();
    for (bool held : {true,false}) {
        const auto entry=held ? FakePjsuaApi::CallEntry::set_hold2 : FakePjsuaApi::CallEntry::reinvite2;
        f.fake.FailCall(entry,PJ_EUNKNOWN);
        assert(f.calls.SetHeld(token,held)==Error::signaling_failed); f.Empty();
        f.fake.ClearCallFailures(); assert(f.calls.SetHeld(token,held)==Error::ok);
        assert(f.calls.SetHeld(token,held)==Error::busy);
        snapshot[0].media_status=PJSUA_CALL_MEDIA_ERROR;
        f.calls.OnCallMediaState(0);
        auto failure=f.Pop(held ? RuntimeNotification::Type::call_held : RuntimeNotification::Type::call_resumed,token);
        assert(failure.error==Error::media_failed &&
               failure.status.error==Error::media_failed &&
               !failure.media_active); f.Empty();
        assert(f.calls.SetHeld(token,held)==Error::ok);
        f.Media(0,PJMEDIA_DIR_NONE); f.Empty();
        f.Media(0,held ? PJMEDIA_DIR_ENCODING : PJMEDIA_DIR_ENCODING_DECODING);
        f.Media(0,held ? PJMEDIA_DIR_ENCODING : PJMEDIA_DIR_ENCODING_DECODING);
        assert(f.calls.SetHeld(token,!held)==Error::busy);
        const auto media=f.Pop(held ? RuntimeNotification::Type::call_held : RuntimeNotification::Type::call_resumed,token);
        assert(media.media_active && media.media_send_enabled &&
               (media.media_receive_enabled != held)); f.Empty();
    }
    f.State(0,PJSIP_INV_STATE_DISCONNECTED,603,true);
    f.Empty(); assert(f.calls.Count()==1 && f.fake.CallUserData(0)!=nullptr);
    f.State(0,PJSIP_INV_STATE_CONFIRMED,200,true); f.Empty();
    f.Media(0,PJMEDIA_DIR_NONE);
    auto n=f.Pop(RuntimeNotification::Type::call_finished,token); assert(n.sip_status==603);
    f.State(0,PJSIP_INV_STATE_DISCONNECTED); f.Media(0,PJMEDIA_DIR_ENCODING); f.Empty();
}
void RejectionsAndCopiedPressure() {
    Fixture f; std::uint32_t tokens[7]{};
    const unsigned codes[]={302,403,487,488,503,603,0};
    const Error errors[]={Error::remote_rejected,Error::remote_rejected,
        Error::cancelled,Error::negotiation_failed,Error::remote_rejected,
        Error::remote_rejected,Error::signaling_failed};
    for (unsigned i=0;i<7;++i) tokens[i]=f.Outgoing(i);
    for (unsigned i=0;i<7;++i) {
        f.State(i,PJSIP_INV_STATE_DISCONNECTED,codes[i],true);
        f.State(i,PJSIP_INV_STATE_DISCONNECTED,200,true);
    }
    assert(f.calls.Count()==7);
    for (unsigned i=0;i<7;++i) assert(f.fake.CallUserData(i)!=nullptr);
    std::memset(f.remote,'x',sizeof(f.remote));
    std::memset(f.address,'x',sizeof(f.address));
    std::memset(f.reason,'x',sizeof(f.reason));
    for (unsigned i=0;i<7;++i) f.Media(i,PJMEDIA_DIR_NONE);
    assert(f.calls.Count()==7);
    for (unsigned i=0;i<7;++i) assert(f.fake.CallUserData(i)==nullptr);
    for (unsigned i=0;i<6;++i) {
        const auto n=f.Pop(RuntimeNotification::Type::call_rejected,tokens[i]);
        assert(n.error==errors[i] && n.sip_status==codes[i] &&
               n.status.error==errors[i] &&
               n.status.sip_status==codes[i]);
        assert(n.remote_uri[0]=='u' && n.remote_uri[max_uri_length]=='\0');
        assert(n.remote_address[0]=='a' && n.remote_address[max_address_length]=='\0');
        assert(n.status.reason[0]=='r' && n.status.reason[1]==' ' && n.status.reason[2]==' ' && n.status.reason[3]==' ');
        assert(n.status.reason[max_reason_length]=='\0');
    }
    for (unsigned i=0;i<7;++i) {
        auto n=f.Pop(RuntimeNotification::Type::call_finished,tokens[i]);
        assert(n.sip_status==codes[i]);
        assert(n.error==errors[i]);
        assert(n.status.error==n.error && n.status.sip_status==codes[i]);
        assert(n.remote_uri[0]=='u' && n.remote_address[0]=='a' && n.status.reason[0]=='r');
    }
    f.Empty(); assert(f.calls.Count()==0);
}
void OnePendingControlBoundsStorage() {
    Fixture f; std::uint32_t tokens[7]{};
    for (unsigned i=0;i<7;++i) { tokens[i]=f.Outgoing(i); f.State(i,PJSIP_INV_STATE_CONFIRMED); }
    for (unsigned i=0;i<7;++i) { assert(f.calls.SetHeld(tokens[i],true)==Error::ok); f.Media(i,PJMEDIA_DIR_ENCODING); }
    for (unsigned repeat=0;repeat<100;++repeat)
        for (unsigned i=0;i<7;++i) {
            assert(f.calls.SetHeld(tokens[i],false)==Error::busy);
            f.Media(i,PJMEDIA_DIR_ENCODING_DECODING); f.Media(i,PJMEDIA_DIR_ENCODING);
        }
    for (unsigned i=0;i<7;++i) f.State(i,PJSIP_INV_STATE_DISCONNECTED);
    for (unsigned i=0;i<7;++i) f.Pop(RuntimeNotification::Type::call_accepted,tokens[i]);
    for (unsigned i=0;i<7;++i) f.Pop(RuntimeNotification::Type::call_held,tokens[i]);
    for (unsigned i=0;i<7;++i) f.Pop(RuntimeNotification::Type::call_finished,tokens[i]);
    f.Empty(); assert(f.calls.Count()==0);
}
void ReentrantDisconnectRetainsLease() {
    Fixture f; const auto token=f.Outgoing(0);
    f.State(0,PJSIP_INV_STATE_CONFIRMED); f.Pop(RuntimeNotification::Type::call_accepted,token);
    nested_manager=&f.calls; nested_fake=&f.fake; nested_disconnect=true;
    f.Media(0,PJMEDIA_DIR_ENCODING_DECODING);
    assert(f.calls.Count()==1 && f.fake.CallUserData(0)!=nullptr); f.Empty();
    f.Media(0,PJMEDIA_DIR_NONE); f.Pop(RuntimeNotification::Type::call_finished,token);
    assert(f.calls.Count()==0 && f.fake.CallUserData(0)==nullptr); f.Empty();
}
void IncomingCancelAndConfirmation() {
    Fixture f;
    pjsua_call_setting setting{}; f.api.call_setting_default(&setting);
    char uri[]="sip:incoming@example.test"; pj_str_t destination{uri,sizeof(uri)-1};
    pjsua_call_id id{};
    assert(f.api.call_make_call(2,&destination,&setting,nullptr,nullptr,&id)==PJ_SUCCESS);
    f.calls.OnIncomingCall(2,id);
    RuntimeNotification incoming{}; assert(f.calls.TryGetNotification(&incoming));
    assert(incoming.type==RuntimeNotification::Type::incoming_call);
    assert(f.calls.AdmitIncoming({1,1},f.agent_handle,incoming.token)==Error::ok);
    f.State(id,PJSIP_INV_STATE_DISCONNECTED,487,true);
    assert(f.calls.PromoteIncoming({1,1},f.agent_handle,incoming.token)==Error::invalid_handle);
    assert(f.calls.Answer(incoming.token)==Error::invalid_handle);
    f.Pop(RuntimeNotification::Type::call_rejected,incoming.token); f.Empty();
    f.Media(id,PJMEDIA_DIR_NONE); f.Pop(RuntimeNotification::Type::call_finished,incoming.token);
    f.calls.OnIncomingCall(2,id); assert(f.calls.TryGetNotification(&incoming));
    assert(f.calls.AdmitIncoming({1,2},f.agent_handle,incoming.token)==Error::ok);
    assert(f.calls.Answer(incoming.token)==Error::ok);
    f.State(id,PJSIP_INV_STATE_CONFIRMED);
    f.Pop(RuntimeNotification::Type::call_accepted,incoming.token);
    f.State(id,PJSIP_INV_STATE_DISCONNECTED); f.Pop(RuntimeNotification::Type::call_finished,incoming.token);
    f.Empty();
}
void FinishOnlyIncomingFailuresPreserveStatus() {
    const Error errors[]={Error::signaling_failed,Error::media_failed};
    const unsigned statuses[]={0,200};
    const pjsua_call_media_status media[]={PJSUA_CALL_MEDIA_NONE,
                                           PJSUA_CALL_MEDIA_ERROR};
    for (unsigned i=0;i<2;++i) {
        Fixture f;
        pjsua_call_setting setting{}; f.api.call_setting_default(&setting);
        char uri[]="sip:finish-only@example.test";
        pj_str_t destination{uri,sizeof(uri)-1};
        pjsua_call_id id{};
        assert(f.api.call_make_call(2,&destination,&setting,nullptr,nullptr,&id)==PJ_SUCCESS);
        f.calls.OnIncomingCall(2,id);
        RuntimeNotification incoming{};
        assert(f.calls.TryGetNotification(&incoming));
        assert(incoming.type==RuntimeNotification::Type::incoming_call);
        assert(f.calls.AdmitIncoming({1,1},f.agent_handle,incoming.token)==Error::ok);
        snapshot[id].state=PJSIP_INV_STATE_DISCONNECTED;
        snapshot[id].last_status=static_cast<pjsip_status_code>(statuses[i]);
        snapshot[id].media_status=media[i];
        f.calls.OnCallState(id);
        const auto finish=f.Pop(RuntimeNotification::Type::call_finished,incoming.token);
        assert(finish.error==errors[i] && finish.status.error==errors[i]);
        assert(finish.sip_status==statuses[i] &&
               finish.status.sip_status==statuses[i]);
        f.Empty();
        assert(f.calls.Count()==0 && f.fake.CallUserData(id)==nullptr);
    }
}
void LiveShutdownRetainsStorageAndOrdersCleanup() {
    Fixture f;
    // The adapter composes the real managers; fake only the native boundary.
    f.fake.Reset();
    PjsuaRuntimeAdapter adapter(f.api);
    assert(adapter.Initialize(f.registry,f.config.security,f.config.conference_format)==Error::ok);
    std::uint32_t token{};
    assert(adapter.PromoteOutgoing({0,1},f.agent_handle,"sip:r@example.test",&token)==Error::ok);
    const auto registrations=f.fake.RegistrationCount();
    assert(adapter.Shutdown()==Error::busy);
    assert(adapter.Pump(99999999,0)==Error::ok);
    assert(adapter.Shutdown()==Error::busy);
    assert(f.fake.CallUserData(0)!=nullptr && f.fake.AccountClearCount()==0);
    assert(f.fake.RegistrationCount()==registrations);
    std::uint32_t rejected{};
    assert(adapter.PromoteOutgoing({1,1},f.agent_handle,"sip:r@example.test",&rejected)==Error::shutting_down);
    snapshot[0].state=PJSIP_INV_STATE_DISCONNECTED;
    snapshot[0].media_status=PJSUA_CALL_MEDIA_ACTIVE;
    f.fake.UaConfig().cb.on_call_state(0,nullptr);
    RuntimeNotification n{}; assert(!adapter.TryGetNotification(&n));
    assert(adapter.Shutdown()==Error::busy && f.fake.CallUserData(0)!=nullptr);
    snapshot[0].media_status=PJSUA_CALL_MEDIA_NONE;
    f.fake.UaConfig().cb.on_call_media_state(0);
    assert(adapter.TryGetNotification(&n) && n.type==RuntimeNotification::Type::call_finished && n.token==token);
    assert(f.fake.CallUserData(0)==nullptr);
    f.fake.SetUnregistrationCallbacksDeferred(true);
    assert(adapter.Shutdown()==Error::busy);
    assert(f.fake.RegistrationCount()==registrations+1 && f.fake.AccountClearCount()==0);
    f.fake.DeliverUnregistrationCallbacks();
    assert(adapter.Pump(100000000,0)==Error::ok);
    assert(adapter.Shutdown()==Error::ok);
    assert(f.fake.TeardownSequenceEquals("clear-call,clear,del,close,destroy,reset"));
}
}
void RunPjsuaCallStateTests() {
    UnsolicitedMediaCannotOverflow(); LifecycleAndControlMatrix();
    RejectionsAndCopiedPressure(); OnePendingControlBoundsStorage();
    ReentrantDisconnectRetainsLease();
    IncomingCancelAndConfirmation(); FinishOnlyIncomingFailuresPreserveStatus();
    LiveShutdownRetainsStorageAndOrdersCleanup();
    printk("PjsuaCallStateTest PASSED\n");
}
} // namespace voip::test
#ifdef VOIP_CALL_STATE_HOST_TEST
int main() { voip::test::RunPjsuaCallStateTests(); }
#endif
