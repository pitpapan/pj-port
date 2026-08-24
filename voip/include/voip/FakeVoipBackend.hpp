#ifndef VOIP_FAKE_VOIP_BACKEND_HPP
#define VOIP_FAKE_VOIP_BACKEND_HPP

#include <voip/VoipFacade.hpp>

namespace voip {

class FakeVoipBackend final : public Backend {
public:
    FakeVoipBackend() noexcept;

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

    Error InjectIncomingCall(const char *remote_uri) noexcept;
    Error EstablishCall(Codec codec = Codec::pcmu) noexcept;

private:
    Observer *observer_;
    bool initialized_;
    bool configured_;
    RegistrationState registration_state_;
    CallInfo call_;
    MediaStats media_stats_;
    bool media_running_;
    bool media_paused_;
    char account_uri_[max_uri_length + 1];
    char registrar_uri_[max_uri_length + 1];
    char username_[max_username_length + 1];
    char password_[max_password_length + 1];

    static Status MakeStatus(Error, std::uint16_t, const char *) noexcept;
    static bool Copy(char *, std::size_t, const char *) noexcept;
    void NotifyCall(Error = Error::ok, std::uint16_t = 0,
                    const char * = "") noexcept;
};

} // namespace voip

#endif
