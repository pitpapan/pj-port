#include "../../src/pjsua/PjsuaMediaBridge.hpp"
#include "FakePjsuaMediaApi.hpp"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <zephyr/sys/printk.h>
#undef assert
#define assert(condition) do { if (!(condition)) { printk("Direction check line %u: %s\n",__LINE__,#condition); std::abort(); } } while (false)
namespace voip::test {
namespace {
using F=FakePjsuaMediaApi;
class Endpoint final : public PcmSource,public PcmSink {
public:
    unsigned flushes=0;
    PcmFormat Format() const noexcept override { return {8000,160,1,SampleFormat::signed_16}; }
    Error Read(std::int16_t *p,std::size_t n,std::uint64_t) noexcept override { std::memset(p,0,n*2); return Error::ok; }
    Error Write(const std::int16_t *,std::size_t,std::uint64_t) noexcept override { return Error::ok; }
    void Flush() noexcept override { ++flushes; }
};
static Endpoint endpoint;
static bool verify_flush=false;
pj_status_t Connect(int src,int dst) {
    if (verify_flush && dst>=8) assert(endpoint.flushes==1);
    return F::Connect(src,dst);
}
static PjsuaApi api=F::Api();
static PjsuaMediaBridge bridge(api);
void Completed(const pjmedia_conf_op_info *op) { bridge.OnConferenceOperation(*op); }
void Reset() {
    assert(bridge.IsFree()); F::Reset(); F::callback=Completed;
    endpoint.flushes=0; verify_flush=false; api.conf_connect=Connect;
}
Error Attach() { return bridge.Attach({0,1},{4,1},1,0,{&endpoint,&endpoint},endpoint.Format()); }
void Finish() {
    bridge.BeginStop(); bridge.SetNativeInactive();
    for (unsigned i=0;i<8 && !bridge.IsFree();++i) { (void)bridge.PollStop(); F::Complete(); }
    assert(bridge.PollStop()==Error::ok);
}
}
void RunPjsuaMediaDirectionTests() {
    struct Case { pjmedia_dir dir; unsigned connections; bool tx,rx; };
    const Case cases[]={{PJMEDIA_DIR_ENCODING_DECODING,2,true,true},
        {PJMEDIA_DIR_ENCODING,1,true,false},{PJMEDIA_DIR_DECODING,1,false,true},
        {PJMEDIA_DIR_NONE,0,false,false}};
    for (const auto &item : cases) {
        Reset(); assert(bridge.ApplyDirection(item.dir)==Error::ok);
        assert(Attach()==Error::ok && F::connects==item.connections);
        unsigned tx=0,rx=0;
        for (unsigned i=0;i<F::queued;++i) if (F::ops[i].op_type==PJMEDIA_CONF_OP_CONNECT_PORTS) {
            const auto &edge=F::ops[i].op_param.connect_ports;
            if (edge.src==8 && edge.sink==1) ++tx;
            else if (edge.src==1 && edge.sink==8) ++rx;
            else assert(false); // no edge through master slot zero or another call.
        }
        assert(tx==static_cast<unsigned>(item.tx) && rx==static_cast<unsigned>(item.rx));
        F::Complete(); assert(bridge.IsActive());
        assert(bridge.ApplyDirection(item.dir)==Error::ok && F::queued==0);
        assert(bridge.ApplyDirection(static_cast<pjmedia_dir>(4))==Error::invalid_argument && F::queued==0);
        Finish(); assert(endpoint.flushes==1);
    }
    Reset(); assert(Attach()==Error::ok); F::Complete();
    assert(bridge.ApplyDirection(PJMEDIA_DIR_ENCODING)==Error::busy);
    assert(F::disconnects==1 && F::ops[0].op_param.disconnect_ports.src==1 &&
           F::ops[0].op_param.disconnect_ports.sink==8);
    assert(bridge.ApplyDirection(PJMEDIA_DIR_ENCODING)==Error::busy && F::disconnects==1);
    F::Complete(); assert(bridge.ApplyDirection(PJMEDIA_DIR_ENCODING)==Error::ok);
    assert(endpoint.flushes==0);
    verify_flush=true;
    assert(bridge.ApplyDirection(PJMEDIA_DIR_ENCODING_DECODING)==Error::busy);
    assert(F::connects==3 && endpoint.flushes==1);
    F::Complete(); assert(bridge.ApplyDirection(PJMEDIA_DIR_ENCODING_DECODING)==Error::ok);
    verify_flush=false; Finish(); assert(endpoint.flushes==2);

    Reset(); assert(Attach()==Error::ok); F::Complete();
    assert(bridge.ApplyDirection(PJMEDIA_DIR_ENCODING)==Error::busy);
    assert(bridge.ApplyDirection(PJMEDIA_DIR_ENCODING_DECODING)==Error::busy && F::connects==2);
    F::Complete(); assert(bridge.ApplyDirection(PJMEDIA_DIR_ENCODING_DECODING)==Error::busy);
    assert(F::connects==3 && endpoint.flushes==1);
    F::Complete(); assert(bridge.ApplyDirection(PJMEDIA_DIR_ENCODING_DECODING)==Error::ok); Finish();

    Reset(); assert(Attach()==Error::ok); F::Complete();
    assert(bridge.ApplyDirection(PJMEDIA_DIR_ENCODING)==Error::busy); F::Complete();
    F::fail_connect=3;
    assert(bridge.ApplyDirection(PJMEDIA_DIR_ENCODING_DECODING)==Error::media_failed);
    assert(bridge.TakeError()==Error::media_failed && !bridge.IsFree()); Finish();
    printk("PjsuaMediaDirectionTest PASSED\n");
}
}
