#include "../../src/pjsua/PjsuaRuntime.hpp"
#include <pjmedia/conference.h>
#include <cassert>
#include <cstring>
#include <initializer_list>
#include <zephyr/sys/printk.h>
namespace voip::test {
namespace {
unsigned reads=0, writes=0, destroys=0;
std::uint64_t source_time=0, sink_time=0;
pj_status_t Capture(pjmedia_port *,pjmedia_frame *frame) {
    ++reads; source_time=frame->timestamp.u64;
    std::memset(frame->buf,0,frame->size); frame->type=PJMEDIA_FRAME_TYPE_AUDIO;
    return PJ_SUCCESS;
}
pj_status_t Play(pjmedia_port *,pjmedia_frame *frame) {
    if (frame->type==PJMEDIA_FRAME_TYPE_AUDIO) { ++writes; sink_time=frame->timestamp.u64; }
    return PJ_SUCCESS;
}
pj_status_t DestroyPort(pjmedia_port *) { ++destroys; return PJ_SUCCESS; }
}
void RunPjsuaConferenceClockTests() {
    const auto &api=NativePjsuaApi();
    PjsuaRuntime runtime(api); pjsua_callback callbacks{};
    assert(runtime.CreateAndInitialize(callbacks,{8000,160,1,SampleFormat::signed_16})==Error::ok);
    assert(runtime.Start()==Error::ok);
    pj_pool_t *pool=api.pool_create("clock-test",2048,2048); assert(pool!=nullptr);
    static pjmedia_port source{}, sink{};
    source={}; sink={}; reads=writes=destroys=0;
    static char name[]="clock-test"; const pj_str_t port_name{name,10};
    for (auto *port : {&source,&sink}) {
        pjmedia_port_info_init(&port->info,&port_name,0,8000,1,16,160);
        port->get_frame=Capture; port->put_frame=Play; port->on_destroy=DestroyPort;
        assert(pjmedia_port_init_grp_lock(port,pool,nullptr)==PJ_SUCCESS);
    }
    pjsua_conf_port_id a=PJSUA_INVALID_ID,b=PJSUA_INVALID_ID;
    assert(pjsua_conf_add_port(pool,&source,&a)==PJ_SUCCESS);
    assert(pjsua_conf_add_port(pool,&sink,&b)==PJ_SUCCESS);
    assert(a>0 && b>0 && a!=b);
    assert(pjsua_conf_connect(a,b)==PJ_SUCCESS);
    assert(runtime.PumpMedia(0)==Error::ok);
    assert(runtime.PumpMedia(20)==Error::ok && reads==1 && writes==1);
    printk("Conference timestamps: source=%llu sink=%llu\n",
           static_cast<unsigned long long>(source_time),static_cast<unsigned long long>(sink_time));
    assert(source_time==0 && sink_time==0);
    assert(runtime.PumpMedia(40)==Error::ok && reads==2 && writes==2);
    assert(source_time==160 && sink_time==160);
    assert(runtime.PumpMedia(100)==Error::ok && reads==3 && writes==3);
    assert(source_time==640 && sink_time==640);
    assert(pjsua_conf_remove_port(a)==PJ_SUCCESS && pjsua_conf_remove_port(b)==PJ_SUCCESS);
    assert(pjmedia_port_destroy(&source)==PJ_SUCCESS && pjmedia_port_destroy(&sink)==PJ_SUCCESS);
    assert(destroys==0); // remove enqueue is not native quiescence.
    assert(runtime.PumpMedia(120)==Error::ok && destroys==2);
    api.pool_release(pool);
    assert(runtime.Destroy()==Error::ok);
    printk("PjsuaConferenceClockTest PASSED\n");
}
}
