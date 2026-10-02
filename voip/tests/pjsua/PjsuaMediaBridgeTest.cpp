#include "../../src/pjsua/PjsuaMediaBridge.hpp"
#include "FakePjsuaMediaApi.hpp"
#include <cassert>
#include <cstring>
#include <atomic>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
namespace voip::test {
namespace {
class BridgeEndpoint final : public PcmSource,public PcmSink {
public:
    unsigned flushes=0;
    std::atomic<bool> block{false}, entered{false}, release{false};
    PcmFormat Format() const noexcept override { return {8000,160,1,SampleFormat::signed_16}; }
    Error Read(std::int16_t *p,std::size_t n,std::uint64_t) noexcept override { std::memset(p,0,n*2); return Error::ok; }
    Error Write(const std::int16_t *,std::size_t,std::uint64_t) noexcept override {
        if (block.load()) { entered.store(true); while (!release.load()) k_yield(); }
        return Error::ok;
    }
    void Flush() noexcept override { ++flushes; }
};
static BridgeEndpoint endpoint;
static PjsuaApi api=FakePjsuaMediaApi::Api();
static PjsuaMediaBridge bridge(api);
K_THREAD_STACK_DEFINE(bridge_callback_stack,4096);
static k_thread bridge_callback_thread;
static std::int16_t frame_samples[160];
static pjmedia_frame blocked_frame;
void WriteFrame(void *,void *,void *) {
    assert(bridge.AudioPort().Port()->put_frame(bridge.AudioPort().Port(),&blocked_frame)==PJ_SUCCESS);
}
void Completed(const pjmedia_conf_op_info *op) { bridge.OnConferenceOperation(*op); }
Error Attach(unsigned generation=1,unsigned agent=0) {
    return bridge.Attach({0,static_cast<std::uint16_t>(generation)},
        {static_cast<std::uint8_t>(agent),static_cast<std::uint16_t>(generation)},generation,0,
        {&endpoint,&endpoint},endpoint.Format());
}
void Reset() { assert(bridge.IsFree()); FakePjsuaMediaApi::Reset(); FakePjsuaMediaApi::callback=Completed; endpoint.flushes=0; }
void Finish() {
    bridge.BeginStop(); bridge.SetNativeInactive();
    for (unsigned i=0;i<8 && !bridge.IsFree();++i) { (void)bridge.PollStop(); FakePjsuaMediaApi::Complete(); }
    assert(bridge.PollStop()==Error::ok && bridge.IsFree());
}
}
void RunPjsuaMediaBridgeTests() {
    using F=FakePjsuaMediaApi;
    Reset();
    assert(Attach()==Error::ok);
    assert(!bridge.IsFree() && !bridge.IsActive() && bridge.CustomSlot()>0 && bridge.CallSlot()==1);
    assert(F::adds==1 && F::connects==2);
    F::Complete(); assert(bridge.IsActive());
    bridge.BeginStop(); bridge.BeginStop();
    assert(bridge.PollStop()==Error::busy && !bridge.IsFree() && endpoint.flushes==0);
    F::Complete(); assert(bridge.PollStop()==Error::busy); // native stream still reachable.
    bridge.SetNativeInactive();
    assert(bridge.PollStop()==Error::busy && F::removes==1 && endpoint.flushes==0);
    assert(bridge.PollStop()==Error::busy && F::removes==1); // enqueue is not removal completion.
    F::Complete(); assert(bridge.PollStop()==Error::ok && bridge.IsFree() && F::released==1);
    assert(endpoint.flushes==1); // shared source/sink is flushed once.
    assert(Attach(2,4)==Error::ok && bridge.Token()==2); F::Complete(); Finish();

    for (unsigned failure=0;failure<5;++failure) {
        Reset();
        if (failure==0) F::fail_pool=true;
        if (failure==1) F::fail_group=true;
        if (failure==2) F::fail_add=true;
        if (failure==3) F::fail_connect=1;
        if (failure==4) F::fail_connect=2;
        assert(Attach()!=Error::ok); F::Complete(); Finish();
        assert(F::queued==0);
    }
    Reset(); F::call_slot=PJSUA_INVALID_ID;
    assert(Attach()==Error::busy && F::connects==0 && !bridge.IsFree());
    F::Complete(); F::call_slot=3;
    assert(Attach()==Error::ok && F::adds==1 && F::connects==2);
    F::Complete(); assert(bridge.IsActive() && bridge.CallSlot()==3);
    bridge.BeginStop(); bridge.SetNativeInactive(); F::fail_disconnect=1;
    assert(bridge.PollStop()!=Error::ok && !bridge.IsFree() && endpoint.flushes==0);
    F::fail_disconnect=0; F::Complete();
    (void)bridge.PollStop(); F::Complete(); F::fail_remove=true;
    assert(bridge.PollStop()!=Error::ok && !bridge.IsFree() && endpoint.flushes==0);
    F::fail_remove=false; Finish();

    Reset(); assert(Attach()==Error::ok);
    F::Complete(PJ_EUNKNOWN);
    assert(bridge.TakeError()==Error::media_failed && !bridge.IsActive()); Finish();

    Reset();
    assert(bridge.Attach({0,1},{0,1},1,0,{nullptr,&endpoint},endpoint.Format())==Error::invalid_argument);
    assert(bridge.IsFree() && F::adds==0 && endpoint.flushes==0);
    F::call_slot=0; assert(Attach()==Error::busy && F::connects==0);
    F::Complete(); F::call_slot=1; assert(Attach()==Error::ok); F::Complete();
    F::call_slot=4; const auto connects=F::connects;
    assert(Attach()==Error::busy && bridge.CallSlot()==1 && F::connects==connects);
    // Native stream replacement removes all its conference edges. A later
    // disconnect would otherwise fail EINVAL against the vanished old slot.
    F::NativeRemoved(1); bridge.BeginStop();
    assert(bridge.PollStop()==Error::busy && !bridge.IsFree());
    bridge.SetNativeInactive(); Finish();
    assert(Attach(3,4)==Error::ok && bridge.CallSlot()==4);
    F::Complete(); Finish();

    Reset(); assert(Attach()==Error::ok); F::Complete();
    blocked_frame={}; blocked_frame.type=PJMEDIA_FRAME_TYPE_AUDIO;
    blocked_frame.buf=frame_samples; blocked_frame.size=sizeof(frame_samples);
    endpoint.block.store(true); endpoint.entered.store(false); endpoint.release.store(false);
    const auto worker=k_thread_create(&bridge_callback_thread,bridge_callback_stack,
        K_THREAD_STACK_SIZEOF(bridge_callback_stack),WriteFrame,nullptr,nullptr,nullptr,
        K_PRIO_PREEMPT(0),0,K_NO_WAIT);
    while (!endpoint.entered.load()) k_yield();
    F::NativeRemoved(1); bridge.BeginStop(); bridge.SetNativeInactive();
    assert(bridge.PollStop()==Error::busy); F::Complete();
    assert(bridge.PollStop()==Error::busy && endpoint.flushes==0 && F::released==0);
    endpoint.release.store(true); assert(k_thread_join(worker,K_SECONDS(1))==0);
    assert(bridge.PollStop()==Error::ok && endpoint.flushes==1 && F::released==1);
    endpoint.block.store(false);

    Reset(); assert(Attach()==Error::ok); F::Complete();
    assert(Attach(2,4)==Error::busy && bridge.Token()==1);
    bridge.BeginStop(); bridge.SetNativeInactive();
    assert(bridge.PollStop()==Error::busy); F::Complete();
    assert(bridge.PollStop()==Error::busy); F::Complete(PJ_EUNKNOWN);
    assert(!bridge.IsFree() && endpoint.flushes==0 && F::released==0);
    assert(bridge.TakeError()==Error::media_failed);
    assert(bridge.PollStop()==Error::busy); F::Complete();
    assert(bridge.PollStop()==Error::ok && endpoint.flushes==1 && F::released==1);
    printk("PjsuaMediaBridgeTest PASSED\n");
}
}
