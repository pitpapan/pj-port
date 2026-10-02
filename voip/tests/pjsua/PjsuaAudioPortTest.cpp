#include "../../src/pjsua/PjsuaAudioPort.hpp"
#include <cassert>
#include <cstring>
#include <initializer_list>
#if defined(VOIP_PCM_HOST_TEST)
#include <thread>
#include <cstdio>
#else
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#endif

namespace voip::test {
namespace {
class Endpoint final : public PcmSource, public PcmSink {
public:
    PcmFormat format{8000,160,1,SampleFormat::signed_16};
    Error read=Error::ok, write=Error::ok;
    std::uint64_t read_time=0, write_time=0;
    unsigned reads=0, writes=0, flushes=0;
    std::atomic<bool> block{false}, entered{false}, release{false};
    PcmFormat Format() const noexcept override { return format; }
    Error Read(std::int16_t *samples, std::size_t count, std::uint64_t time) noexcept override {
        ++reads; read_time=time;
        for (std::size_t i=0;i<count;++i) samples[i]=read==Error::ok ? 1234 : -1234;
        if (block.load()) {
            entered.store(true);
            while (!release.load()) {
#if defined(VOIP_PCM_HOST_TEST)
                std::this_thread::yield();
#else
                k_yield();
#endif
            }
        }
        return read;
    }
    Error Write(const std::int16_t *samples, std::size_t count, std::uint64_t time) noexcept override {
        assert(count==format.samples_per_frame && samples[0]==1234);
        ++writes; write_time=time; return write;
    }
    void Flush() noexcept override { ++flushes; }
};
static Endpoint endpoint;
static PjsuaAudioPort port;
static std::int16_t samples[162];
static pjmedia_frame frame;
#if !defined(VOIP_PCM_HOST_TEST)
K_THREAD_STACK_DEFINE(callback_stack,4096);
static k_thread callback_thread;
void Callback(void *,void *,void *) { assert(port.Port()->get_frame(port.Port(),&frame)==PJ_SUCCESS); }
#endif
}
void RunPjsuaAudioPortTests() {
    for (unsigned count : {80U,160U}) {
        endpoint.format.samples_per_frame=count;
        assert(port.Initialize({nullptr,&endpoint},endpoint.format)==Error::invalid_argument);
        assert(port.Initialize({&endpoint,nullptr},endpoint.format)==Error::invalid_argument);
        auto bad=endpoint.format; bad.channels=2;
        assert(port.Initialize({&endpoint,&endpoint},bad)==Error::invalid_argument);
        bad=endpoint.format; bad.sample_format=static_cast<SampleFormat>(99);
        assert(port.Initialize({&endpoint,&endpoint},bad)==Error::invalid_argument);
        bad=endpoint.format; bad.samples_per_frame--;
        assert(port.Initialize({&endpoint,&endpoint},bad)==Error::invalid_argument);
        bad=endpoint.format; bad.sample_rate_hz=16000;
        assert(port.Initialize({&endpoint,&endpoint},bad)==Error::invalid_argument);
        assert(port.Initialize({&endpoint,&endpoint},endpoint.format)==Error::ok);
        assert(port.Initialize({&endpoint,&endpoint},endpoint.format)==Error::busy);
        std::memset(samples,0,sizeof(samples)); samples[count]=2468;
        frame={}; frame.buf=samples; frame.size=count*sizeof(samples[0]); frame.timestamp.u64=123456;
        assert(port.Port()->get_frame(port.Port(),&frame)==PJ_SUCCESS);
        assert(frame.type==PJMEDIA_FRAME_TYPE_AUDIO && endpoint.read_time==123456);
        assert(samples[0]==1234 && samples[count-1]==1234 && samples[count]==2468);
        assert(port.Port()->put_frame(port.Port(),&frame)==PJ_SUCCESS);
        assert(endpoint.write_time==123456);
        for (auto error : {Error::busy,Error::resource_exhausted,Error::media_failed}) {
            endpoint.read=error;
            assert(port.Port()->get_frame(port.Port(),&frame)==PJ_SUCCESS);
            for (unsigned i=0;i<count;++i) assert(samples[i]==0);
            assert(samples[count]==2468);
            assert(port.TakeError()==(error==Error::media_failed ? error : Error::ok));
        }
        endpoint.read=Error::ok; port.Port()->get_frame(port.Port(),&frame);
        endpoint.write=Error::resource_exhausted;
        assert(port.Port()->put_frame(port.Port(),&frame)==PJ_SUCCESS && port.TakeError()==Error::ok);
        endpoint.write=Error::invalid_state;
        port.Port()->put_frame(port.Port(),&frame);
        endpoint.write=Error::internal_failure;
        port.Port()->put_frame(port.Port(),&frame);
        assert(port.TakeError()==Error::invalid_state && port.TakeError()==Error::ok);
        endpoint.write=Error::ok;
        const auto reads=endpoint.reads, writes=endpoint.writes;
        assert(port.Port()->get_frame(port.Port(),nullptr)==PJ_EINVAL);
        auto invalid=frame; invalid.buf=nullptr;
        assert(port.Port()->get_frame(port.Port(),&invalid)==PJ_EINVAL);
        invalid=frame; invalid.size-=2;
        assert(port.Port()->get_frame(port.Port(),&invalid)==PJ_EINVAL);
        invalid=frame; invalid.type=PJMEDIA_FRAME_TYPE_VIDEO;
        // get_frame's type is output, not a precondition on caller storage.
        assert(port.Port()->get_frame(port.Port(),&invalid)==PJ_SUCCESS);
        assert(invalid.type==PJMEDIA_FRAME_TYPE_AUDIO);
        invalid.type=PJMEDIA_FRAME_TYPE_VIDEO;
        assert(port.Port()->put_frame(port.Port(),&invalid)==PJ_SUCCESS);
        invalid=frame; invalid.buf=nullptr;
        assert(port.Port()->put_frame(port.Port(),&invalid)==PJ_EINVAL);
        assert(endpoint.reads==reads+1 && endpoint.writes==writes);
        port.BeginStop(); assert(port.IsQuiescent());
        assert(port.Port()->get_frame(port.Port(),&frame)==PJ_SUCCESS);
        assert(port.Port()->put_frame(port.Port(),&frame)==PJ_SUCCESS);
        assert(endpoint.reads==reads+1 && endpoint.writes==writes && endpoint.flushes==0);
    }
    assert(port.Initialize({&endpoint,&endpoint},endpoint.format)==Error::ok);
    endpoint.block.store(true);
#if defined(VOIP_PCM_HOST_TEST)
    std::thread thread([] { assert(port.Port()->get_frame(port.Port(),&frame)==PJ_SUCCESS); });
    while (!endpoint.entered.load()) std::this_thread::yield();
#else
    k_thread_create(&callback_thread,callback_stack,K_THREAD_STACK_SIZEOF(callback_stack),
                    Callback,nullptr,nullptr,nullptr,0,0,K_NO_WAIT);
    while (!endpoint.entered.load()) k_yield();
#endif
    port.BeginStop(); assert(!port.IsQuiescent());
    endpoint.release.store(true);
#if defined(VOIP_PCM_HOST_TEST)
    thread.join();
#else
    assert(k_thread_join(&callback_thread,K_SECONDS(1))==0);
#endif
    assert(port.IsQuiescent());
#if defined(VOIP_PCM_HOST_TEST)
    std::puts("PjsuaAudioPortTest PASSED");
#else
    printk("PjsuaAudioPortTest PASSED\n");
#endif
}
}
#if defined(VOIP_PCM_HOST_TEST)
int main() { voip::test::RunPjsuaAudioPortTests(); }
#endif
