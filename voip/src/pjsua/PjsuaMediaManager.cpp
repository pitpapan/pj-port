#include "PjsuaMediaManager.hpp"
namespace voip {
Error PjsuaMediaManager::Initialize(const AgentRegistry &registry,MediaSecurity security,PcmFormat format) noexcept {
    const auto validated=MediaSecurityPolicy::Validate(security);
    if (validated!=Error::ok) return validated;
    if (registry_ || Available()!=2) return Error::invalid_state;
    registry_=&registry; format_=format; security_=security; stopping_=false;
    return Error::ok;
}
PjsuaMediaManager::Owner *PjsuaMediaManager::Find(std::uint32_t token) noexcept {
    if (!token) return nullptr;
    for (auto &owner : owners_) if (owner.token==token) return &owner;
    return nullptr;
}
Error PjsuaMediaManager::OnPromoted(CallHandle call,AgentHandle agent,std::uint32_t token) noexcept {
    if (!registry_) return Error::invalid_state;
    if (!call.IsValid() || !token || !registry_->Resolve(agent)) return Error::invalid_handle;
    const auto validated=MediaSecurityPolicy::Validate(security_);
    if (validated!=Error::ok) return validated;
    if (auto *owner=Find(token))
        return owner->call.slot==call.slot && owner->call.generation==call.generation &&
               owner->agent.slot==agent.slot && owner->agent.generation==agent.generation ? Error::ok : Error::invalid_handle;
    for (const auto &owner : owners_)
        if (owner.token && owner.agent.slot==agent.slot && owner.agent.generation==agent.generation) return Error::busy;
    for (unsigned i=0;i<owners_.size();++i) if (!owners_[i].token && bridges_[i].IsFree()) {
        owners_[i]={}; owners_[i].call=call; owners_[i].agent=agent; owners_[i].token=token;
        owners_[i].stopping=stopping_;
        return Error::ok;
    }
    return Error::resource_exhausted;
}
void PjsuaMediaManager::OnMediaState(std::uint32_t token,const CopiedMediaState &media) noexcept {
    auto *owner=Find(token);
    if (!owner || owner->stopping ||
        (owner->media.native_id!=PJSUA_INVALID_ID && media.native_id!=owner->media.native_id)) return;
    owner->media=media;
}
void PjsuaMediaManager::BeginStop(std::uint32_t token) noexcept {
    if (auto *owner=Find(token)) {
        owner->stopping=true;
        bridges_[static_cast<unsigned>(owner-owners_.data())].BeginStop();
    }
}
void PjsuaMediaManager::MarkNativeInactive(std::uint32_t token) noexcept {
    if (auto *owner=Find(token)) {
        owner->native_inactive=true;
        bridges_[static_cast<unsigned>(owner-owners_.data())].SetNativeInactive();
    }
}
bool PjsuaMediaManager::IsQuiescent(std::uint32_t token) const noexcept {
    for (unsigned i=0;i<owners_.size();++i)
        if (owners_[i].token==token) return bridges_[i].IsFree() && owners_[i].native_inactive;
    return true;
}
void PjsuaMediaManager::OnConferenceOperation(const pjmedia_conf_op_info &op) noexcept {
    for (auto &bridge : bridges_) bridge.OnConferenceOperation(op);
}
void PjsuaMediaManager::Fail(Owner &owner,Error error) noexcept {
    if (error==Error::ok || error==Error::busy) return;
    if (owner.failure==Error::ok) { owner.failure=error; owner.failure_pending=true; }
    BeginStop(owner.token);
}
void PjsuaMediaManager::Pump() noexcept {
    if (!registry_) return;
    for (unsigned i=0;i<owners_.size();++i) {
        auto &owner=owners_[i]; auto &bridge=bridges_[i];
        if (!owner.token) continue;
        const auto error=bridge.TakeError();
        Fail(owner,error);
        if (owner.stopping) {
            const auto stopped=bridge.PollStop();
            // A cleanup error keeps the owner and its borrowed storage live.
            // The actor retries; no timeout authorizes recycling this bridge.
            if (stopped==Error::ok && owner.native_inactive && !owner.failure_pending) owner={};
            continue;
        }
        if (!bridge.IsFree() && (bridge.NativeSlotRemoved() ||
            (owner.media.slot>0 && bridge.CallSlot()>0 && owner.media.slot!=bridge.CallSlot()))) {
            owner.retiring=true; bridge.BeginStop();
        }
        if (owner.retiring) {
            if (bridge.NativeSlotRemoved()) bridge.SetNativeInactive();
            if (bridge.PollStop()!=Error::ok) continue;
            owner.retiring=false;
        }
        const auto &media=owner.media;
        const bool live=media.status==PJSUA_CALL_MEDIA_ACTIVE || media.status==PJSUA_CALL_MEDIA_LOCAL_HOLD ||
                        media.status==PJSUA_CALL_MEDIA_REMOTE_HOLD;
        if (!live || media.native_id<0 || media.slot<=0) continue;
        const auto *agent=registry_->Resolve(owner.agent);
        if (!agent) { Fail(owner,Error::invalid_handle); continue; }
        const auto attached=bridge.Attach(owner.call,owner.agent,owner.token,media.native_id,agent->audio,format_);
        Fail(owner,attached);
    }
}
bool PjsuaMediaManager::TakeFailure(std::uint32_t *token,Error *error) noexcept {
    if (!token || !error) return false;
    for (auto &owner : owners_) if (owner.failure_pending) {
        *token=owner.token; *error=owner.failure; owner.failure_pending=false; return true;
    }
    return false;
}
std::uint16_t PjsuaMediaManager::Available() const noexcept {
    std::uint16_t free=0; for (const auto &bridge : bridges_) if (bridge.IsFree()) ++free; return free;
}
Error PjsuaMediaManager::Shutdown() noexcept {
    BeginShutdown();
    Pump();
    for (const auto &owner : owners_) if (owner.token) return Error::busy;
    registry_=nullptr; return Error::ok;
}
void PjsuaMediaManager::BeginShutdown() noexcept {
    stopping_=true;
    for (auto &owner : owners_) if (owner.token) BeginStop(owner.token);
}
}
