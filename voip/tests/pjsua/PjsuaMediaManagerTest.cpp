#include "../../src/pjsua/PjsuaMediaManager.hpp"
#include "FakePjsuaMediaApi.hpp"
#include <cassert>
#include <zephyr/sys/printk.h>
namespace voip::test {
namespace {
class Endpoint final : public PcmSource,public PcmSink {
public:
    std::int16_t sentinel=0;
    unsigned writes=0,flushes=0;
    Error failure=Error::ok;
    PcmFormat Format() const noexcept override { return {8000,160,1,SampleFormat::signed_16}; }
    Error Read(std::int16_t *samples,std::size_t n,std::uint64_t time) noexcept override {
        assert(n==160 && time==1234); for (unsigned i=0;i<n;++i) samples[i]=sentinel; return failure;
    }
    Error Write(const std::int16_t *samples,std::size_t n,std::uint64_t time) noexcept override {
        assert(n==160 && time==1234 && samples[0]==sentinel && samples[159]==sentinel); ++writes; return Error::ok;
    }
    void Flush() noexcept override { ++flushes; }
};
static std::array<Endpoint,5> endpoints;
static std::array<AgentConfig,5> agents;
static AgentRegistry registry;
static PjsuaApi api=FakePjsuaMediaApi::Api();
static PjsuaMediaManager manager(api);
void Completed(const pjmedia_conf_op_info *op) { manager.OnConferenceOperation(*op); }
void Stop(unsigned token) {
    manager.BeginStop(token); manager.MarkNativeInactive(token);
    for (unsigned i=0;i<10 && !manager.IsQuiescent(token);++i) { manager.Pump(); FakePjsuaMediaApi::Complete(); }
    manager.Pump(); assert(manager.IsQuiescent(token));
}
}
void RunPjsuaMediaManagerTests() {
    using F=FakePjsuaMediaApi;
    F::Reset(); F::callback=Completed;
    const char *identities[]={"sip:zero@example.test","sip:one@example.test","sip:two@example.test",
                              "sip:three@example.test","sip:four@example.test"};
    for (unsigned i=0;i<5;++i) {
        endpoints[i].sentinel=1000+i*900; endpoints[i].writes=endpoints[i].flushes=0; endpoints[i].failure=Error::ok;
        agents[i]={{identities[i],"sip:registrar@example.test","user","password"},
                   {&endpoints[i],&endpoints[i]},false};
    }
    ServiceConfig config{agents.data(),5,1000,5000,endpoints[0].Format(),{SignalingSecurity::none,MediaSecurity::none}};
    assert(registry.Initialize(config)==Error::ok);
    assert(MediaSecurityPolicy::Validate(MediaSecurity::none)==Error::ok);
    assert(MediaSecurityPolicy::Validate(MediaSecurity::srtp_sdes)==Error::unsupported_configuration);
    assert(MediaSecurityPolicy::Validate(static_cast<MediaSecurity>(255))==Error::invalid_argument);
    assert(manager.Initialize(registry,MediaSecurity::srtp_sdes,config.conference_format)==Error::unsupported_configuration);
    assert(F::adds==0 && F::connects==0);
    assert(manager.Initialize(registry,MediaSecurity::none,config.conference_format)==Error::ok);
    AgentHandle zero{},four{},third{};
    assert(registry.GetAgentHandle(0,&zero)==Error::ok && registry.GetAgentHandle(4,&four)==Error::ok);
    assert(registry.GetAgentHandle(2,&third)==Error::ok);
    auto stale=zero; ++stale.generation;
    assert(manager.OnPromoted({0,1},stale,1)==Error::invalid_handle && F::adds==0);
    // Unpromoted callbacks never attach queued calls.
    CopiedMediaState media{0,PJMEDIA_DIR_ENCODING_DECODING,PJSUA_CALL_MEDIA_ACTIVE,1};
    manager.OnMediaState(99,media); manager.Pump(); assert(F::adds==0 && manager.Available()==2);
    assert(manager.OnPromoted({0,1},zero,1)==Error::ok);
    assert(manager.OnPromoted({1,1},four,2)==Error::ok);
    assert(manager.Available()==2); // signalling ownership is not an attached PCM bridge.
    assert(manager.OnPromoted({2,1},third,3)==Error::resource_exhausted);
    manager.OnMediaState(1,media); media.native_id=1; media.slot=2; manager.OnMediaState(2,media);
    manager.Pump(); F::Complete(); manager.Pump();
    assert(manager.Available()==0 && F::adds==2 && F::connects==4);
    manager.OnMediaState(2,media); manager.Pump(); assert(F::adds==2 && F::connects==4);
    std::int16_t samples[160]; pjmedia_frame frame{};
    frame.buf=samples; frame.size=sizeof(samples); frame.timestamp.u64=1234;
    for (unsigned i=0;i<2;++i) {
        auto *port=F::ports[i].port; assert(port);
        assert(port->get_frame(port,&frame)==PJ_SUCCESS && port->put_frame(port,&frame)==PJ_SUCCESS);
    }
    assert(endpoints[0].writes==1 && endpoints[4].writes==1 && endpoints[2].writes==0);
    endpoints[0].failure=endpoints[4].failure=Error::media_failed;
    for (unsigned i=0;i<2;++i) assert(F::ports[i].port->get_frame(F::ports[i].port,&frame)==PJ_SUCCESS);
    manager.Pump();
    unsigned failures=0; std::uint32_t token=0; Error error=Error::ok;
    while (manager.TakeFailure(&token,&error)) { assert((token==1 || token==2) && error==Error::media_failed); ++failures; }
    assert(failures==2 && manager.Available()==0);
    manager.BeginStop(1); assert(!manager.IsQuiescent(1) && manager.Available()==0);
    Stop(1); assert(manager.Available()==1 && endpoints[0].flushes==1 && endpoints[4].flushes==0);
    Stop(2); assert(manager.Available()==2 && endpoints[4].flushes==1);
    assert(manager.Shutdown()==Error::ok);
    registry.Reset(); assert(registry.Initialize(config)==Error::ok);
    assert(manager.Initialize(registry,MediaSecurity::none,config.conference_format)==Error::ok);
    assert(manager.OnPromoted({0,2},zero,3)==Error::invalid_handle); // old registry generation.
    assert(registry.GetAgentHandle(0,&zero)==Error::ok);
    assert(manager.OnPromoted({0,2},zero,3)==Error::ok);
    media.native_id=0; media.slot=1; endpoints[0].failure=Error::ok;
    manager.OnMediaState(3,media); manager.Pump(); F::Complete(); manager.Pump();
    F::call_slot=3; media.slot=3; manager.OnMediaState(3,media); F::NativeRemoved(1);
    for (unsigned i=0;i<4;++i) { manager.Pump(); F::Complete(); }
    assert(manager.Available()==1 && F::ports[0].port!=nullptr);
    Stop(3); assert(manager.Shutdown()==Error::ok); registry.Reset();
    printk("PjsuaMediaManagerTest PASSED\n");
}
}
