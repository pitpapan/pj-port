#include <voip/VoipFacade.hpp>

namespace voip {

VoipManager::VoipManager(Backend &backend) noexcept
    : backend_(backend), initialized_(false) {}

VoipManager::~VoipManager() { (void)Shutdown(); }

Error VoipManager::Initialize(Observer *observer) noexcept {
    if (initialized_) {
        return Error::invalid_state;
    }
    const Error result = backend_.Initialize(observer);
    initialized_ = result == Error::ok;
    return result;
}

Error VoipManager::Shutdown() noexcept {
    if (!initialized_) {
        return Error::ok;
    }
    const Error result = backend_.Shutdown();
    if (result == Error::ok) {
        initialized_ = false;
    }
    return result;
}

#define VOIP_FORWARD(method, ...)                                                \
    return initialized_ ? backend_.method(__VA_ARGS__) : Error::not_initialized

Error VoipManager::ConfigureAccount(const AccountConfig &config) noexcept {
    VOIP_FORWARD(ConfigureAccount, config);
}
Error VoipManager::RegisterAccount() noexcept { VOIP_FORWARD(RegisterAccount); }
Error VoipManager::UnregisterAccount() noexcept {
    VOIP_FORWARD(UnregisterAccount);
}
Error VoipManager::StartOutgoingCall(const char *uri) noexcept {
    VOIP_FORWARD(StartOutgoingCall, uri);
}
Error VoipManager::AcceptCall() noexcept { VOIP_FORWARD(AcceptCall); }
Error VoipManager::RejectCall(std::uint16_t status) noexcept {
    VOIP_FORWARD(RejectCall, status);
}
Error VoipManager::EndCall() noexcept { VOIP_FORWARD(EndCall); }
Error VoipManager::SetHeld(bool held) noexcept { VOIP_FORWARD(SetHeld, held); }

#undef VOIP_FORWARD

RegistrationState VoipManager::GetRegistrationState() const noexcept {
    return initialized_ ? backend_.GetRegistrationState()
                        : RegistrationState::disabled;
}

CallInfo VoipManager::GetCallInfo() const noexcept {
    CallInfo empty{};
    empty.state = CallState::idle;
    empty.codec = Codec::pcmu;
    empty.direction = MediaDirection::inactive;
    return initialized_ ? backend_.GetCallInfo() : empty;
}

} // namespace voip
