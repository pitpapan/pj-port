#include "PjsuaMediaBridge.hpp"
#include <initializer_list>
namespace voip {
void PjsuaMediaBridge::Latch(Error error) noexcept {
    if (error_==Error::ok) error_=error;
}
Error PjsuaMediaBridge::Attach(CallHandle call,AgentHandle agent,std::uint32_t token,
                             pjsua_call_id native,AgentAudioBinding binding,PcmFormat format) noexcept {
    if (!call.IsValid() || !agent.IsValid() || token==0 || native<0) return Error::invalid_argument;
    if (state_!=State::free) {
        if (call.slot!=call_.slot || call.generation!=call_.generation ||
            agent.slot!=agent_.slot || agent.generation!=agent_.generation ||
            token!=token_ || native!=native_) return Error::busy;
        if (state_==State::stopping || state_==State::quiescing) return Error::busy;
        return Connect();
    }
    if (!api_.pool_create || !api_.pool_release || !api_.port_init_grp_lock || !api_.port_destroy ||
        !api_.conf_add_port || !api_.call_get_conf_port || !api_.conf_connect ||
        !api_.conf_disconnect || !api_.conf_remove_port) return Error::unsupported_configuration;
    const auto initialized=audio_.Initialize(binding,format);
    if (initialized!=Error::ok) return initialized;
    call_=call; agent_=agent; token_=token; native_=native; sink_=binding.sink;
    state_=State::binding; error_=Error::ok;
    pool_=api_.pool_create("agent-bridge",2048,1024);
    if (!pool_) { BeginStop(); (void)PollStop(); return Error::resource_exhausted; }
    if (api_.port_init_grp_lock(audio_.Port(),pool_,nullptr)!=PJ_SUCCESS) {
        BeginStop(); (void)PollStop(); return Error::resource_exhausted;
    }
    group_=true;
    if (api_.conf_add_port(pool_,audio_.Port(),&custom_)!=PJ_SUCCESS) {
        custom_=PJSUA_INVALID_ID; BeginStop(); (void)PollStop(); return Error::media_failed;
    }
    add_pending_=true;
    if (custom_<=0) { BeginStop(); return Error::media_failed; }
    const auto result=Connect();
    if (result!=Error::ok && result!=Error::busy) BeginStop();
    return result;
}
Error PjsuaMediaBridge::Connect() noexcept {
    const auto current=api_.call_get_conf_port(native_);
    if (current<=0) return Error::busy;
    if (slot_>0 && current!=slot_) {
        // Retire the old stream before binding its replacement; never
        // redirect outstanding old-slot operations onto another native slot.
        return Error::busy;
    }
    if (current==custom_) return Error::media_failed;
    slot_=current; native_inactive_=false;
    for (bool transmit : {true,false}) {
        auto &edge=transmit ? tx_ : rx_;
        if (edge.pending || edge.connected) continue;
        const auto status=api_.conf_connect(transmit ? custom_ : slot_,transmit ? slot_ : custom_);
        if (status!=PJ_SUCCESS) { Latch(Error::media_failed); return Error::media_failed; }
        edge.pending=true;
    }
    return Error::ok;
}
void PjsuaMediaBridge::BeginStop() noexcept {
    if (state_==State::free) return;
    audio_.BeginStop(); state_=State::stopping;
}
Error PjsuaMediaBridge::Disconnect(Edge &edge,bool transmit) noexcept {
    if (edge.pending || edge.disconnect_pending) return Error::busy;
    if (!edge.connected) return Error::ok;
    const auto status=api_.conf_disconnect(transmit ? custom_ : slot_,transmit ? slot_ : custom_);
    if (status!=PJ_SUCCESS) return PjsuaStatus(status);
    edge.disconnect_pending=true;
    return Error::busy;
}
Error PjsuaMediaBridge::PollStop() noexcept {
    if (state_==State::free) return Error::ok;
    if (state_!=State::stopping && state_!=State::quiescing) return Error::busy;
    if (add_pending_) return Error::busy;
    const auto tx=Disconnect(tx_,true), rx=Disconnect(rx_,false);
    if (tx!=Error::ok && tx!=Error::busy) return tx;
    if (rx!=Error::ok && rx!=Error::busy) return rx;
    if (tx!=Error::ok || rx!=Error::ok || !native_inactive_) return Error::busy;
    if (custom_>0 && !remove_done_) {
        if (!remove_pending_) {
            const auto status=api_.conf_remove_port(custom_);
            if (status!=PJ_SUCCESS) return PjsuaStatus(status);
            remove_pending_=true;
        }
        state_=State::quiescing;
    }
    if (group_ && !owner_released_) {
        // Drop our reference, not the conference reference. on_destroy
        // remains deferred until actual conference removal releases it.
        const auto status=api_.port_destroy(audio_.Port());
        if (status!=PJ_SUCCESS) return PjsuaStatus(status);
        owner_released_=true;
    }
    if ((custom_>0 && !remove_done_) || (group_ && !audio_.NativeDestroyed()) ||
        !audio_.IsQuiescent()) return Error::busy;
    sink_->Flush();
    if (pool_) api_.pool_release(pool_);
    pool_=nullptr; sink_=nullptr; call_={}; agent_={}; token_=0; native_=PJSUA_INVALID_ID;
    custom_=slot_=PJSUA_INVALID_ID; tx_={}; rx_={};
    group_=owner_released_=add_pending_=remove_pending_=remove_done_=false;
    native_inactive_=true; native_slot_removed_=false; state_=State::free;
    return Error::ok;
}
void PjsuaMediaBridge::OnConferenceOperation(const pjmedia_conf_op_info &op) noexcept {
    if (state_==State::free) return;
    if (op.op_type==PJMEDIA_CONF_OP_REMOVE_PORT && op.status==PJ_SUCCESS &&
        static_cast<int>(op.op_param.remove_port.port)==slot_) {
        // Removing a native stream disconnects all of its conference edges.
        // Its slot may already be absent when actor-side cleanup runs.
        native_slot_removed_=true; tx_.connected=rx_.connected=false;
        return;
    }
    if (op.op_type==PJMEDIA_CONF_OP_ADD_PORT && static_cast<int>(op.op_param.add_port.port)==custom_) {
        add_pending_=false;
        if (op.status!=PJ_SUCCESS) Latch(Error::media_failed);
    } else if (op.op_type==PJMEDIA_CONF_OP_REMOVE_PORT &&
               static_cast<int>(op.op_param.remove_port.port)==custom_ && remove_pending_) {
        remove_pending_=false; remove_done_=op.status==PJ_SUCCESS;
        if (!remove_done_) Latch(Error::media_failed);
    } else if (op.op_type==PJMEDIA_CONF_OP_CONNECT_PORTS || op.op_type==PJMEDIA_CONF_OP_DISCONNECT_PORTS) {
        const bool connect=op.op_type==PJMEDIA_CONF_OP_CONNECT_PORTS;
        const auto src=connect ? op.op_param.connect_ports.src : op.op_param.disconnect_ports.src;
        const auto dst=connect ? op.op_param.connect_ports.sink : op.op_param.disconnect_ports.sink;
        Edge *edge=nullptr;
        if (static_cast<int>(src)==custom_ && static_cast<int>(dst)==slot_) edge=&tx_;
        if (static_cast<int>(src)==slot_ && static_cast<int>(dst)==custom_) edge=&rx_;
        if (!edge || (connect ? !edge->pending : !edge->disconnect_pending)) return;
        if (connect) { edge->pending=false; edge->connected=op.status==PJ_SUCCESS; }
        else { edge->disconnect_pending=false; if (op.status==PJ_SUCCESS) edge->connected=false; }
        if (op.status!=PJ_SUCCESS && !native_slot_removed_) Latch(Error::media_failed);
    }
    if (state_==State::binding && !add_pending_ && tx_.connected && rx_.connected)
        state_=State::active;
}
bool PjsuaMediaBridge::IsFree() const noexcept { return state_==State::free; }
bool PjsuaMediaBridge::IsActive() const noexcept {
    return state_==State::active && !native_slot_removed_;
}
void PjsuaMediaBridge::SetNativeInactive() noexcept { native_inactive_=true; }
Error PjsuaMediaBridge::TakeError() noexcept {
    const auto endpoint=audio_.TakeError();
    if (endpoint!=Error::ok) Latch(endpoint);
    const auto error=error_; error_=Error::ok; return error;
}
std::uint32_t PjsuaMediaBridge::Token() const noexcept { return token_; }
pjsua_conf_port_id PjsuaMediaBridge::CustomSlot() const noexcept { return custom_; }
pjsua_conf_port_id PjsuaMediaBridge::CallSlot() const noexcept { return slot_; }
}
