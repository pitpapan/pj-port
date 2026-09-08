#include "ScriptedRegistrar.hpp"

#include <pjsip.h>
#include <pjsip/sip_auth.h>
#include <zephyr/sys/atomic.h>

namespace {

using voip_integration::ScriptedRegistrar;

struct RegistrarState {
    pjsip_endpoint *endpoint{};
    pj_pool_t *pool{};
    pjsip_auth_srv auth{};
    atomic_t requests{};
    atomic_t tcp_requests{};
    atomic_t non_tcp_requests{};
    atomic_t challenges{};
    atomic_t authorized{};
    atomic_t forbidden{};
    atomic_t registrations{};
    atomic_t unregistrations{};
    atomic_t callback_failures{};
    atomic_t registrations_by_agent[5]{};
    atomic_t timeout_agent_1{};
    atomic_t timeout_agent_3{};
    atomic_t short_expiration_agents{};
};

RegistrarState state;

const char *ExpectedPassword(const pj_str_t *account) {
    if (pj_strcmp2(account, "agent0") == 0) return "valid-0";
    if (pj_strcmp2(account, "agent1") == 0) return "valid-1";
    if (pj_strcmp2(account, "agent2") == 0) return "valid-2";
    if (pj_strcmp2(account, "agent3") == 0) return "valid-3";
    if (pj_strcmp2(account, "agent4") == 0) return "valid-4";
    return nullptr;
}

pj_status_t LookupCredential(pj_pool_t *, const pj_str_t *realm,
                             const pj_str_t *account,
                             pjsip_cred_info *credential) {
    const char *password = ExpectedPassword(account);
    if (password == nullptr) return PJSIP_EAUTHACCNOTFOUND;
    pj_bzero(credential, sizeof(*credential));
    credential->realm = *realm;
    credential->scheme = pj_str(const_cast<char *>("digest"));
    credential->username = *account;
    credential->data_type = PJSIP_CRED_DATA_PLAIN_PASSWD;
    credential->data = pj_str(const_cast<char *>(password));
    return PJ_SUCCESS;
}

unsigned RequestExpiration(pjsip_rx_data *data) {
    auto *expires = static_cast<pjsip_expires_hdr *>(pjsip_msg_find_hdr(
        data->msg_info.msg, PJSIP_H_EXPIRES, nullptr));
    if (expires != nullptr) return static_cast<unsigned>(expires->ivalue);
    auto *contact = static_cast<pjsip_contact_hdr *>(pjsip_msg_find_hdr(
        data->msg_info.msg, PJSIP_H_CONTACT, nullptr));
    return contact != nullptr && contact->expires != PJSIP_EXPIRES_NOT_SPECIFIED
        ? static_cast<unsigned>(contact->expires) : 3U;
}

unsigned AgentIndex(pjsip_rx_data *data) {
    constexpr unsigned invalid_index = 99;
    if (data == nullptr || data->msg_info.msg == nullptr) return invalid_index;
    const auto *authorization = static_cast<const pjsip_authorization_hdr *>(
        pjsip_msg_find_hdr(data->msg_info.msg, PJSIP_H_AUTHORIZATION, nullptr));
    if (authorization == nullptr || authorization->type != PJSIP_H_AUTHORIZATION ||
        authorization->scheme.ptr == nullptr || authorization->scheme.slen != 6 ||
        pj_stricmp2(&authorization->scheme, "Digest") != 0)
        return invalid_index;
    const pj_str_t &username = authorization->credential.digest.username;
    if (username.ptr == nullptr || username.slen != 6 ||
        pj_strncmp2(&username, "agent", 5) != 0)
        return invalid_index;
    const char suffix = username.ptr[5];
    return suffix >= '0' && suffix <= '4' ? static_cast<unsigned>(suffix - '0')
                                             : invalid_index;
}

bool Consume408(unsigned index) {
    atomic_t *armed = index == 1 ? &state.timeout_agent_1
                    : index == 3 ? &state.timeout_agent_3 : nullptr;
    return armed != nullptr && atomic_cas(armed, 1, 0);
}

pj_bool_t OnRequest(pjsip_rx_data *data);

pjsip_module registrar_module = {
    .name = {const_cast<char *>("voip-scripted-registrar"), 23},
    .id = -1,
    .priority = PJSIP_MOD_PRIORITY_APPLICATION,
    .on_rx_request = OnRequest,
};

pj_status_t SendResponse(pjsip_rx_data *data, int code, bool challenge,
                         unsigned expiration) {
    pjsip_transaction *transaction = nullptr;
    pjsip_tx_data *response = nullptr;
    pj_status_t status = pjsip_tsx_create_uas(&registrar_module, data, &transaction);
    if (status == PJ_SUCCESS) pjsip_tsx_recv_msg(transaction, data);
    if (status == PJ_SUCCESS)
        status = pjsip_endpt_create_response(state.endpoint, data, code, nullptr, &response);
    if (status == PJ_SUCCESS && challenge) {
        pj_str_t qop = pj_str(const_cast<char *>("auth"));
        pj_str_t nonce = pj_str(const_cast<char *>("plan3-fixed-nonce"));
        pj_str_t opaque = pj_str(const_cast<char *>("plan3-opaque"));
        status = pjsip_auth_srv_challenge(&state.auth, &qop, &nonce, &opaque,
                                          PJ_FALSE, response);
    }
    if (status == PJ_SUCCESS && code == 200) {
        auto *expires = pjsip_expires_hdr_create(response->pool, expiration);
        pjsip_msg_add_hdr(response->msg, reinterpret_cast<pjsip_hdr *>(expires));
        auto *request_contact = static_cast<pjsip_contact_hdr *>(pjsip_msg_find_hdr(
            data->msg_info.msg, PJSIP_H_CONTACT, nullptr));
        if (request_contact != nullptr) {
            auto *contact = reinterpret_cast<pjsip_contact_hdr *>(pjsip_hdr_clone(
                response->pool, reinterpret_cast<pjsip_hdr *>(request_contact)));
            contact->expires = static_cast<int>(expiration);
            pjsip_msg_add_hdr(response->msg, reinterpret_cast<pjsip_hdr *>(contact));
        }
    }
    if (status == PJ_SUCCESS) status = pjsip_tsx_send_msg(transaction, response);
    return status;
}

pj_bool_t OnRequest(pjsip_rx_data *data) {
    if (data == nullptr || data->msg_info.msg == nullptr || state.endpoint == nullptr ||
        data->msg_info.msg->line.req.method.id != PJSIP_REGISTER_METHOD)
        return PJ_FALSE;
    atomic_inc(&state.requests);
    if (data->tp_info.transport == nullptr ||
        data->tp_info.transport->key.type != PJSIP_TRANSPORT_TCP) {
        atomic_inc(&state.non_tcp_requests);
        return PJ_FALSE;
    }
    atomic_inc(&state.tcp_requests);

    int code = 200;
    const unsigned agent_index = AgentIndex(data);
    pj_status_t status = pjsip_auth_srv_verify(&state.auth, data, &code);
    if (status == PJSIP_EAUTHNOAUTH) {
        atomic_inc(&state.challenges);
        status = SendResponse(data, 401, true, 0);
    } else if (status != PJ_SUCCESS) {
        // Deliberately use a final rejection so the service must never retry it.
        atomic_inc(&state.forbidden);
        status = SendResponse(data, 403, false, 0);
    } else if (Consume408(agent_index)) {
        status = SendResponse(data, 408, false, 0);
    } else {
        unsigned expiration = RequestExpiration(data);
        if (expiration != 0 && agent_index < 5 &&
            (atomic_get(&state.short_expiration_agents) & (1 << agent_index)) != 0)
            expiration = 3;
        atomic_inc(&state.authorized);
        atomic_inc(expiration == 0 ? &state.unregistrations : &state.registrations);
        if (expiration != 0 && agent_index < 5)
            atomic_inc(&state.registrations_by_agent[agent_index]);
        status = SendResponse(data, 200, false, expiration);
    }
    if (status != PJ_SUCCESS) atomic_inc(&state.callback_failures);
    return PJ_TRUE;
}

} // namespace

namespace voip_integration {

ScriptedRegistrar &ScriptedRegistrar::Instance() noexcept {
    static ScriptedRegistrar registrar;
    return registrar;
}

void ScriptedRegistrar::Reset() noexcept {
    state = {};
    registrar_module.id = -1;
}

void ScriptedRegistrar::ArmOne408(unsigned agent_index) noexcept {
    if (agent_index == 1) atomic_set(&state.timeout_agent_1, 1);
    if (agent_index == 3) atomic_set(&state.timeout_agent_3, 1);
}

void ScriptedRegistrar::UseShortSuccessfulExpiration(unsigned agent_index) noexcept {
    // This only changes the protocol response. Native PJSUA callbacks remain
    // the sole source of service registration state.
    if (agent_index < 5)
        atomic_set(&state.short_expiration_agents,
                   atomic_get(&state.short_expiration_agents) | (1 << agent_index));
}

RegistrarStats ScriptedRegistrar::Stats() const noexcept {
    return {static_cast<unsigned>(atomic_get(&state.requests)),
            static_cast<unsigned>(atomic_get(&state.tcp_requests)),
            static_cast<unsigned>(atomic_get(&state.non_tcp_requests)),
            static_cast<unsigned>(atomic_get(&state.challenges)),
            static_cast<unsigned>(atomic_get(&state.authorized)),
            static_cast<unsigned>(atomic_get(&state.forbidden)),
            static_cast<unsigned>(atomic_get(&state.registrations)),
            static_cast<unsigned>(atomic_get(&state.unregistrations)),
            static_cast<unsigned>(atomic_get(&state.callback_failures)),
            {static_cast<unsigned>(atomic_get(&state.registrations_by_agent[0])),
             static_cast<unsigned>(atomic_get(&state.registrations_by_agent[1])),
             static_cast<unsigned>(atomic_get(&state.registrations_by_agent[2])),
             static_cast<unsigned>(atomic_get(&state.registrations_by_agent[3])),
             static_cast<unsigned>(atomic_get(&state.registrations_by_agent[4]))}};
}

} // namespace voip_integration

extern "C" unsigned voip_pjsua_registration_test_listener_port() noexcept {
    return ScriptedRegistrar::listener_port;
}

extern "C" pj_status_t voip_pjsua_registration_test_install(
    pjsip_endpoint *endpoint) noexcept {
    if (endpoint == nullptr || registrar_module.id != -1) return PJ_EINVALIDOP;
    state.endpoint = endpoint;
    state.pool = pjsip_endpt_create_pool(endpoint, "p3-reg", 4096, 4096);
    if (state.pool == nullptr) return PJ_ENOMEM;
    pj_str_t realm = pj_str(const_cast<char *>("plan3"));
    pj_status_t status = pjsip_auth_srv_init(state.pool, &state.auth, &realm,
                                             LookupCredential, 0);
    if (status == PJ_SUCCESS) status = pjsip_endpt_register_module(endpoint, &registrar_module);
    if (status != PJ_SUCCESS) {
        pjsip_endpt_release_pool(endpoint, state.pool);
        state.pool = nullptr;
        state.endpoint = nullptr;
    }
    return status;
}
