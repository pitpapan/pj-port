#include "PjsuaAccountManager.hpp"

#include <cassert>
#include <cstring>
#include <limits>

namespace voip {
namespace {
std::size_t StringLength(const char *value) noexcept {
    std::size_t length = 0;
    while (value[length] != '\0') ++length;
    return length;
}
}

void PjsuaAccountManager::AssertActor() const noexcept {
#ifndef NDEBUG
    assert(actor_thread_ == std::thread::id{} || actor_thread_ == std::this_thread::get_id());
#endif
}

pj_str_t PjsuaAccountManager::NativeString(char *value) noexcept {
    return pj_str_t{value, static_cast<pj_ssize_t>(StringLength(value))};
}

std::size_t PjsuaAccountManager::Count() const noexcept {
    AssertActor();
    return count_;
}

bool PjsuaAccountManager::IsNativeIdInDomain(pjsua_acc_id id) noexcept {
    return id >= 0 && id < PJSUA_MAX_ACC;
}

PjsuaAccountManager::NativeLookup *PjsuaAccountManager::FindLookup(pjsua_acc_id id) noexcept {
    if (!IsNativeIdInDomain(id)) return nullptr;
    for (NativeLookup &entry : lookup_)
        if (entry.context != nullptr && entry.id == id) return &entry;
    return nullptr;
}

const PjsuaAccountManager::NativeLookup *PjsuaAccountManager::FindLookup(pjsua_acc_id id) const noexcept {
    if (!IsNativeIdInDomain(id)) return nullptr;
    for (const NativeLookup &entry : lookup_)
        if (entry.context != nullptr && entry.id == id) return &entry;
    return nullptr;
}

PjsuaAccountContext *PjsuaAccountManager::OwnedContext(void *candidate) noexcept {
    for (PjsuaAccountContext &context : contexts_)
        if (candidate == &context) return &context;
    return nullptr;
}

const PjsuaAccountContext *PjsuaAccountManager::OwnedContext(const void *candidate) const noexcept {
    for (const PjsuaAccountContext &context : contexts_)
        if (candidate == &context) return &context;
    return nullptr;
}

bool PjsuaAccountManager::InsertLookup(pjsua_acc_id id,
                                       PjsuaAccountContext *context) noexcept {
    if (!IsNativeIdInDomain(id) || context == nullptr) return false;
    for (NativeLookup &entry : lookup_) {
        if (entry.context != nullptr && entry.id == id) return false;
    }
    for (NativeLookup &entry : lookup_) {
        if (entry.context == nullptr) { entry = {id, context}; return true; }
    }
    return false;
}

PjsuaAccountContext *PjsuaAccountManager::Resolve(pjsua_acc_id id) noexcept {
    AssertActor();
    NativeLookup *entry = FindLookup(id);
    if (entry == nullptr) return nullptr;
    PjsuaAccountContext *context = OwnedContext(api_.acc_get_user_data(id));
    if (context == nullptr || context != entry->context || !context->occupied ||
        context->account_id != id) return nullptr;
    return context;
}

const PjsuaAccountContext *PjsuaAccountManager::Resolve(pjsua_acc_id id) const noexcept {
    AssertActor();
    const NativeLookup *entry = FindLookup(id);
    if (entry == nullptr) return nullptr;
    const PjsuaAccountContext *context = OwnedContext(api_.acc_get_user_data(id));
    if (context == nullptr || context != entry->context || !context->occupied ||
        context->account_id != id) return nullptr;
    return context;
}

Error PjsuaAccountManager::NativeId(AgentHandle agent, pjsua_acc_id *id) const noexcept {
    AssertActor();
    if (id == nullptr) return Error::invalid_argument;
    for (const PjsuaAccountContext &context : contexts_) {
        if (context.occupied && context.agent.slot == agent.slot && context.agent.generation == agent.generation) {
            *id = context.account_id;
            return Error::ok;
        }
    }
    return Error::invalid_handle;
}

void PjsuaAccountManager::Clear() noexcept {
    for (PjsuaAccountContext &context : contexts_) context = PjsuaAccountContext{};
    for (NativeLookup &entry : lookup_) entry = NativeLookup{};
    count_ = 0;
    notification_head_ = 0;
    notification_size_ = 0;
    now_ms_ = 0;
}

void PjsuaAccountManager::Rollback() noexcept {
    for (std::size_t index = count_; index != 0; --index) {
        PjsuaAccountContext &context = contexts_[index - 1];
        (void)api_.acc_set_user_data(context.account_id, nullptr);
        (void)api_.acc_del(context.account_id);
    }
    Clear();
}

Error PjsuaAccountManager::Initialize(const AgentRegistry &registry,
                                      pjsua_transport_id transport_id) noexcept {
    AssertActor();
    if (actor_thread_ == std::thread::id{}) actor_thread_ = std::this_thread::get_id();
    if (count_ != 0 || registry.Empty() || registry.Count() > max_accounts ||
        transport_id == PJSUA_INVALID_ID) return Error::invalid_state;

    Clear();
    for (std::size_t index = 0; index < registry.Count(); ++index) {
        AgentHandle handle{};
        if (registry.GetAgentHandle(static_cast<std::uint8_t>(index), &handle) != Error::ok) { Rollback(); return Error::internal_failure; }
        const AgentContext *agent = registry.Resolve(handle);
        if (agent == nullptr) { Rollback(); return Error::internal_failure; }
        PjsuaAccountContext &context = contexts_[index];
        context.agent = handle;
        context.registration = agent->registration;
        context.register_on_start = agent->registration != RegistrationState::disabled;
        context.occupied = true;

        pjsua_acc_config config{};
        api_.acc_config_default(&config);
        config.user_data = &context;
        config.id = NativeString(const_cast<char *>(agent->sip.identity_uri));
        config.reg_uri = NativeString(const_cast<char *>(agent->sip.registrar_uri));
        config.cred_count = 1;
        config.cred_info[0].realm = pj_str_t{const_cast<char *>("*"), 1};
        config.cred_info[0].scheme = pj_str_t{const_cast<char *>("Digest"), 6};
        config.cred_info[0].username = NativeString(const_cast<char *>(agent->sip.auth_username));
        config.cred_info[0].data_type = PJSIP_CRED_DATA_PLAIN_PASSWD;
        config.cred_info[0].data = NativeString(const_cast<char *>(agent->sip.auth_password));
        config.transport_id = transport_id;
        config.register_on_acc_add = PJ_FALSE;
        config.allow_contact_rewrite = PJ_FALSE;
        config.contact_use_src_port = PJ_FALSE;
        config.allow_via_rewrite = PJ_FALSE;
        config.allow_sdp_nat_rewrite = PJ_FALSE;
        config.use_rfc5626 = PJ_FALSE;
        config.sip_stun_use = PJSUA_STUN_USE_DISABLED;
        config.media_stun_use = PJSUA_STUN_USE_DISABLED;
        config.sip_upnp_use = PJSUA_UPNP_USE_DISABLED;
        config.media_upnp_use = PJSUA_UPNP_USE_DISABLED;
        config.ice_cfg_use = PJSUA_ICE_CONFIG_USE_DEFAULT;
        config.turn_cfg_use = PJSUA_TURN_CONFIG_USE_DEFAULT;
        config.use_srtp = PJMEDIA_SRTP_DISABLED;
        config.reg_retry_interval = 0;
        config.reg_first_retry_interval = 0;
        config.reg_retry_random_interval = 0;
        config.reg_delay_before_refresh = 1;

        pjsua_acc_id native_id = PJSUA_INVALID_ID;
        if (PjsuaStatus(api_.acc_add(&config, PJ_FALSE, &native_id)) != Error::ok) {
            Rollback(); return Error::internal_failure;
        }
        if (!IsNativeIdInDomain(native_id) || FindLookup(native_id) != nullptr) {
            context = PjsuaAccountContext{};
            Rollback();
            return Error::internal_failure;
        }
        context.account_id = native_id;
        ++count_;
        if (!InsertLookup(native_id, &context)) { Rollback(); return Error::internal_failure; }
    }
    return Error::ok;
}

Error PjsuaAccountManager::StartInitialRegistration() noexcept {
    AssertActor();
    if (count_ == 0) return Error::invalid_state;
    for (const PjsuaAccountContext &context : contexts_) {
        if (context.occupied && context.register_on_start &&
            PjsuaStatus(api_.acc_set_registration(context.account_id, PJ_TRUE)) != Error::ok)
            return Error::internal_failure;
    }
    return Error::ok;
}

std::uint64_t PjsuaAccountManager::AddSaturated(std::uint64_t base,
                                                std::uint64_t increment) noexcept {
    return increment > std::numeric_limits<std::uint64_t>::max() - base
        ? std::numeric_limits<std::uint64_t>::max() : base + increment;
}

bool PjsuaAccountManager::Recoverable(const PjsuaRegistrationRecord &record) noexcept {
    if (record.native_status != PJ_SUCCESS) return true;
    return record.sip_status == 408 || record.sip_status == 480 ||
           (record.sip_status >= 500 && record.sip_status < 700);
}

void PjsuaAccountManager::Notify(const PjsuaAccountContext &context,
                                 RegistrationState registration, Error error,
                                 std::uint16_t sip_status, const char *reason) noexcept {
    const bool guaranteed = IsGuaranteed(registration);
    if (guaranteed) {
        // Keep at most one pending failure and one retry-wait record per agent.
        // A new retry cycle replaces its older pair so the bounded queue cannot
        // be exhausted by unobserved cycles of a single account.
        for (std::size_t offset = 0; offset < notification_size_; ++offset) {
            const PjsuaRegistrationNotification &existing =
                notifications_[(notification_head_ + offset) % notification_capacity];
            if (existing.agent.slot == context.agent.slot &&
                existing.agent.generation == context.agent.generation &&
                ((registration == RegistrationState::retry_wait &&
                  existing.registration == RegistrationState::retry_wait) ||
                 (IsFailure(registration) && IsFailure(existing.registration)))) {
                RemoveNotification(offset);
                break;
            }
        }
        while (notification_size_ == notification_capacity) {
            bool removed = false;
            for (std::size_t offset = 0; offset < notification_size_; ++offset) {
                const RegistrationState existing =
                    notifications_[(notification_head_ + offset) % notification_capacity].registration;
                if (!IsGuaranteed(existing)) {
                    RemoveNotification(offset);
                    removed = true;
                    break;
                }
            }
            // The coalescing above bounds guaranteed records to two per agent.
            // This is defensive if a future state is classified incorrectly.
            if (!removed) RemoveNotification(0);
        }
    } else if (notification_size_ == notification_capacity) {
        return;
    }
    PjsuaRegistrationNotification &notification =
        notifications_[(notification_head_ + notification_size_) % notification_capacity];
    notification = {};
    notification.agent = context.agent;
    notification.registration = registration;
    notification.status.error = error;
    notification.status.sip_status = sip_status;
    if (reason != nullptr) std::strncpy(notification.status.reason, reason, max_reason_length);
    ++notification_size_;
}

bool PjsuaAccountManager::IsFailure(RegistrationState registration) noexcept {
    return registration == RegistrationState::transport_failed ||
           registration == RegistrationState::authentication_failed;
}

bool PjsuaAccountManager::IsGuaranteed(RegistrationState registration) noexcept {
    return IsFailure(registration) || registration == RegistrationState::retry_wait;
}

void PjsuaAccountManager::RemoveNotification(std::size_t offset) noexcept {
    if (offset >= notification_size_) return;
    for (std::size_t index = offset; index + 1 < notification_size_; ++index)
        notifications_[(notification_head_ + index) % notification_capacity] =
            notifications_[(notification_head_ + index + 1) % notification_capacity];
    --notification_size_;
}

void PjsuaAccountManager::RemoveRetryWait(AgentHandle agent) noexcept {
    for (std::size_t offset = 0; offset < notification_size_; ++offset) {
        const PjsuaRegistrationNotification &notification =
            notifications_[(notification_head_ + offset) % notification_capacity];
        if (notification.registration == RegistrationState::retry_wait &&
            notification.agent.slot == agent.slot &&
            notification.agent.generation == agent.generation) {
            RemoveNotification(offset);
            return;
        }
    }
}

bool PjsuaAccountManager::TryGetNotification(PjsuaRegistrationNotification *notification) noexcept {
    AssertActor();
    if (notification == nullptr || notification_size_ == 0) return false;
    *notification = notifications_[notification_head_];
    notification_head_ = (notification_head_ + 1) % notification_capacity;
    --notification_size_;
    return true;
}

void PjsuaAccountManager::OnRegistrationStarted(const PjsuaRegistrationRecord &record) noexcept {
    AssertActor();
    PjsuaAccountContext *context = Resolve(record.account);
    if (context == nullptr || !context->register_on_start) return;
    context->registration = (record.unregistration || !record.renew)
        ? RegistrationState::unregistering : RegistrationState::registering;
    Notify(*context, context->registration, Error::ok, 0, "");
}

void PjsuaAccountManager::ScheduleRetry(PjsuaAccountContext &context,
                                        const PjsuaRegistrationRecord &record) noexcept {
    static constexpr std::uint64_t base_delays_ms[] = {1000, 2000, 4000, 8000, 16000, 30000, 30000};
    const std::size_t index = context.retry.attempt < 7 ? context.retry.attempt : 6;
    const std::uint64_t jitter = static_cast<std::uint64_t>(context.agent.slot) * 50;
    context.retry.due_ms = AddSaturated(now_ms_, AddSaturated(base_delays_ms[index], jitter));
    context.retry.scheduled = true;
    context.registration = RegistrationState::retry_wait;
    RemoveRetryWait(context.agent);
    Notify(context, RegistrationState::retry_wait, Error::signaling_failed,
           record.sip_status > 0 ? static_cast<std::uint16_t>(record.sip_status) : 0,
           record.reason);
}

void PjsuaAccountManager::OnRegistrationState(const PjsuaRegistrationRecord &record) noexcept {
    AssertActor();
    PjsuaAccountContext *context = Resolve(record.account);
    if (context == nullptr) return;
    const std::uint16_t sip = record.sip_status > 0 ? static_cast<std::uint16_t>(record.sip_status) : 0;
    if (record.native_status == PJ_SUCCESS && record.sip_status >= 200 && record.sip_status < 300) {
        context->retry = {};
        if (record.unregistration || !record.renew || record.expiration == 0) {
            context->registration = RegistrationState::disabled;
            context->refresh_due_ms = 0;
            Notify(*context, RegistrationState::disabled, Error::ok, sip, record.reason);
        } else {
            context->registration = RegistrationState::registered;
            context->refresh_due_ms = AddSaturated(now_ms_, record.expiration > 1
                ? static_cast<std::uint64_t>(record.expiration - 1) * 1000 : 0);
            Notify(*context, RegistrationState::registered, Error::ok, sip, record.reason);
        }
        return;
    }
    if (record.sip_status == 401 || record.sip_status == 407 || record.sip_status == 403) {
        context->retry = {};
        context->refresh_due_ms = 0;
        context->registration = RegistrationState::authentication_failed;
        Notify(*context, RegistrationState::authentication_failed, Error::authentication_failed, sip, record.reason);
        return;
    }
    const Error error = Recoverable(record) ? Error::signaling_failed : Error::remote_rejected;
    if (!Recoverable(record)) context->retry = {};
    context->refresh_due_ms = 0;
    context->registration = RegistrationState::transport_failed;
    Notify(*context, RegistrationState::transport_failed, error, sip, record.reason);
    if (Recoverable(record)) ScheduleRetry(*context, record);
}

Error PjsuaAccountManager::Pump(std::uint64_t now_ms) noexcept {
    AssertActor();
    now_ms_ = now_ms;
    for (PjsuaAccountContext &context : contexts_) {
        if (!context.occupied) continue;
        if (context.refresh_due_ms != 0 && now_ms >= context.refresh_due_ms) {
            context.refresh_due_ms = 0;
            context.registration = RegistrationState::refreshing;
            Notify(context, RegistrationState::refreshing, Error::ok, 0, "");
        }
        if (!context.retry.scheduled || now_ms < context.retry.due_ms) continue;
        context.retry.scheduled = false;
        if (context.retry.attempt != std::numeric_limits<std::uint8_t>::max()) ++context.retry.attempt;
        context.registration = RegistrationState::registering;
        Notify(context, RegistrationState::registering, Error::ok, 0, "");
        const pj_status_t status = api_.acc_set_registration(context.account_id, PJ_TRUE);
        if (status != PJ_SUCCESS) {
            PjsuaRegistrationRecord failure{};
            failure.account = context.account_id;
            failure.native_status = status;
            std::strncpy(failure.reason, "registration start failed", max_reason_length);
            OnRegistrationState(failure);
        }
    }
    return Error::ok;
}

Error PjsuaAccountManager::Shutdown() noexcept {
    AssertActor();
    Rollback();
    actor_thread_ = {};
    return Error::ok;
}

} // namespace voip
