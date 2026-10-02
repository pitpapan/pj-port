#ifndef VOIP_PJSUA_MEDIA_MANAGER_HPP
#define VOIP_PJSUA_MEDIA_MANAGER_HPP
#include "PjsuaMediaBridge.hpp"
#include "../core/AgentRegistry.hpp"
#include "MediaSecurityPolicy.hpp"
#include "PjsuaCallRecords.hpp"
#include <array>
namespace voip {
class PjsuaMediaManager final {
public:
    explicit PjsuaMediaManager(const PjsuaApi &api=NativePjsuaApi()) noexcept : bridges_{PjsuaMediaBridge(api),PjsuaMediaBridge(api)} {}
    Error Initialize(const AgentRegistry &,MediaSecurity,PcmFormat) noexcept;
    Error OnPromoted(CallHandle,AgentHandle,std::uint32_t) noexcept;
    void OnMediaState(std::uint32_t,const CopiedMediaState &) noexcept;
    void BeginStop(std::uint32_t) noexcept;
    void MarkNativeInactive(std::uint32_t) noexcept;
    bool IsQuiescent(std::uint32_t) const noexcept;
    void OnConferenceOperation(const pjmedia_conf_op_info &) noexcept;
    void Pump() noexcept;
    bool TakeFailure(std::uint32_t *,Error *) noexcept;
    std::uint16_t Available() const noexcept;
    Error Shutdown() noexcept;
    void BeginShutdown() noexcept;
private:
    struct Owner {
        CallHandle call{};
        AgentHandle agent{};
        std::uint32_t token=0;
        CopiedMediaState media{};
        bool stopping=false, native_inactive=false, retiring=false, failure_pending=false;
        Error failure=Error::ok;
    };
    Owner *Find(std::uint32_t) noexcept;
    void Fail(Owner &,Error) noexcept;
    std::array<PjsuaMediaBridge,2> bridges_;
    std::array<Owner,2> owners_{};
    const AgentRegistry *registry_=nullptr;
    PcmFormat format_{};
    MediaSecurity security_=MediaSecurity::none;
    bool stopping_=false;
};
}
#endif
