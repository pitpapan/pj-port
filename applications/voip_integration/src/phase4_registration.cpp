#include <voip/PjVoipBackend.hpp>

#include <pjsip.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>

namespace {

constexpr unsigned wait_ms = 3000;
constexpr unsigned refresh_wait_ms = 7000;
constexpr unsigned registration_expiry = 4;

int failures;
atomic_t callback_failures;

#define CHECK(condition) do {                                                   \
    if (!(condition)) {                                                         \
        printk("[Phase 4] FAIL line %d: %s\n", __LINE__, #condition);          \
        ++failures;                                                             \
    }                                                                           \
} while (false)

struct Registrar {
    pjsip_endpoint *endpoint{};
    pj_pool_t *pool{};
    pjsip_auth_srv auth{};
    atomic_t requests{};
    atomic_t challenges{};
    atomic_t authorized{};
    atomic_t registrations{};
    atomic_t unregistrations{};
    atomic_t tcp_requests{};
    atomic_t tcp_register_requests{};
    atomic_t rejected{};
    atomic_t fragmented{};
    atomic_t coalesced{};
    atomic_t timeout_next{};
    atomic_t close_before_auth{};
    atomic_t close_after_auth{};
};

Registrar *active_registrar;

pj_status_t LookupCredential(pj_pool_t *, const pj_str_t *realm,
                             const pj_str_t *account,
                             pjsip_cred_info *credential) {
    if (pj_strcmp2(account, "alice") != 0) return PJSIP_EAUTHACCNOTFOUND;
    pj_bzero(credential, sizeof(*credential));
    credential->realm = *realm;
    credential->scheme = pj_str(const_cast<char *>("digest"));
    credential->username = *account;
    credential->data_type = PJSIP_CRED_DATA_PLAIN_PASSWD;
    credential->data = pj_str(const_cast<char *>("phase4-secret"));
    return PJ_SUCCESS;
}

unsigned RequestExpiration(pjsip_rx_data *data) {
    auto *expires = static_cast<pjsip_expires_hdr *>(pjsip_msg_find_hdr(
        data->msg_info.msg, PJSIP_H_EXPIRES, nullptr));
    if (expires != nullptr) return static_cast<unsigned>(expires->ivalue);
    auto *contact = static_cast<pjsip_contact_hdr *>(pjsip_msg_find_hdr(
        data->msg_info.msg, PJSIP_H_CONTACT, nullptr));
    if (contact != nullptr && contact->expires != PJSIP_EXPIRES_NOT_SPECIFIED)
        return static_cast<unsigned>(contact->expires);
    return registration_expiry;
}

pj_status_t SendResponse(Registrar &registrar, pjsip_rx_data *data, int code,
                         bool challenge, unsigned expiration);
pj_bool_t OnRequest(pjsip_rx_data *data);

pjsip_module registrar_module = {
    .name = {const_cast<char *>("voip-phase4-registrar"), 21},
    .id = -1,
    .priority = PJSIP_MOD_PRIORITY_APPLICATION,
    .on_rx_request = OnRequest,
};

pj_status_t SendResponse(Registrar &registrar, pjsip_rx_data *data, int code,
                         bool challenge, unsigned expiration) {
    pjsip_transaction *transaction = nullptr;
    pjsip_tx_data *response = nullptr;
    pj_status_t status = pjsip_tsx_create_uas(&registrar_module, data,
                                               &transaction);
    if (status != PJ_SUCCESS) return status;
    pjsip_tsx_recv_msg(transaction, data);
    status = pjsip_endpt_create_response(registrar.endpoint, data, code, nullptr,
                                         &response);
    if (status != PJ_SUCCESS) return status;
    if (challenge) {
        pj_str_t qop = pj_str(const_cast<char *>("auth"));
        pj_str_t nonce = pj_str(const_cast<char *>("phase4-fixed-nonce"));
        pj_str_t opaque = pj_str(const_cast<char *>("phase4-opaque"));
        status = pjsip_auth_srv_challenge(&registrar.auth, &qop, &nonce,
                                          &opaque, PJ_FALSE, response);
    }
    if (status == PJ_SUCCESS && code == 200) {
        auto *expires = pjsip_expires_hdr_create(response->pool, expiration);
        pjsip_msg_add_hdr(response->msg, reinterpret_cast<pjsip_hdr *>(expires));
        auto *request_contact = static_cast<pjsip_contact_hdr *>(
            pjsip_msg_find_hdr(data->msg_info.msg, PJSIP_H_CONTACT, nullptr));
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
    Registrar *registrar = active_registrar;
    if (registrar == nullptr || data->msg_info.msg == nullptr)
        return PJ_FALSE;
    if (data->msg_info.msg->line.req.method.id == PJSIP_OPTIONS_METHOD) {
        const pj_str_t &id = data->msg_info.cid->id;
        if (pj_strcmp2(&id, "phase4-fragment") == 0)
            atomic_inc(&registrar->fragmented);
        else if (pj_strncmp2(&id, "phase4-batch-", 13) == 0)
            atomic_inc(&registrar->coalesced);
        else
            return PJ_FALSE;
        if (data->tp_info.transport->key.type == PJSIP_TRANSPORT_TCP)
            atomic_inc(&registrar->tcp_requests);
        if (pjsip_endpt_respond_stateless(registrar->endpoint, data, 200,
                                           nullptr, nullptr, nullptr) != PJ_SUCCESS)
            atomic_inc(&callback_failures);
        return PJ_TRUE;
    }
    if (data->msg_info.msg->line.req.method.id != PJSIP_REGISTER_METHOD)
        return PJ_FALSE;
    atomic_inc(&registrar->requests);
    if (data->tp_info.transport->key.type == PJSIP_TRANSPORT_TCP) {
        atomic_inc(&registrar->tcp_requests);
        atomic_inc(&registrar->tcp_register_requests);
    }
    if (atomic_cas(&registrar->timeout_next, 1, 0)) {
        if (SendResponse(*registrar, data, 408, false, 0) != PJ_SUCCESS)
            atomic_inc(&callback_failures);
        return PJ_TRUE;
    }
    if (atomic_cas(&registrar->close_before_auth, 1, 0)) {
        (void)pjsip_transport_shutdown(data->tp_info.transport);
        return PJ_TRUE;
    }
    int response_code = 200;
    pj_status_t status = pjsip_auth_srv_verify(&registrar->auth, data,
                                                &response_code);
    if (status == PJSIP_EAUTHNOAUTH) {
        atomic_inc(&registrar->challenges);
        status = SendResponse(*registrar, data, 401, true, 0);
    } else if (status != PJ_SUCCESS) {
        atomic_inc(&registrar->rejected);
        status = SendResponse(*registrar, data, response_code, false, 0);
    } else {
        const unsigned expiration = RequestExpiration(data);
        atomic_inc(&registrar->authorized);
        if (atomic_cas(&registrar->close_after_auth, 1, 0)) {
            (void)pjsip_transport_shutdown(data->tp_info.transport);
            return PJ_TRUE;
        }
        atomic_inc(expiration == 0 ? &registrar->unregistrations
                                   : &registrar->registrations);
        status = SendResponse(*registrar, data, 200, false, expiration);
    }
    if (status != PJ_SUCCESS) atomic_inc(&callback_failures);
    return PJ_TRUE;
}

class Observer final : public voip::Observer {
public:
    atomic_t registering{};
    atomic_t registered{};
    atomic_t unregistering{};
    atomic_t disabled{};
    atomic_t failed{};
    atomic_t connection_lost{};
    atomic_t last_code{};

    void OnRegistrationState(voip::RegistrationState state,
                             const voip::Status &status) override {
        atomic_set(&last_code, status.sip_status);
        switch (state) {
        case voip::RegistrationState::registering: atomic_inc(&registering); break;
        case voip::RegistrationState::registered: atomic_inc(&registered); break;
        case voip::RegistrationState::unregistering: atomic_inc(&unregistering); break;
        case voip::RegistrationState::disabled: atomic_inc(&disabled); break;
        case voip::RegistrationState::failed: atomic_inc(&failed); break;
        case voip::RegistrationState::connection_lost:
            atomic_inc(&connection_lost); break;
        default: break;
        }
    }
};

bool WaitFor(atomic_t &value, atomic_val_t expected, unsigned timeout) {
    for (unsigned elapsed = 0; elapsed < timeout; elapsed += 10) {
        if (atomic_get(&value) >= expected) return true;
        k_msleep(10);
    }
    return false;
}

pj_status_t SendExact(pj_sock_t socket, const char *data, pj_size_t length) {
    pj_size_t total = 0;
    while (total < length) {
        pj_ssize_t sent = static_cast<pj_ssize_t>(length - total);
        pj_status_t status = pj_sock_send(socket, data + total, &sent, 0);
        if (status != PJ_SUCCESS || sent <= 0)
            return status == PJ_SUCCESS ? PJ_EUNKNOWN : status;
        total += static_cast<pj_size_t>(sent);
    }
    return PJ_SUCCESS;
}

int MakeOptions(char *buffer, pj_size_t capacity, unsigned port,
                const char *call_id, unsigned sequence, const char *body) {
    const unsigned body_length = body == nullptr ? 0 : pj_ansi_strlen(body);
    return pj_ansi_snprintf(
        buffer, capacity,
        "OPTIONS sip:service@127.0.0.1:%u;transport=tcp SIP/2.0\r\n"
        "Via: SIP/2.0/TCP 127.0.0.1:9;branch=" PJSIP_RFC3261_BRANCH_ID "-%s\r\n"
        "From: <sip:raw@127.0.0.1>;tag=%u\r\n"
        "To: <sip:service@127.0.0.1>\r\nCall-ID: %s\r\n"
        "CSeq: %u OPTIONS\r\nMax-Forwards: 70\r\n"
        "Content-Length: %u\r\n\r\n%s",
        port, call_id, sequence, call_id, sequence, body_length,
        body == nullptr ? "" : body);
}

void TestTcpFraming(Registrar &registrar, unsigned port) {
    pj_sock_t socket = PJ_INVALID_SOCKET;
    pj_sockaddr_in address;
    pj_str_t loopback = pj_str(const_cast<char *>("127.0.0.1"));
    char fragment[768];
    char first[768];
    char second[768];
    char batch[1536];
    const char body[] = "fragmented-body";
    CHECK(pj_sock_socket(pj_AF_INET(), pj_SOCK_STREAM(), 0, &socket) == PJ_SUCCESS);
    CHECK(pj_sockaddr_in_init(&address, &loopback, static_cast<pj_uint16_t>(port)) ==
          PJ_SUCCESS);
    CHECK(pj_sock_connect(socket, &address, sizeof(address)) == PJ_SUCCESS);
    const int fragment_length = MakeOptions(fragment, sizeof(fragment), port,
                                             "phase4-fragment", 701, body);
    CHECK(fragment_length > 40);
    CHECK(SendExact(socket, fragment, 19) == PJ_SUCCESS);
    k_msleep(20);
    CHECK(atomic_get(&registrar.fragmented) == 0);
    CHECK(SendExact(socket, fragment + 19,
                    static_cast<pj_size_t>(fragment_length - 19)) == PJ_SUCCESS);
    CHECK(WaitFor(registrar.fragmented, 1, wait_ms));

    const int first_length = MakeOptions(first, sizeof(first), port,
                                          "phase4-batch-one", 702, nullptr);
    const int second_length = MakeOptions(second, sizeof(second), port,
                                           "phase4-batch-two", 703, nullptr);
    CHECK(first_length > 0 && second_length > 0 &&
          first_length + second_length < static_cast<int>(sizeof(batch)));
    pj_memcpy(batch, first, first_length);
    pj_memcpy(batch + first_length, second, second_length);
    CHECK(SendExact(socket, batch,
                    static_cast<pj_size_t>(first_length + second_length)) == PJ_SUCCESS);
    CHECK(WaitFor(registrar.coalesced, 2, wait_ms));
    CHECK(pj_sock_close(socket) == PJ_SUCCESS);
}

void Lifecycle(unsigned number) {
    voip::PjVoipBackend backend;
    voip::VoipManager manager(backend);
    Observer observer;
    Registrar registrar;

    CHECK(manager.Initialize(&observer) == voip::Error::ok);
    registrar.endpoint = static_cast<pjsip_endpoint *>(
        backend.NativeSipEndpointForValidation());
    CHECK(registrar.endpoint != nullptr);
    registrar.pool = pjsip_endpt_create_pool(registrar.endpoint, "p4-reg", 4096,
                                              4096);
    CHECK(registrar.pool != nullptr);
    pj_str_t realm = pj_str(const_cast<char *>("phase4"));
    CHECK(pjsip_auth_srv_init(registrar.pool, &registrar.auth, &realm,
                              LookupCredential, 0) == PJ_SUCCESS);
    active_registrar = &registrar;
    CHECK(pjsip_endpt_register_module(registrar.endpoint, &registrar_module) ==
          PJ_SUCCESS);

    TestTcpFraming(registrar, backend.TcpPort());

    char registrar_uri[96];
    pj_ansi_snprintf(registrar_uri, sizeof(registrar_uri),
                     "sip:127.0.0.1:%u;transport=tcp", backend.TcpPort());
    const voip::AccountConfig account{
        "<sip:alice@phase4.test>", registrar_uri, "alice", "phase4-secret",
        registration_expiry};
    CHECK(manager.ConfigureAccount(account) == voip::Error::ok);
    CHECK(manager.RegisterAccount() == voip::Error::ok);
    CHECK(WaitFor(observer.registered, 1, wait_ms));
    CHECK(atomic_get(&observer.registering) == 1);
    CHECK(atomic_get(&observer.last_code) == 200);
    CHECK(atomic_get(&registrar.challenges) == 1);
    CHECK(atomic_get(&registrar.registrations) == 1);

    CHECK(WaitFor(observer.registered, 2, refresh_wait_ms));
    CHECK(atomic_get(&registrar.registrations) == 2);
    CHECK(manager.UnregisterAccount() == voip::Error::ok);
    CHECK(WaitFor(observer.disabled, 1, wait_ms));
    CHECK(atomic_get(&observer.unregistering) == 1);
    CHECK(atomic_get(&registrar.unregistrations) == 1);
    CHECK(atomic_get(&registrar.tcp_register_requests) ==
          atomic_get(&registrar.requests));
    CHECK(atomic_get(&observer.failed) == 0);
    CHECK(atomic_get(&callback_failures) == 0);

    const voip::AccountConfig wrong_password{
        "<sip:alice@phase4.test>", registrar_uri, "alice", "wrong-secret",
        registration_expiry};
    CHECK(manager.ConfigureAccount(wrong_password) == voip::Error::ok);
    CHECK(manager.RegisterAccount() == voip::Error::ok);
    CHECK(WaitFor(observer.failed, 1, wait_ms));
    CHECK(atomic_get(&registrar.rejected) == 1);
    CHECK(atomic_get(&observer.last_code) == 403);

    CHECK(manager.ConfigureAccount(account) == voip::Error::ok);
    atomic_set(&registrar.timeout_next, 1);
    CHECK(manager.RegisterAccount() == voip::Error::ok);
    CHECK(WaitFor(observer.connection_lost, 1, wait_ms));
    CHECK(atomic_get(&observer.last_code) == 408);
    CHECK(WaitFor(observer.registered, 3, wait_ms));

    CHECK(manager.ConfigureAccount(account) == voip::Error::ok);
    atomic_set(&registrar.close_before_auth, 1);
    CHECK(manager.RegisterAccount() == voip::Error::ok);
    CHECK(WaitFor(observer.connection_lost, 2, wait_ms));
    CHECK(WaitFor(observer.registered, 4, wait_ms));

    CHECK(manager.ConfigureAccount(account) == voip::Error::ok);
    atomic_set(&registrar.close_after_auth, 1);
    CHECK(manager.RegisterAccount() == voip::Error::ok);
    CHECK(WaitFor(observer.connection_lost, 3, wait_ms));
    CHECK(WaitFor(observer.registered, 5, wait_ms));

    CHECK(manager.UnregisterAccount() == voip::Error::ok);
    CHECK(WaitFor(observer.disabled, 2, wait_ms));
    CHECK(atomic_get(&callback_failures) == 0);

    k_msleep(100);
    CHECK(pjsip_endpt_unregister_module(registrar.endpoint, &registrar_module) ==
          PJ_SUCCESS);
    active_registrar = nullptr;
    pjsip_endpt_release_pool(registrar.endpoint, registrar.pool);
    CHECK(manager.Shutdown() == voip::Error::ok);
    CHECK(!backend.HasLiveResources());
    printk("[Phase 4] lifecycle %u TCP Digest/refresh/unregister: %s\n", number,
           failures == 0 ? "PASSED" : "FAILED");
}

} // namespace

int main() {
    printk("VoIP integration Phase 4 TCP registration validation\n");
    for (unsigned lifecycle = 1; lifecycle <= 3; ++lifecycle) Lifecycle(lifecycle);
    if (failures == 0)
        printk("VOIP INTEGRATION PHASE 4 RESULT: PASSED (3 registration lifecycles)\n");
    else
        printk("VOIP INTEGRATION PHASE 4 RESULT: FAILED (%d checks)\n", failures);
    return failures == 0 ? 0 : 1;
}
