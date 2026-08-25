#ifndef VOIP_PJ_VOIP_BACKEND_HPP
#define VOIP_PJ_VOIP_BACKEND_HPP

#include <voip/VoipFacade.hpp>

#include <cstdint>

namespace voip {

enum class RuntimeFailurePoint : std::uint8_t {
    none,
    after_pjlib,
    after_pjlib_util,
    after_pool_factory,
    after_sip_endpoint,
    after_media_endpoint,
    after_tcp_factory,
    after_event_thread,
};

struct RuntimeResources {
    std::uint32_t pj_pool_bytes;
    std::uint32_t pj_pool_peak_bytes;
    std::uint32_t pj_pools;
    std::uint32_t timers;
    std::uint32_t transactions;
    std::uint32_t dialogs;
    std::uint32_t event_stack_max_bytes;
};

class PjVoipBackend final : public Backend {
public:
    explicit PjVoipBackend(
        RuntimeFailurePoint failure = RuntimeFailurePoint::none) noexcept;
    ~PjVoipBackend() override;

    PjVoipBackend(const PjVoipBackend &) = delete;
    PjVoipBackend &operator=(const PjVoipBackend &) = delete;

    Error Initialize(Observer *) override;
    Error Shutdown() override;
    Error ConfigureAccount(const AccountConfig &) override;
    Error RegisterAccount() override;
    Error UnregisterAccount() override;
    Error StartOutgoingCall(const char *) override;
    Error AcceptCall() override;
    Error RejectCall(std::uint16_t) override;
    Error EndCall() override;
    Error SetHeld(bool) override;
    Error StartHeadlessMedia(Codec) override;
    Error SetMediaPaused(bool) override;
    Error StopMedia() override;
    MediaStats GetMediaStats() const override;
    RegistrationState GetRegistrationState() const override;
    CallInfo GetCallInfo() const override;

    Error SubmitProbe(std::uint32_t value) noexcept;
    Error BeginShutdown() noexcept;
    std::uint32_t ProcessedProbeCount() const noexcept;
    std::uint32_t LastProbeValue() const noexcept;
    std::uint16_t TcpPort() const noexcept;
    bool IsRunning() const noexcept;
    bool HasLiveResources() const noexcept;
    void SetProbeProcessingPaused(bool paused) noexcept;
    bool HasRegistrationClient() const noexcept;
    void SetObserver(Observer *observer) noexcept;
    Error InjectRegistrationState(RegistrationState state,
                                  std::uint16_t sip_status = 0,
                                  Error error = Error::ok) noexcept;
    void *NativeSipEndpointForValidation() const noexcept;
    Error InjectMediaTransportFailureForValidation() noexcept;
    bool SrtpKeysActiveForValidation() const noexcept;
    bool SrtpKeysClearedForValidation() const noexcept;
    bool SrtpTransportActiveForValidation() const noexcept;
    RuntimeResources ResourcesForValidation() const noexcept;

private:
    class Impl;
    Impl *impl_;
    RuntimeFailurePoint failure_;
};

} // namespace voip

#endif
