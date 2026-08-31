#include "PjsuaAccountManager.hpp"

#include <cassert>
#include <cstring>

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

Error PjsuaAccountManager::Shutdown() noexcept {
    AssertActor();
    Rollback();
    actor_thread_ = {};
    return Error::ok;
}

} // namespace voip
