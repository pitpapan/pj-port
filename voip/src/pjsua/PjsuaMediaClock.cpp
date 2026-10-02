#include "PjsuaMediaClock.hpp"
#include <cstring>
#include <limits>
namespace voip {
Error PjsuaMediaClock::Initialize(pjmedia_port *master, PcmFormat format) noexcept {
    if (master_!=nullptr || pool_!=nullptr) return Error::invalid_state;
    if (master==nullptr || format.sample_rate_hz==0 || format.samples_per_frame==0 ||
        format.channels!=1 || format.sample_format!=SampleFormat::signed_16 ||
        (static_cast<std::uint32_t>(format.samples_per_frame)*1000U)%format.sample_rate_hz!=0 ||
        master->info.fmt.type!=PJMEDIA_TYPE_AUDIO ||
        PJMEDIA_PIA_SRATE(&master->info)!=format.sample_rate_hz ||
        PJMEDIA_PIA_CCNT(&master->info)!=1 || PJMEDIA_PIA_BITS(&master->info)!=16 ||
        PJMEDIA_PIA_SPF(&master->info)!=format.samples_per_frame) return Error::invalid_argument;
    if (api_.pool_create==nullptr || api_.pool_alloc==nullptr || api_.pool_release==nullptr ||
        api_.port_get_frame==nullptr || api_.port_put_frame==nullptr) return Error::unsupported_configuration;
    const auto count=static_cast<std::size_t>(format.samples_per_frame);
    if (count>(std::numeric_limits<std::size_t>::max()-1024)/sizeof(std::int16_t))
        return Error::invalid_argument;
    bytes_=count*sizeof(std::int16_t);
    pool_=api_.pool_create("voip-clock",bytes_+1024,512);
    if (pool_==nullptr) return Error::resource_exhausted;
    buffer_=api_.pool_alloc(pool_,bytes_);
    if (buffer_==nullptr) { Stop(); return Error::resource_exhausted; }
    master_=master;
    samples_=format.samples_per_frame;
    period_ms_=(static_cast<std::uint32_t>(samples_)*1000U)/format.sample_rate_hz;
    anchor_=timestamp_=ticks_=skipped_=max_lateness_=0; armed_=false;
    return Error::ok;
}
Error PjsuaMediaClock::Pump(std::uint64_t now_ms) noexcept {
    if (master_==nullptr) return Error::ok;
    if (!armed_) { anchor_=now_ms; armed_=true; return Error::ok; }
    const auto elapsed=now_ms-anchor_; // modular milliseconds, including wrap.
    if (elapsed<period_ms_) return Error::ok;
    const auto periods=elapsed/period_ms_;
    const auto lateness=elapsed-period_ms_;
    if (lateness>max_lateness_) max_lateness_=lateness;
    skipped_+=periods-1;
    timestamp_+=(periods-1)*samples_;
    anchor_+=periods*period_ms_;
    std::memset(buffer_,0,bytes_);
    pjmedia_frame frame{};
    frame.type=PJMEDIA_FRAME_TYPE_AUDIO; frame.buf=buffer_; frame.size=bytes_;
    frame.timestamp.u64=timestamp_;
    timestamp_+=samples_; ++ticks_;
    // Input and output use only the master timing interface. Application
    // sources/sinks connect directly to their own native call slots.
    if (api_.port_put_frame(master_,&frame)!=PJ_SUCCESS) return Error::media_failed;
    frame.type=PJMEDIA_FRAME_TYPE_AUDIO; frame.size=bytes_;
    return api_.port_get_frame(master_,&frame)==PJ_SUCCESS ? Error::ok : Error::media_failed;
}
void PjsuaMediaClock::Stop() noexcept {
    master_=nullptr; buffer_=nullptr; armed_=false;
    if (pool_!=nullptr) { api_.pool_release(pool_); pool_=nullptr; }
}
}
