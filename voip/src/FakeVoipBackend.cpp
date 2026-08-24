#include <voip/FakeVoipBackend.hpp>

#include <cstring>

namespace voip {

FakeVoipBackend::FakeVoipBackend() noexcept
    : observer_(nullptr), initialized_(false), configured_(false),
      registration_state_(RegistrationState::disabled), call_{}, account_uri_{},
      registrar_uri_{}, username_{}, password_{} {
    call_.state = CallState::idle;
    call_.codec = Codec::pcmu;
    call_.direction = MediaDirection::inactive;
}

bool FakeVoipBackend::Copy(char *destination, std::size_t capacity,
                           const char *source) noexcept {
    if (destination == nullptr || source == nullptr || capacity == 0) {
        return false;
    }
    const std::size_t length = std::strlen(source);
    if (length >= capacity) {
        return false;
    }
    std::memcpy(destination, source, length + 1);
    return true;
}

Status FakeVoipBackend::MakeStatus(Error error, std::uint16_t sip_status,
                                   const char *reason) noexcept {
    Status status{};
    status.error = error;
    status.sip_status = sip_status;
    (void)Copy(status.reason, sizeof(status.reason), reason == nullptr ? "" : reason);
    return status;
}

Error FakeVoipBackend::Initialize(Observer *observer) {
    if (initialized_) {
        return Error::invalid_state;
    }
    observer_ = observer;
    initialized_ = true;
    return Error::ok;
}

Error FakeVoipBackend::Shutdown() {
    if (!initialized_) {
        return Error::ok;
    }
    observer_ = nullptr;
    initialized_ = false;
    configured_ = false;
    registration_state_ = RegistrationState::disabled;
    call_ = {};
    call_.state = CallState::idle;
    call_.codec = Codec::pcmu;
    call_.direction = MediaDirection::inactive;
    std::memset(password_, 0, sizeof(password_));
    return Error::ok;
}

Error FakeVoipBackend::ConfigureAccount(const AccountConfig &config) {
    if (!initialized_ || config.registration_expires_seconds == 0 ||
        config.account_uri == nullptr || config.registrar_uri == nullptr ||
        config.username == nullptr || config.password == nullptr) {
        return Error::invalid_argument;
    }
    if (!Copy(account_uri_, sizeof(account_uri_), config.account_uri) ||
        !Copy(registrar_uri_, sizeof(registrar_uri_), config.registrar_uri) ||
        !Copy(username_, sizeof(username_), config.username) ||
        !Copy(password_, sizeof(password_), config.password)) {
        return Error::value_too_long;
    }
    configured_ = true;
    registration_state_ = RegistrationState::disabled;
    return Error::ok;
}

Error FakeVoipBackend::RegisterAccount() {
    if (!initialized_) return Error::not_initialized;
    if (!configured_) return Error::invalid_state;
    registration_state_ = RegistrationState::registering;
    if (observer_ != nullptr) {
        observer_->OnRegistrationState(registration_state_, MakeStatus(Error::ok, 0, ""));
    }
    registration_state_ = RegistrationState::registered;
    if (observer_ != nullptr) {
        observer_->OnRegistrationState(registration_state_, MakeStatus(Error::ok, 200, "OK"));
    }
    return Error::ok;
}

Error FakeVoipBackend::UnregisterAccount() {
    if (registration_state_ != RegistrationState::registered) {
        return Error::invalid_state;
    }
    registration_state_ = RegistrationState::unregistering;
    if (observer_ != nullptr) {
        observer_->OnRegistrationState(registration_state_, MakeStatus(Error::ok, 0, ""));
    }
    registration_state_ = RegistrationState::disabled;
    if (observer_ != nullptr) {
        observer_->OnRegistrationState(registration_state_, MakeStatus(Error::ok, 200, "OK"));
    }
    return Error::ok;
}

void FakeVoipBackend::NotifyCall(Error error, std::uint16_t sip_status,
                                 const char *reason) noexcept {
    if (observer_ != nullptr) {
        observer_->OnCallState(call_, MakeStatus(error, sip_status, reason));
    }
}

Error FakeVoipBackend::StartOutgoingCall(const char *remote_uri) {
    if (!initialized_) return Error::not_initialized;
    if (call_.state != CallState::idle && call_.state != CallState::disconnected) {
        return Error::busy;
    }
    if (!Copy(call_.remote_uri, sizeof(call_.remote_uri), remote_uri)) {
        return remote_uri == nullptr ? Error::invalid_argument : Error::value_too_long;
    }
    call_.state = CallState::outgoing;
    call_.direction = MediaDirection::inactive;
    NotifyCall();
    return Error::ok;
}

Error FakeVoipBackend::InjectIncomingCall(const char *remote_uri) noexcept {
    if (!initialized_) return Error::not_initialized;
    if (call_.state != CallState::idle && call_.state != CallState::disconnected) {
        return Error::busy;
    }
    if (!Copy(call_.remote_uri, sizeof(call_.remote_uri), remote_uri)) {
        return remote_uri == nullptr ? Error::invalid_argument : Error::value_too_long;
    }
    call_.state = CallState::incoming;
    if (observer_ != nullptr) observer_->OnIncomingCall(call_);
    NotifyCall();
    return Error::ok;
}

Error FakeVoipBackend::AcceptCall() {
    if (call_.state != CallState::incoming) return Error::invalid_state;
    call_.state = CallState::early;
    NotifyCall(Error::ok, 180, "Ringing");
    return Error::ok;
}

Error FakeVoipBackend::EstablishCall(Codec codec) noexcept {
    if (call_.state != CallState::outgoing && call_.state != CallState::early) {
        return Error::invalid_state;
    }
    call_.state = CallState::established;
    call_.codec = codec;
    call_.direction = MediaDirection::send_receive;
    call_.local_rtp_port = 4000;
    call_.local_rtcp_port = 4001;
    call_.remote_rtp_port = 5000;
    call_.remote_rtcp_port = 5001;
    (void)Copy(call_.remote_address, sizeof(call_.remote_address), "127.0.0.1");
    NotifyCall(Error::ok, 200, "OK");
    if (observer_ != nullptr) {
        observer_->OnMediaState(call_, MakeStatus(Error::ok, 0, ""));
    }
    return Error::ok;
}

Error FakeVoipBackend::RejectCall(std::uint16_t status) {
    if (call_.state != CallState::incoming && call_.state != CallState::early) {
        return Error::invalid_state;
    }
    call_.state = CallState::disconnected;
    call_.direction = MediaDirection::inactive;
    NotifyCall(Error::ok, status, "Rejected");
    return Error::ok;
}

Error FakeVoipBackend::EndCall() {
    if (call_.state == CallState::idle || call_.state == CallState::disconnected) {
        return Error::invalid_state;
    }
    call_.state = CallState::disconnecting;
    NotifyCall();
    call_.state = CallState::disconnected;
    call_.direction = MediaDirection::inactive;
    NotifyCall(Error::ok, 200, "OK");
    return Error::ok;
}

Error FakeVoipBackend::SetHeld(bool held) {
    if (held && call_.state == CallState::established) {
        call_.state = CallState::held;
        call_.direction = MediaDirection::inactive;
    } else if (!held && call_.state == CallState::held) {
        call_.state = CallState::established;
        call_.direction = MediaDirection::send_receive;
    } else {
        return Error::invalid_state;
    }
    NotifyCall();
    return Error::ok;
}

RegistrationState FakeVoipBackend::GetRegistrationState() const {
    return registration_state_;
}
CallInfo FakeVoipBackend::GetCallInfo() const { return call_; }

} // namespace voip
