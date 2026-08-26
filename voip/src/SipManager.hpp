#ifndef VOIP_SIP_MANAGER_HPP
#define VOIP_SIP_MANAGER_HPP

#include <voip/VoipFacade.hpp>
#include <voip/VoipService.hpp>

#include "HandlePool.hpp"

#include <cstddef>

namespace voip {

#if defined(CONFIG_VOIP_MAX_ACCOUNTS)
constexpr std::size_t sip_manager_max_accounts = CONFIG_VOIP_MAX_ACCOUNTS;
#else
constexpr std::size_t sip_manager_max_accounts = 4;
#endif
#if defined(CONFIG_VOIP_MAX_CALLS)
constexpr std::size_t sip_manager_max_calls = CONFIG_VOIP_MAX_CALLS;
#else
constexpr std::size_t sip_manager_max_calls = 4;
#endif

/* Owns SIP account/call slots and presents a PJPROJECT-independent seam. */
class SipManager final : public Observer {
public:
    explicit SipManager(Backend &backend) noexcept;
    ~SipManager() override = default;

    SipManager(const SipManager &) = delete;
    SipManager &operator=(const SipManager &) = delete;

    Error Initialize(Observer *) noexcept;
    Error Shutdown() noexcept;
    Error AddAccount(const AccountConfig &, AccountHandle *) noexcept;
    Error RemoveAccount(AccountHandle) noexcept;
    Error SetRegistration(AccountHandle, bool) noexcept;
    Error Dial(AccountHandle, const CallConfig &, CallHandle *) noexcept;
    Error Answer(CallHandle) noexcept;
    Error Reject(CallHandle, std::uint16_t) noexcept;
    Error Hangup(CallHandle) noexcept;
    Error SetHeld(CallHandle, bool) noexcept;
    Error GetAccountState(AccountHandle, RegistrationState *) const noexcept;
    Error GetCallState(CallHandle, CallInfo *) const noexcept;
    AccountHandle CurrentAccount() const noexcept;
    CallHandle CurrentCall() const noexcept;

private:
    void OnRegistrationState(RegistrationState, const Status &) override;
    void OnIncomingCall(const CallInfo &) override;
    void OnCallState(const CallInfo &, const Status &) override;
    void OnMediaState(const CallInfo &, const Status &) override;

    Backend &backend_;
    Observer *observer_;
    bool initialized_;
    bool account_live_;
    bool call_live_;
    AccountHandle account_;
    CallHandle call_;
    RegistrationState registration_state_;
    CallInfo call_info_;
    HandlePool<AccountHandle, sip_manager_max_accounts> account_slots_;
    HandlePool<CallHandle, sip_manager_max_calls> call_slots_;
};

} // namespace voip

#endif
