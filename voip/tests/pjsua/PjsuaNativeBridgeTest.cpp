#include "../../src/pjsua/PjsuaRuntime.hpp"
#include "../../src/pjsua/PjsuaMediaBridge.hpp"
#include <cassert>
#include <zephyr/sys/printk.h>
namespace voip::test {
namespace {
int native_slot=PJSUA_INVALID_ID;
unsigned native_destroyed=0, sent=0;
PjsuaMediaBridge *observed_bridge=nullptr;
int Slot(pjsua_call_id) { return native_slot; }
void Completed(const pjmedia_conf_op_info *op) { if (observed_bridge) observed_bridge->OnConferenceOperation(*op); }
pj_status_t Capture(pjmedia_port *,pjmedia_frame *frame) {
    auto *samples=static_cast<std::int16_t *>(frame->buf);
    for (unsigned i=0;i<160;++i) samples[i]=-4321;
    frame->type=PJMEDIA_FRAME_TYPE_AUDIO; return PJ_SUCCESS;
}
pj_status_t Play(pjmedia_port *,pjmedia_frame *frame) {
    if (frame->type==PJMEDIA_FRAME_TYPE_AUDIO) {
        assert(frame->size==320 && static_cast<std::int16_t *>(frame->buf)[0]==7777); ++sent;
    }
    return PJ_SUCCESS;
}
pj_status_t Destroy(pjmedia_port *) { ++native_destroyed; return PJ_SUCCESS; }
class Endpoint final : public PcmSource,public PcmSink {
public:
    unsigned reads=0, writes=0, flushes=0;
    PcmFormat Format() const noexcept override { return {8000,160,1,SampleFormat::signed_16}; }
    Error Read(std::int16_t *samples,std::size_t n,std::uint64_t time) noexcept override {
        assert(n==160 && time==0); ++reads;
        for (unsigned i=0;i<n;++i) samples[i]=7777;
        return Error::ok;
    }
    Error Write(const std::int16_t *samples,std::size_t n,std::uint64_t time) noexcept override {
        assert(n==160 && samples[0]==-4321 && time==0); ++writes; return Error::ok;
    }
    void Flush() noexcept override { ++flushes; }
};
static Endpoint endpoint;
static pjmedia_port native_port;
static PjsuaApi api;
static PjsuaMediaBridge bridge(api);
}
void RunPjsuaNativeBridgeTests() {
    api=NativePjsuaApi(); api.call_get_conf_port=Slot;
    PjsuaRuntime runtime(api); pjsua_callback callbacks{};
    callbacks.on_conf_op_completed=Completed; observed_bridge=&bridge;
    assert(runtime.CreateAndInitialize(callbacks,endpoint.Format())==Error::ok);
    assert(runtime.Start()==Error::ok);
    auto *pool=api.pool_create("native-bridge-test",2048,2048); assert(pool);
    native_port={}; endpoint.reads=endpoint.writes=endpoint.flushes=native_destroyed=sent=0;
    static char name[]="synthetic-call"; const pj_str_t port_name{name,14};
    assert(pjmedia_port_info_init(&native_port.info,&port_name,0,8000,1,16,160)==PJ_SUCCESS);
    native_port.get_frame=Capture; native_port.put_frame=Play; native_port.on_destroy=Destroy;
    assert(api.port_init_grp_lock(&native_port,pool,nullptr)==PJ_SUCCESS);
    assert(api.conf_add_port(pool,&native_port,&native_slot)==PJ_SUCCESS && native_slot>0);
    assert(bridge.Attach({0,1},{4,1},10,0,{&endpoint,&endpoint},endpoint.Format())==Error::ok);
    assert(!bridge.IsActive());
    assert(runtime.PumpMedia(0)==Error::ok && runtime.PumpMedia(20)==Error::ok);
    assert(bridge.IsActive() && endpoint.reads==1 && endpoint.writes==1 && sent==1);
    // Exercise real native removal before custom cleanup, as disconnected
    // call teardown can remove the stream before the actor sees its callback.
    assert(api.conf_remove_port(native_slot)==PJ_SUCCESS);
    assert(api.port_destroy(&native_port)==PJ_SUCCESS && native_destroyed==0);
    assert(runtime.PumpMedia(40)==Error::ok && native_destroyed==1);
    assert(!bridge.IsActive());
    bridge.BeginStop(); bridge.SetNativeInactive();
    assert(bridge.PollStop()==Error::busy && endpoint.flushes==0);
    assert(runtime.PumpMedia(60)==Error::ok);
    assert(bridge.PollStop()==Error::ok && bridge.IsFree() && endpoint.flushes==1);
    assert(runtime.PumpMedia(80)==Error::ok && endpoint.reads==1 && endpoint.writes==1);
    api.pool_release(pool);
    assert(runtime.Destroy()==Error::ok); observed_bridge=nullptr;
    printk("PjsuaNativeBridgeTest PASSED\n");
}
}
