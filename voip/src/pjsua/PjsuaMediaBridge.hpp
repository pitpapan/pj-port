#ifndef VOIP_PJSUA_MEDIA_BRIDGE_HPP
#define VOIP_PJSUA_MEDIA_BRIDGE_HPP
#include "PjsuaApi.hpp"
#include "PjsuaAudioPort.hpp"
#include <pjmedia/conference.h>
namespace voip {
class PjsuaMediaBridge final {
public:
    explicit PjsuaMediaBridge(const PjsuaApi &api=NativePjsuaApi()) noexcept : api_(api) {}
    Error Attach(CallHandle, AgentHandle, std::uint32_t, pjsua_call_id,
                 AgentAudioBinding, PcmFormat) noexcept;
    void BeginStop() noexcept;
    Error PollStop() noexcept;
    bool IsFree() const noexcept;
    bool IsActive() const noexcept;
    bool NativeSlotRemoved() const noexcept { return native_slot_removed_; }
    void SetNativeInactive() noexcept;
    void OnConferenceOperation(const pjmedia_conf_op_info &) noexcept;
    Error TakeError() noexcept;
    std::uint32_t Token() const noexcept;
    pjsua_conf_port_id CustomSlot() const noexcept;
    pjsua_conf_port_id CallSlot() const noexcept;
    PjsuaAudioPort &AudioPort() noexcept { return audio_; }
private:
    enum class State { free, binding, active, stopping, quiescing };
    struct Edge { bool pending=false, connected=false, disconnect_pending=false; };
    Error Connect() noexcept;
    Error Disconnect(Edge &, bool transmit) noexcept;
    void Latch(Error) noexcept;
    const PjsuaApi &api_;
    PjsuaAudioPort audio_;
    State state_=State::free;
    CallHandle call_{};
    AgentHandle agent_{};
    std::uint32_t token_=0;
    pjsua_call_id native_=PJSUA_INVALID_ID;
    pjsua_conf_port_id custom_=PJSUA_INVALID_ID, slot_=PJSUA_INVALID_ID;
    pj_pool_t *pool_=nullptr;
    PcmSink *sink_=nullptr;
    Edge tx_{}, rx_{};
    bool group_=false, owner_released_=false, add_pending_=false;
    bool remove_pending_=false, remove_done_=false, native_inactive_=true;
    bool native_slot_removed_=false;
    Error error_=Error::ok;
};
}
#endif
