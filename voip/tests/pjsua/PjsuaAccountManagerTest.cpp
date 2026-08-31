#include "FakePjsuaApi.hpp"
#include "../../src/core/AgentRegistry.hpp"
#include "../../src/pjsua/PjsuaAccountManager.hpp"

#include <cassert>
#include <cstring>
#include <type_traits>
#include <zephyr/sys/printk.h>

namespace voip::test {
namespace {
class Source final : public PcmSource { public: PcmFormat Format() const noexcept override { return {8000, 160, 1, SampleFormat::signed_16}; } Error Read(std::int16_t *, std::size_t, std::uint64_t) noexcept override { return Error::ok; } };
class Sink final : public PcmSink { public: PcmFormat Format() const noexcept override { return {8000, 160, 1, SampleFormat::signed_16}; } Error Write(const std::int16_t *, std::size_t, std::uint64_t) noexcept override { return Error::ok; } void Flush() noexcept override {} };
struct Fixture {
    Source sources[5]{}; Sink sinks[5]{}; AgentConfig agents[5]{};
    ServiceConfig config{agents, 5, 1000, 5000, {8000, 160, 1, SampleFormat::signed_16}, {SignalingSecurity::none, MediaSecurity::none}};
    Fixture() noexcept { for (std::size_t i = 0; i < 5; ++i) agents[i] = {{i == 0 ? "sip:zero@example.test" : i == 1 ? "sip:one@example.test" : i == 2 ? "sip:two@example.test" : i == 3 ? "sip:three@example.test" : "sip:four@example.test", "sip:registrar.example.test", i == 0 ? "zero" : i == 1 ? "one" : i == 2 ? "two" : i == 3 ? "three" : "four", i == 0 ? "password-zero" : i == 1 ? "password-one" : i == 2 ? "password-two" : i == 3 ? "password-three" : "password-four"}, {&sources[i], &sinks[i]}, i != 4}; }
    Error Initialize(AgentRegistry &registry) noexcept { return registry.Initialize(config); }
};
void CheckConfig(const pjsua_acc_config &cfg, const char *identity, const char *username, const char *password, const PjsuaAccountContext *context) {
    const char *registrar = "sip:registrar.example.test";
    assert(cfg.id.slen == static_cast<pj_ssize_t>(std::strlen(identity)) && std::memcmp(cfg.id.ptr, identity, cfg.id.slen) == 0);
    assert(cfg.reg_uri.slen == static_cast<pj_ssize_t>(std::strlen(registrar)) && std::memcmp(cfg.reg_uri.ptr, registrar, cfg.reg_uri.slen) == 0);
    assert(cfg.cred_count == 1 && cfg.cred_info[0].data_type == PJSIP_CRED_DATA_PLAIN_PASSWD);
    assert(cfg.cred_info[0].realm.slen == 1 && std::memcmp(cfg.cred_info[0].realm.ptr, "*", 1) == 0);
    assert(cfg.cred_info[0].scheme.slen == 6 && std::memcmp(cfg.cred_info[0].scheme.ptr, "Digest", 6) == 0);
    assert(cfg.cred_info[0].username.slen == static_cast<pj_ssize_t>(std::strlen(username)) && std::memcmp(cfg.cred_info[0].username.ptr, username, cfg.cred_info[0].username.slen) == 0);
    assert(cfg.cred_info[0].data.slen == static_cast<pj_ssize_t>(std::strlen(password)) && std::memcmp(cfg.cred_info[0].data.ptr, password, cfg.cred_info[0].data.slen) == 0);
    assert(cfg.transport_id == 4 && cfg.user_data == context && cfg.register_on_acc_add == PJ_FALSE);
    assert(cfg.allow_contact_rewrite == PJ_FALSE && cfg.contact_use_src_port == PJ_FALSE &&
           cfg.allow_via_rewrite == PJ_FALSE && cfg.allow_sdp_nat_rewrite == PJ_FALSE &&
           cfg.use_rfc5626 == PJ_FALSE);
    assert(cfg.sip_stun_use == PJSUA_STUN_USE_DISABLED && cfg.media_stun_use == PJSUA_STUN_USE_DISABLED);
    assert(cfg.sip_upnp_use == PJSUA_UPNP_USE_DISABLED && cfg.media_upnp_use == PJSUA_UPNP_USE_DISABLED);
    assert(cfg.ice_cfg_use != PJSUA_ICE_CONFIG_USE_CUSTOM && cfg.turn_cfg_use != PJSUA_TURN_CONFIG_USE_CUSTOM);
    assert(cfg.use_srtp == PJMEDIA_SRTP_DISABLED && cfg.reg_retry_interval == 0 && cfg.reg_first_retry_interval == 0 && cfg.reg_retry_random_interval == 0);
}
static_assert(!std::is_convertible<PjsuaAccountContext, OwnedSipAccountConfig>::value, "account context must not own copied SIP configuration");
static_assert(!std::is_pointer<decltype(PjsuaAccountContext{}.agent)>::value, "account context must retain only an AgentHandle");
}

void RunPjsuaAccountManagerTests() {
    Fixture fixture; AgentRegistry registry; assert(fixture.Initialize(registry) == Error::ok);
    FakePjsuaApi fake; PjsuaAccountManager accounts(fake.Api());
    assert(accounts.Initialize(registry, 4) == Error::ok && accounts.Count() == 5);
    const char *identities[] = {"sip:zero@example.test", "sip:one@example.test", "sip:two@example.test", "sip:three@example.test", "sip:four@example.test"};
    const char *users[] = {"zero", "one", "two", "three", "four"}; const char *passwords[] = {"password-zero", "password-one", "password-two", "password-three", "password-four"};
    for (std::uint8_t i = 0; i < 5; ++i) { AgentHandle h{}; assert(registry.GetAgentHandle(i, &h) == Error::ok); pjsua_acc_id id{}; assert(accounts.NativeId(h, &id) == Error::ok); const PjsuaAccountContext *context = accounts.Resolve(id); assert(context != nullptr && context->agent.slot == i); CheckConfig(fake.AccountConfig(i), identities[i], users[i], passwords[i], context); }
    const std::size_t lookup_calls = fake.AccountGetUserDataCount();
    assert(accounts.Resolve(99) == nullptr && fake.AccountGetUserDataCount() == lookup_calls);
    const PjsuaAccountContext *context_two = accounts.Resolve(2);
    const PjsuaAccountContext *context_zero = accounts.Resolve(0);
    assert(context_two != nullptr && context_zero != nullptr);
    fake.SetAccountUserData(2, const_cast<PjsuaAccountContext *>(context_zero));
    assert(accounts.Resolve(2) == nullptr);
    PjsuaAccountContext foreign{};
    fake.SetAccountUserData(2, &foreign);
    assert(accounts.Resolve(2) == nullptr);
    fake.SetAccountUserData(2, const_cast<PjsuaAccountContext *>(context_two));
    assert(accounts.Resolve(2) == context_two);
    fake.SetAccountUserData(2, nullptr);
    assert(accounts.Resolve(2) == nullptr);
    assert(accounts.StartInitialRegistration() == Error::ok && fake.RegistrationCount() == 4);
    assert(accounts.Shutdown() == Error::ok && accounts.Count() == 0 && accounts.Resolve(2) == nullptr);

    FakePjsuaApi rollback_fake; rollback_fake.FailAccountAdd(3); PjsuaAccountManager rollback(rollback_fake.Api());
    assert(rollback.Initialize(registry, 4) != Error::ok && rollback.Count() == 0 && rollback_fake.AccountDeleteCount() == 2);
    assert(rollback_fake.DeletedAccount(0) == 0 && rollback_fake.DeletedAccount(1) == 2);
    assert(rollback_fake.ClearedAccount(0) == 0 && rollback_fake.ClearedAccount(1) == 2);

    const pjsua_acc_id duplicate[] = {2, 2}; FakePjsuaApi duplicate_fake; duplicate_fake.SetAccountIds(duplicate, 2); PjsuaAccountManager duplicate_accounts(duplicate_fake.Api());
    fixture.config.agent_count = 2; assert(fixture.Initialize(registry) == Error::ok); assert(duplicate_accounts.Initialize(registry, 4) != Error::ok && duplicate_accounts.Count() == 0);
    assert(duplicate_fake.AccountClearCount() == 1 && duplicate_fake.AccountDeleteCount() == 1);
    assert(duplicate_fake.ClearedAccount(0) == 2 && duplicate_fake.DeletedAccount(0) == 2);

    const pjsua_acc_id unknown[] = {PJSUA_INVALID_ID}; FakePjsuaApi unknown_fake; unknown_fake.SetAccountIds(unknown, 1); PjsuaAccountManager unknown_accounts(unknown_fake.Api());
    fixture.config.agent_count = 1; assert(fixture.Initialize(registry) == Error::ok); assert(unknown_accounts.Initialize(registry, 4) != Error::ok && unknown_accounts.Count() == 0);
    assert(unknown_fake.AccountClearCount() == 0 && unknown_fake.AccountDeleteCount() == 0);

    const pjsua_acc_id malformed_after_valid[] = {2, 0, PJSUA_INVALID_ID}; FakePjsuaApi malformed_fake; malformed_fake.SetAccountIds(malformed_after_valid, 3); PjsuaAccountManager malformed_accounts(malformed_fake.Api());
    fixture.config.agent_count = 3; assert(fixture.Initialize(registry) == Error::ok); assert(malformed_accounts.Initialize(registry, 4) != Error::ok && malformed_accounts.Count() == 0);
    assert(malformed_fake.AccountClearCount() == 2 && malformed_fake.AccountDeleteCount() == 2);
    assert(malformed_fake.ClearedAccount(0) == 0 && malformed_fake.ClearedAccount(1) == 2);
    assert(malformed_fake.DeletedAccount(0) == 0 && malformed_fake.DeletedAccount(1) == 2);

    fixture.config.agent_count = 5; assert(fixture.Initialize(registry) == Error::ok); FakePjsuaApi capacity_fake; PjsuaAccountManager capacity(capacity_fake.Api());
    assert(capacity.Initialize(registry, 4) == Error::ok); assert(capacity.Initialize(registry, 4) == Error::invalid_state); assert(capacity.Resolve(PJSUA_INVALID_ID) == nullptr);
    assert(capacity.Shutdown() == Error::ok);
    assert(sizeof(PjsuaAccountManager) < 4096);
    assert(sizeof(PjsuaAccountContext) < 128);
    assert(fake.AccountAddCount() == 5);
    (void)sizeof(PjsuaAccountContext);
    // No PcmSource/PcmSink/OwnedSipAccountConfig or credential arrays are members: the fixed context type above is deliberately inspected by its public shape.
    static_assert(std::is_standard_layout<PjsuaAccountContext>::value, "context remains a simple bounded record");
    printk("PjsuaAccountManagerTest PASSED\n");
}

} // namespace voip::test
