#include "PjsuaAudioPort.hpp"
#include <cstring>
#include <cstdint>
namespace voip {
namespace {
bool SameFormat(PcmFormat a, PcmFormat b) noexcept {
    return a.sample_rate_hz==b.sample_rate_hz && a.samples_per_frame==b.samples_per_frame &&
           a.channels==b.channels && a.sample_format==b.sample_format;
}
struct Entry {
    std::atomic<unsigned> &count;
    explicit Entry(std::atomic<unsigned> &value) noexcept : count(value) {
        count.fetch_add(1,std::memory_order_acq_rel);
    }
    ~Entry() noexcept { count.fetch_sub(1,std::memory_order_release); }
};
bool Aligned(const void *buffer) noexcept {
    return buffer!=nullptr && reinterpret_cast<std::uintptr_t>(buffer)%alignof(std::int16_t)==0;
}
}
Error PjsuaAudioPort::Initialize(AgentAudioBinding binding, PcmFormat format) noexcept {
    if (!IsQuiescent()) return Error::busy;
    if (binding.source==nullptr || binding.sink==nullptr || format.sample_rate_hz==0 ||
        format.samples_per_frame==0 || format.channels!=1 || format.sample_format!=SampleFormat::signed_16 ||
        (static_cast<std::uint32_t>(format.samples_per_frame)*1000U)%format.sample_rate_hz!=0 ||
        !SameFormat(format,binding.source->Format()) || !SameFormat(format,binding.sink->Format()))
        return Error::invalid_argument;
    port_={};
    static char name[]="agent-pcm";
    const pj_str_t port_name{name,sizeof(name)-1};
    if (pjmedia_port_info_init(&port_.info,&port_name,PJMEDIA_SIG_CLASS_PORT_AUD('A','G'),
        format.sample_rate_hz,1,16,format.samples_per_frame)!=PJ_SUCCESS) return Error::media_failed;
    binding_=binding; format_=format;
    bytes_=static_cast<std::size_t>(format.samples_per_frame)*sizeof(std::int16_t);
    port_.port_data.pdata=this;
    port_.get_frame=Get; port_.put_frame=Put;
    error_.store(Error::ok,std::memory_order_relaxed);
    stopping_.store(false,std::memory_order_release);
    return Error::ok;
}
void PjsuaAudioPort::Latch(Error error) noexcept {
    if (error==Error::ok || error==Error::busy || error==Error::resource_exhausted) return;
    Error expected=Error::ok;
    (void)error_.compare_exchange_strong(expected,error,std::memory_order_acq_rel);
}
pj_status_t PjsuaAudioPort::Get(pjmedia_port *native, pjmedia_frame *frame) noexcept {
    if (native==nullptr || native->port_data.pdata==nullptr) return PJ_EINVAL;
    auto &self=*static_cast<PjsuaAudioPort *>(native->port_data.pdata);
    Entry entry(self.entries_);
    if (frame==nullptr || !Aligned(frame->buf) || frame->size!=self.bytes_ ||
        (frame->type!=PJMEDIA_FRAME_TYPE_NONE && frame->type!=PJMEDIA_FRAME_TYPE_AUDIO)) return PJ_EINVAL;
    Error result=Error::busy;
    if (!self.stopping_.load(std::memory_order_acquire))
        result=self.binding_.source->Read(static_cast<std::int16_t *>(frame->buf),
                                          self.format_.samples_per_frame,frame->timestamp.u64);
    if (result!=Error::ok) std::memset(frame->buf,0,self.bytes_);
    self.Latch(result);
    frame->type=PJMEDIA_FRAME_TYPE_AUDIO;
    return PJ_SUCCESS;
}
pj_status_t PjsuaAudioPort::Put(pjmedia_port *native, pjmedia_frame *frame) noexcept {
    if (native==nullptr || native->port_data.pdata==nullptr) return PJ_EINVAL;
    auto &self=*static_cast<PjsuaAudioPort *>(native->port_data.pdata);
    Entry entry(self.entries_);
    if (frame==nullptr) return PJ_EINVAL;
    if (self.stopping_.load(std::memory_order_acquire) || frame->type!=PJMEDIA_FRAME_TYPE_AUDIO)
        return PJ_SUCCESS;
    if (!Aligned(frame->buf) || frame->size!=self.bytes_) return PJ_EINVAL;
    self.Latch(self.binding_.sink->Write(static_cast<const std::int16_t *>(frame->buf),
                                        self.format_.samples_per_frame,frame->timestamp.u64));
    return PJ_SUCCESS;
}
}
