#include "../../src/pjsua/PjsuaRuntimeAdapter.hpp"
#include "FakePjsuaApi.hpp"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <zephyr/sys/printk.h>
#undef assert
#define assert(condition) do { if (!(condition)) { printk("Media control check line %u: %s\n",__LINE__,#condition); std::abort(); } } while (false)
namespace voip::test {
namespace {
class Endpoint final : public PcmSource,public PcmSink {
public:
    unsigned flushes=0;
    PcmFormat Format() const noexcept override { return {8000,160,1,SampleFormat::signed_16}; }
    Error Read(std::int16_t *p,std::size_t n,std::uint64_t) noexcept override { std::memset(p,0,n*2); return Error::ok; }
    Error Write(const std::int16_t *,std::size_t,std::uint64_t) noexcept override { return Error::ok; }
    void Flush() noexcept override { ++flushes; }
};
static Endpoint endpoint;
static AgentRegistry registry;
static FakePjsuaApi fake;
static PjsuaApi api;
static PjsuaRuntimeAdapter adapter(api);
static pjsua_call_info info;
static pjsip_transaction transaction;
pj_status_t CallInfo(pjsua_call_id id,pjsua_call_info *out) { assert(id==0 && out); *out=info; return PJ_SUCCESS; }
void Media() { fake.UaConfig().cb.on_call_media_state(0); }
void Empty() { RuntimeNotification n{}; assert(!adapter.TryGetNotification(&n)); }
RuntimeNotification Pop(RuntimeNotification::Type type,Error error=Error::ok) {
    RuntimeNotification n{}; assert(adapter.TryGetNotification(&n));
    assert(n.type==type && n.error==error); return n;
}
}
void RunPjsuaMediaControlReconciliationTests() {
    using F=FakePjsuaMediaApi;
    fake.Reset(); api=fake.Api(); api.call_get_info=CallInfo; endpoint.flushes=0;
    AgentConfig agent{{"sip:control@example.test","sip:registrar@example.test","user","password"},{&endpoint,&endpoint},false};
    ServiceConfig config{&agent,1,1000,5000,endpoint.Format(),{SignalingSecurity::none,MediaSecurity::none}};
    assert(registry.Initialize(config)==Error::ok);
    assert(adapter.Initialize(registry,config.security,config.conference_format)==Error::ok);
    AgentHandle handle{}; assert(registry.GetAgentHandle(0,&handle)==Error::ok);
    std::uint32_t token=0;
    assert(adapter.PromoteOutgoing({0,1},handle,"sip:peer@example.test",&token)==Error::ok);
    info={}; info.id=0; info.state=PJSIP_INV_STATE_CONFIRMED;
    info.media_status=PJSUA_CALL_MEDIA_ACTIVE; info.media_dir=PJMEDIA_DIR_ENCODING_DECODING; info.conf_slot=1;
    fake.UaConfig().cb.on_call_state(0,nullptr); Pop(RuntimeNotification::Type::call_accepted);
    Media(); assert(adapter.Pump(0,0)==Error::ok && adapter.Pump(20,0)==Error::ok); Empty();
    assert(F::connects==2 && adapter.AvailableMediaBridges()==1);

    assert(adapter.SetHeld(token,true)==Error::ok);
    info.media_status=PJSUA_CALL_MEDIA_LOCAL_HOLD; info.media_dir=PJMEDIA_DIR_ENCODING; Media();
    Empty(); // negotiation is not successful conference reconciliation.
    assert(adapter.Pump(21,0)==Error::ok && F::disconnects==1); Empty();
    assert(adapter.Pump(40,0)==Error::ok);
    auto held=Pop(RuntimeNotification::Type::call_held);
    assert(held.media_active && held.media_send_enabled && !held.media_receive_enabled);
    Media(); Empty(); assert(endpoint.flushes==0);

    const Error errors[]={Error::negotiation_failed,Error::remote_rejected,
                         Error::signaling_failed,Error::timed_out,Error::negotiation_failed};
    for (unsigned fault=0;fault<5;++fault) {
        assert(adapter.SetHeld(token,false)==Error::ok);
        transaction={}; transaction.method.id=PJSIP_INVITE_METHOD; transaction.role=PJSIP_ROLE_UAC;
        transaction.cseq=100+fault; transaction.state=PJSIP_TSX_STATE_CALLING;
        fake.UaConfig().cb.on_call_tsx_state(0,&transaction,nullptr);
        transaction.state=PJSIP_TSX_STATE_COMPLETED;
        transaction.status_code=fault==0 ? 488 : fault==1 ? 500 : fault==4 ? 200 : 408;
        transaction.transport_err=fault==2 ? PJ_EEOF : fault==3 ? PJ_ETIMEDOUT : PJ_SUCCESS;
        fake.UaConfig().cb.on_call_tsx_state(0,&transaction,nullptr);
        if (fault==4) { Empty(); fake.UaConfig().cb.on_call_media_update_result(0,PJ_EUNKNOWN); }
        Pop(RuntimeNotification::Type::call_resumed,errors[fault]);
        fake.UaConfig().cb.on_call_tsx_state(0,&transaction,nullptr); Empty();
        assert(F::connects==2 && F::disconnects==1 && endpoint.flushes==0);
    }
    // A late media failure must cancel a staged success before the actor can
    // publish it, even if the old held bridge already matches rollback SDP.
    assert(adapter.SetHeld(token,false)==Error::ok);
    info.media_status=PJSUA_CALL_MEDIA_ACTIVE; info.media_dir=PJMEDIA_DIR_ENCODING_DECODING; Media(); Empty();
    info.media_status=PJSUA_CALL_MEDIA_ERROR; info.media_dir=PJMEDIA_DIR_ENCODING; Media();
    assert(adapter.Pump(40,0)==Error::ok);
    Pop(RuntimeNotification::Type::call_resumed,Error::media_failed); Empty();
    info.media_status=PJSUA_CALL_MEDIA_LOCAL_HOLD; Media();
    assert(adapter.SetHeld(token,false)==Error::ok);
    info.media_status=PJSUA_CALL_MEDIA_REMOTE_HOLD; info.media_dir=PJMEDIA_DIR_DECODING; Media(); Empty();
    assert(adapter.Pump(41,0)==Error::ok && F::connects==3 && endpoint.flushes==1); Empty();
    assert(adapter.Pump(60,0)==Error::ok);
    auto resumed=Pop(RuntimeNotification::Type::call_resumed);
    assert(resumed.media_active && !resumed.media_send_enabled && resumed.media_receive_enabled); Empty();

    // Remote-only changes route audio without inventing a local operation.
    info.media_status=PJSUA_CALL_MEDIA_REMOTE_HOLD; info.media_dir=PJMEDIA_DIR_DECODING; Media(); Empty();
    assert(adapter.Pump(61,0)==Error::ok && F::disconnects==2);
    assert(adapter.Pump(80,0)==Error::ok); Empty();
    Media(); assert(adapter.Pump(81,0)==Error::ok && F::disconnects==2); Empty();
    assert(adapter.SetHeld(token,true)==Error::ok);
    info.media_status=PJSUA_CALL_MEDIA_LOCAL_HOLD; info.media_dir=PJMEDIA_DIR_ENCODING;
    F::fail_connect=4; Media(); assert(adapter.Pump(82,0)==Error::ok);
    assert(fake.HangupCount()==1 && adapter.AvailableMediaBridges()==1);
    Pop(RuntimeNotification::Type::call_held,Error::media_failed); Empty();
    info.state=PJSIP_INV_STATE_DISCONNECTED; info.media_status=PJSUA_CALL_MEDIA_NONE;
    info.last_status=PJSIP_SC_OK; fake.UaConfig().cb.on_call_state(0,nullptr); F::NativeRemoved(1);
    assert(adapter.Pump(83,0)==Error::ok && adapter.Pump(100,0)==Error::ok && adapter.Pump(120,0)==Error::ok);
    Pop(RuntimeNotification::Type::call_finished,Error::media_failed); Empty();
    assert(adapter.AvailableMediaBridges()==2 && endpoint.flushes==2);
    assert(adapter.Shutdown()==Error::busy && adapter.Shutdown()==Error::ok);
    registry.Reset(); printk("PjsuaMediaControlReconciliationTest PASSED\n");
}
}
