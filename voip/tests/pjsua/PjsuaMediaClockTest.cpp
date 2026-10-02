#include "../../src/pjsua/PjsuaMediaClock.hpp"
#include <cassert>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <zephyr/sys/printk.h>
namespace voip::test {
namespace {
alignas(std::int16_t) std::int16_t buffer[1024];
pj_pool_t pool{};
unsigned allocations=0, releases=0, gets=0, puts=0;
bool fail_pool=false, fail_alloc=false, fail_get=false, fail_put=false;
std::uint64_t timestamp=0;
std::size_t expected_bytes=0;
pj_pool_t *Create(const char *,pj_size_t,pj_size_t) { return fail_pool ? nullptr : &pool; }
void *Allocate(pj_pool_t *p,pj_size_t bytes) {
    assert(p==&pool && bytes<=sizeof(buffer)); ++allocations; return fail_alloc ? nullptr : buffer;
}
void Release(pj_pool_t *p) { assert(p==&pool); ++releases; }
pj_status_t Put(pjmedia_port *,pjmedia_frame *frame) {
    ++puts; assert(frame->size==expected_bytes && frame->type==PJMEDIA_FRAME_TYPE_AUDIO);
    for (unsigned i=0; i<expected_bytes/sizeof(std::int16_t); ++i)
        assert(static_cast<std::int16_t *>(frame->buf)[i]==0);
    timestamp=frame->timestamp.u64;
    return fail_put ? PJ_EUNKNOWN : PJ_SUCCESS;
}
pj_status_t Get(pjmedia_port *,pjmedia_frame *frame) {
    ++gets; assert(frame->timestamp.u64==timestamp && frame->size==expected_bytes);
    std::memset(frame->buf,123,frame->size);
    return fail_get ? PJ_EUNKNOWN : PJ_SUCCESS;
}
}
void RunPjsuaMediaClockTests() {
    PjsuaApi api{}; api.pool_create=Create; api.pool_alloc=Allocate; api.pool_release=Release;
    api.port_put_frame=Put; api.port_get_frame=Get;
    PjsuaMediaClock clock(api);
    pjmedia_port master{}; char name[]="master"; const pj_str_t port_name{name,6};
    for (unsigned samples : {160U,80U}) {
        gets=puts=allocations=releases=0;
        const PcmFormat format{8000,static_cast<std::uint16_t>(samples),1,SampleFormat::signed_16};
        const unsigned period=samples/8;
        pjmedia_port_info_init(&master.info,&port_name,0,8000,1,16,samples);
        expected_bytes=samples*sizeof(std::int16_t);
        assert(clock.Initialize(nullptr,format)==Error::invalid_argument);
        fail_pool=true; assert(clock.Initialize(&master,format)==Error::resource_exhausted);
        fail_pool=false; fail_alloc=true;
        assert(clock.Initialize(&master,format)==Error::resource_exhausted && releases==1);
        fail_alloc=false;
        assert(clock.Initialize(&master,format)==Error::ok);
        assert(clock.Initialize(&master,format)==Error::invalid_state);
        assert(clock.Pump(0)==Error::ok && clock.Pump(period-1)==Error::ok && gets==0);
        assert(clock.Pump(period)==Error::ok && gets==1 && puts==1 && timestamp==0);
        assert(clock.Pump(period)==Error::ok && gets==1);
        assert(clock.Pump(2*period)==Error::ok && gets==2 && timestamp==samples);
        assert(clock.Pump(10*period+3)==Error::ok && gets==3 && timestamp==9*samples);
        assert(clock.Skipped()==7 && clock.MaxLatenessMs()==7*period+3 && clock.Ticks()==3);
        assert(clock.Pump(10*period+3)==Error::ok && gets==3);
        clock.Stop(); assert(clock.Pump(1000000)==Error::ok && gets==3);
        assert(releases==2);
        assert(clock.Initialize(&master,format)==Error::ok);
        clock.Pump(std::numeric_limits<std::uint64_t>::max()-period+1);
        clock.Pump(0); assert(gets==4 && timestamp==0); // millisecond counter wrap.
        fail_get=true;
        assert(clock.Pump(period)==Error::media_failed && gets==5);
        fail_get=false; fail_put=true;
        assert(clock.Pump(2*period)==Error::media_failed && gets==5);
        fail_put=false;
        assert(clock.Pump(3*period)==Error::ok && gets==6);
        clock.Stop();
        assert(clock.Initialize(&master,format)==Error::ok);
        clock.Pump(0);
        const auto gap=std::numeric_limits<std::uint64_t>::max();
        const auto periods=gap/period;
        clock.Pump(gap);
        assert(timestamp==(periods-1)*samples); // modular sample counter arithmetic.
        assert(clock.Ticks()==1 && clock.Skipped()==periods-1);
        clock.Stop();
    }
    printk("PjsuaMediaClockTest PASSED\n");
}
}
