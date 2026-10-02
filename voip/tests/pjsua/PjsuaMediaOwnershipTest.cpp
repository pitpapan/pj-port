#include "../../src/pjsua/PjsuaRuntimeAdapter.hpp"
#include "FakePjsuaApi.hpp"
#include <cassert>
#include <cstring>
#include <zephyr/sys/printk.h>
namespace voip::test {
namespace {
class Endpoint final : public PcmSource,public PcmSink {
public:
    Error error=Error::ok;
    unsigned flushes=0;
    PcmFormat Format() const noexcept override { return {8000,160,1,SampleFormat::signed_16}; }
    Error Read(std::int16_t *samples,std::size_t n,std::uint64_t) noexcept override {
        for (unsigned i=0;i<n;++i) samples[i]=1234; return error;
    }
    Error Write(const std::int16_t *,std::size_t,std::uint64_t) noexcept override { return Error::ok; }
    void Flush() noexcept override { ++flushes; }
};
static std::array<Endpoint,5> endpoints;
static std::array<AgentConfig,5> agents;
static AgentRegistry registry;
static FakePjsuaApi fake;
static std::array<pjsua_call_info,7> observations;
pj_status_t CallInfo(pjsua_call_id id,pjsua_call_info *info) {
    if (id<0 || id>=7 || !info) return PJ_EINVAL;
    *info=observations[id]; return PJ_SUCCESS;
}
static PjsuaApi api;
static decltype(api.call_hangup) native_hangup;
static bool check_stopped_on_hangup=false;
pj_status_t ShutdownHangup(pjsua_call_id id,unsigned status,const pj_str_t *reason,const pjsua_msg_data *data) {
    if (check_stopped_on_hangup && id==0) {
        std::int16_t samples[160]; pjmedia_frame frame{};
        frame.buf=samples; frame.size=sizeof(samples);
        auto *port=FakePjsuaMediaApi::ports[0].port; assert(port);
        assert(port->get_frame(port,&frame)==PJ_SUCCESS);
        printk("Shutdown native dispatch PCM sample=%d\n",samples[0]);
        assert(samples[0]==0);
    }
    return native_hangup(id,status,reason,data);
}
static PjsuaRuntimeAdapter adapter(api);
unsigned finishes[7]{};
Error finished_errors[7]{};
void Drain() {
    RuntimeNotification notification{};
    while (adapter.TryGetNotification(&notification))
        if (notification.type==RuntimeNotification::Type::call_finished) {
            assert(notification.token<7); ++finishes[notification.token];
            finished_errors[notification.token]=notification.error;
        }
}
void Media(unsigned id) { fake.UaConfig().cb.on_call_media_state(id); }
void State(unsigned id) { fake.UaConfig().cb.on_call_state(id,nullptr); }
}
void RunPjsuaMediaOwnershipTests() {
    fake.Reset(); api=fake.Api(); api.call_get_info=CallInfo;
    native_hangup=api.call_hangup; api.call_hangup=ShutdownHangup;
    check_stopped_on_hangup=false;
    observations={}; std::memset(finishes,0,sizeof(finishes));
    const char *identities[]={"sip:zero@example.test","sip:one@example.test","sip:two@example.test",
                              "sip:three@example.test","sip:four@example.test"};
    for (unsigned i=0;i<5;++i) {
        endpoints[i].error=Error::ok; endpoints[i].flushes=0;
        agents[i]={{identities[i],"sip:registrar@example.test","user","password"},{&endpoints[i],&endpoints[i]},false};
    }
    ServiceConfig config{agents.data(),5,1000,5000,endpoints[0].Format(),{SignalingSecurity::none,MediaSecurity::none}};
    assert(registry.Initialize(config)==Error::ok);
    assert(adapter.Initialize(registry,config.security,config.conference_format)==Error::ok);
    printk("Media ownership: initialized\n");
    AgentHandle zero{},four{},two{};
    assert(registry.GetAgentHandle(0,&zero)==Error::ok && registry.GetAgentHandle(4,&four)==Error::ok);
    assert(registry.GetAgentHandle(2,&two)==Error::ok);
    std::uint32_t a=0,b=0;
    assert(adapter.PromoteOutgoing({0,1},zero,"sip:peer-a@example.test",&a)==Error::ok);
    assert(adapter.PromoteOutgoing({1,1},four,"sip:peer-b@example.test",&b)==Error::ok);
    assert(adapter.Pump(0,0)==Error::ok && adapter.AvailableMediaBridges()==2);
    printk("Media ownership: signalling promoted\n");
    observations[0].id=0; observations[0].state=PJSIP_INV_STATE_CALLING;
    observations[0].media_status=PJSUA_CALL_MEDIA_ACTIVE;
    observations[0].media_dir=PJMEDIA_DIR_ENCODING_DECODING; observations[0].conf_slot=1;
    Media(0); // media before confirmation.
    observations[1].id=1; observations[1].state=PJSIP_INV_STATE_CONFIRMED;
    State(1); // confirmation before media.
    observations[1].media_status=PJSUA_CALL_MEDIA_ACTIVE;
    observations[1].media_dir=PJMEDIA_DIR_ENCODING_DECODING; observations[1].conf_slot=2; Media(1);
    assert(FakePjsuaMediaApi::adds==0); // callbacks only copy observations.
    assert(adapter.Pump(1,0)==Error::ok && adapter.AvailableMediaBridges()==0);
    assert(adapter.Pump(20,0)==Error::ok && FakePjsuaMediaApi::adds==2);
    printk("Media ownership: two attached\n");
    observations[0].state=PJSIP_INV_STATE_CONFIRMED; State(0); Drain();
    observations[2].id=2; observations[2].state=PJSIP_INV_STATE_INCOMING;
    observations[2].media_status=PJSUA_CALL_MEDIA_ACTIVE; observations[2].conf_slot=3;
    fake.DeliverIncomingCall(4,2);
    RuntimeNotification incoming{}; assert(adapter.TryGetNotification(&incoming));
    assert(incoming.type==RuntimeNotification::Type::incoming_call);
    assert(adapter.AdmitIncoming({2,1},two,incoming.token)==Error::ok);
    Media(2); assert(adapter.Pump(21,0)==Error::ok && FakePjsuaMediaApi::adds==2);

    // Fatal endpoint failure is call-local and retains both owners until the
    // affected native call and custom conference port have actually drained.
    endpoints[4].error=Error::media_failed;
    std::int16_t samples[160]; pjmedia_frame frame{}; frame.buf=samples; frame.size=sizeof(samples);
    auto *failed_port=FakePjsuaMediaApi::ports[1].port; assert(failed_port);
    assert(failed_port->get_frame(failed_port,&frame)==PJ_SUCCESS);
    fake.FailCall(FakePjsuaApi::CallEntry::hangup,PJ_EUNKNOWN);
    assert(adapter.Pump(22,0)==Error::ok && fake.HangupCount()==1);
    Drain(); assert(finishes[b]==0 && adapter.AvailableMediaBridges()==0);
    fake.ClearCallFailures();
    assert(adapter.Pump(23,0)==Error::ok);
    printk("Endpoint hangup recovery: attempts=%u\n",fake.HangupCount());
    assert(fake.HangupCount()==2);
    assert(adapter.Pump(24,0)==Error::ok && fake.HangupCount()==2);
    auto *unaffected=FakePjsuaMediaApi::ports[0].port; assert(unaffected);
    assert(unaffected->get_frame(unaffected,&frame)==PJ_SUCCESS && samples[0]==1234);
    observations[1].state=PJSIP_INV_STATE_DISCONNECTED; observations[1].media_status=PJSUA_CALL_MEDIA_NONE;
    observations[1].last_status=PJSIP_SC_OK; State(1); FakePjsuaMediaApi::NativeRemoved(2);
    Drain();
    printk("Native inactive, custom reachable: finishes=%u available=%u\n",finishes[b],adapter.AvailableMediaBridges());
    assert(finishes[b]==0); // native inactivity alone is insufficient.
    assert(adapter.Pump(25,0)==Error::ok); Drain(); assert(finishes[b]==0);
    assert(adapter.Pump(40,0)==Error::ok); Drain();
    printk("After disconnect completion: finishes=%u queued=%u removes=%u available=%u\n",
           finishes[b],FakePjsuaMediaApi::queued,FakePjsuaMediaApi::removes,adapter.AvailableMediaBridges());
    assert(adapter.Pump(60,0)==Error::ok); Drain();
    assert(finishes[b]==1 && finished_errors[b]==Error::media_failed && adapter.AvailableMediaBridges()==1);
    assert(endpoints[4].flushes==1 && endpoints[0].flushes==0);
    assert(unaffected->get_frame(unaffected,&frame)==PJ_SUCCESS && samples[0]==1234);
    check_stopped_on_hangup=true;
    assert(adapter.Shutdown()==Error::busy);
    check_stopped_on_hangup=false;
    observations[0].state=PJSIP_INV_STATE_DISCONNECTED; observations[0].media_status=PJSUA_CALL_MEDIA_NONE;
    observations[0].last_status=PJSIP_SC_OK; State(0); FakePjsuaMediaApi::NativeRemoved(1);
    FakePjsuaMediaApi::fail_remove=true;
    assert(adapter.Pump(61,0)==Error::ok && adapter.Pump(80,0)==Error::ok); Drain();
    assert(finishes[a]==0 && adapter.AvailableMediaBridges()==1);
    assert(adapter.Shutdown()==Error::busy);
    assert(adapter.Pump(3000,0)==Error::ok && adapter.Shutdown()==Error::busy);
    assert(finishes[a]==0 && endpoints[0].flushes==0 && registry.Resolve(zero)!=nullptr);
    FakePjsuaMediaApi::fail_remove=false;
    assert(adapter.Pump(3001,0)==Error::ok && adapter.Pump(3020,0)==Error::ok); Drain();
    assert(finishes[a]==1 && adapter.AvailableMediaBridges()==2);
    observations[2].state=PJSIP_INV_STATE_DISCONNECTED; observations[2].media_status=PJSUA_CALL_MEDIA_NONE;
    observations[2].last_status=PJSIP_SC_REQUEST_TERMINATED; State(2); Drain();
    assert(adapter.Shutdown()==Error::busy);
    assert(adapter.Pump(3021,0)==Error::ok && adapter.Shutdown()==Error::ok);
    assert(adapter.AvailableMediaBridges()==2);
    registry.Reset();
    printk("PjsuaMediaOwnershipTest PASSED\n");
}
}
