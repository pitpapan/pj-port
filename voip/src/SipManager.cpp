#include "SipManager.hpp"

#include <cstring>

namespace voip {
namespace {

bool ValidString(const char *value, std::size_t capacity) noexcept {
    return value != nullptr && std::strlen(value) < capacity;
}

} // namespace

SipManager::SipManager(Backend &backend) noexcept
    : backend_(backend), observer_(nullptr), initialized_(false),
      account_live_(false), call_live_(false), account_{}, call_{},
      registration_state_(RegistrationState::disabled), call_info_{},
      account_slots_{}, call_slots_{} {}

Error SipManager::Initialize(Observer *observer) noexcept {
    if (initialized_) return Error::invalid_state;
    if (observer == nullptr) return Error::invalid_argument;
    observer_ = observer;
    const Error result = backend_.Initialize(this);
    if (result != Error::ok) {
        observer_ = nullptr;
        return result;
    }
    initialized_ = true;
    return Error::ok;
}

Error SipManager::Shutdown() noexcept {
    if (!initialized_) return Error::ok;
    const Error result = backend_.Shutdown();
    call_slots_.Clear();
    account_slots_.Clear();
    initialized_ = false;
    account_live_ = false;
    call_live_ = false;
    observer_ = nullptr;
    return result;
}

Error SipManager::AddAccount(const AccountConfig &config,
                             AccountHandle *handle) noexcept {
    if (!initialized_) return Error::not_initialized;
    if (handle == nullptr ||
        !ValidString(config.account_uri, max_uri_length + 1) ||
        !ValidString(config.registrar_uri, max_uri_length + 1) ||
        !ValidString(config.username, max_username_length + 1) ||
        !ValidString(config.password, max_password_length + 1))
        return Error::invalid_argument;
    if (account_live_) return Error::busy;
    const Error result = backend_.ConfigureAccount(config);
    if (result != Error::ok) return result;
    if (!account_slots_.Acquire(handle)) return Error::resource_exhausted;
    account_ = *handle;
    account_live_ = true;
    registration_state_ = RegistrationState::disabled;
    return Error::ok;
}

Error SipManager::RemoveAccount(AccountHandle handle) noexcept {
    if (!initialized_) return Error::not_initialized;
    if (!account_slots_.IsLive(handle)) return Error::invalid_argument;
    if (call_live_) return Error::busy;
    (void)backend_.UnregisterAccount();
    (void)account_slots_.Release(handle);
    account_live_ = false;
    return Error::ok;
}

Error SipManager::SetRegistration(AccountHandle handle, bool enabled) noexcept {
    if (!initialized_) return Error::not_initialized;
    if (!account_slots_.IsLive(handle)) return Error::invalid_argument;
    return enabled ? backend_.RegisterAccount() : backend_.UnregisterAccount();
}

Error SipManager::Dial(AccountHandle account, const CallConfig &config,
                       CallHandle *call) noexcept {
    if (!initialized_) return Error::not_initialized;
    if (!account_slots_.IsLive(account) || call == nullptr ||
        !ValidString(config.remote_uri, max_uri_length + 1) ||
        config.source == nullptr || config.sink == nullptr)
        return Error::invalid_argument;
    if (call_live_) return Error::busy;
    if (!call_slots_.Acquire(call)) return Error::resource_exhausted;
    account_ = account;
    call_ = *call;
    call_live_ = true;
    const Error result = backend_.StartOutgoingCall(config.remote_uri);
    if (result != Error::ok) {
        (void)call_slots_.Release(*call);
        call_live_ = false;
    }
    return result;
}

Error SipManager::Answer(CallHandle call) noexcept {
    if (!initialized_) return Error::not_initialized;
    if (!call_slots_.IsLive(call)) return Error::invalid_argument;
    return backend_.AcceptCall();
}

Error SipManager::Reject(CallHandle call, std::uint16_t status) noexcept {
    if (!initialized_) return Error::not_initialized;
    if (!call_slots_.IsLive(call)) return Error::invalid_argument;
    return backend_.RejectCall(status);
}

Error SipManager::Hangup(CallHandle call) noexcept {
    if (!initialized_) return Error::not_initialized;
    if (!call_slots_.IsLive(call)) return Error::invalid_argument;
    return backend_.EndCall();
}

Error SipManager::SetHeld(CallHandle call, bool held) noexcept {
    if (!initialized_) return Error::not_initialized;
    if (!call_slots_.IsLive(call)) return Error::invalid_argument;
    return backend_.SetHeld(held);
}

Error SipManager::GetAccountState(AccountHandle handle,
                                  RegistrationState *state) const noexcept {
    if (!initialized_) return Error::not_initialized;
    if (state == nullptr || !account_slots_.IsLive(handle))
        return Error::invalid_argument;
    *state = registration_state_;
    return Error::ok;
}

Error SipManager::GetCallState(CallHandle handle,
                               CallInfo *info) const noexcept {
    if (!initialized_) return Error::not_initialized;
    if (info == nullptr || !call_slots_.IsLive(handle))
        return Error::invalid_argument;
    *info = call_info_;
    return Error::ok;
}

AccountHandle SipManager::CurrentAccount() const noexcept { return account_; }
CallHandle SipManager::CurrentCall() const noexcept { return call_; }

void SipManager::OnRegistrationState(RegistrationState state,
                                     const Status &status) {
    registration_state_ = state;
    if (observer_ != nullptr) observer_->OnRegistrationState(state, status);
}

void SipManager::OnIncomingCall(const CallInfo &info) {
    if (!call_live_ && call_slots_.Acquire(&call_)) call_live_ = true;
    call_info_ = info;
    if (observer_ != nullptr) observer_->OnIncomingCall(info);
}

void SipManager::OnCallState(const CallInfo &info, const Status &status) {
    call_info_ = info;
    call_live_ = info.state != CallState::disconnected &&
                 info.state != CallState::failed;
    if (observer_ != nullptr) observer_->OnCallState(info, status);
    if (!call_live_) (void)call_slots_.Release(call_);
}

void SipManager::OnMediaState(const CallInfo &info, const Status &status) {
    call_info_ = info;
    if (observer_ != nullptr) observer_->OnMediaState(info, status);
}

} // namespace voip
