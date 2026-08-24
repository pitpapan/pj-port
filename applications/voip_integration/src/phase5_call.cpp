#include <voip/PjVoipBackend.hpp>

#include <pjsip.h>
#include <pjsip-ua/sip_inv.h>
#include <pjmedia/sdp.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>

namespace {

constexpr unsigned wait_ms = 7000;
int failures;
atomic_t callback_failures;

#define CHECK(condition) do {                                                   \
    if (!(condition)) {                                                         \
        printk("[Phase 5] FAIL line %d: %s\n", __LINE__, #condition);          \
        ++failures;                                                             \
    }                                                                           \
} while (false)

struct Peer {
    enum class Mode { normal, pcma, busy, unsupported, cancel, remote_bye,
                      close_early, close_confirmed }
        mode{Mode::normal};
    pjsip_endpoint *endpoint{};
    pjsip_inv_session *invite{};
    pjsip_transport *transport{};
    pj_timer_entry timer{};
    int answer_stage{};
    atomic_t invites{};
    atomic_t confirmed{};
    atomic_t disconnected{};
    atomic_t tcp_requests{};
    atomic_t register_requests{};
    atomic_t registrations{};
    atomic_t unregistrations{};
    char contact[96]{};
};

Peer *active_peer;

pj_status_t ParseSdp(pj_pool_t *pool, pjmedia_sdp_session **session) {
    static const char text[] =
        "v=0\r\no=peer 1 1 IN IP4 127.0.0.1\r\ns=phase5-peer\r\n"
        "c=IN IP4 127.0.0.1\r\nt=0 0\r\n"
        "m=audio 4002 RTP/AVP 0 8\r\na=sendrecv\r\n"
        "a=rtpmap:0 PCMU/8000\r\na=rtpmap:8 PCMA/8000\r\n";
    char *copy = static_cast<char *>(pj_pool_alloc(pool, sizeof(text)));
    if (copy == nullptr) return PJ_ENOMEM;
    pj_memcpy(copy, text, sizeof(text));
    return pjmedia_sdp_parse(pool, copy, sizeof(text) - 1, session);
}

pj_status_t ParsePcmaSdp(pj_pool_t *pool, pjmedia_sdp_session **session) {
    static const char text[] =
        "v=0\r\no=peer 1 1 IN IP4 127.0.0.1\r\ns=phase5-peer\r\n"
        "c=IN IP4 127.0.0.1\r\nt=0 0\r\n"
        "m=audio 4002 RTP/AVP 8\r\na=sendrecv\r\n"
        "a=rtpmap:8 PCMA/8000\r\n";
    char *copy = static_cast<char *>(pj_pool_alloc(pool, sizeof(text)));
    if (copy == nullptr) return PJ_ENOMEM;
    pj_memcpy(copy, text, sizeof(text));
    return pjmedia_sdp_parse(pool, copy, sizeof(text) - 1, session);
}

pj_status_t SendAnswer(pjsip_inv_session *invite, pjsip_rx_data *request,
                       int code, bool initial) {
    pjsip_tx_data *response = nullptr;
    pj_status_t status = initial
        ? pjsip_inv_initial_answer(invite, request, code, nullptr, nullptr,
                                   &response)
        : pjsip_inv_answer(invite, code, nullptr, nullptr, &response);
    if (status == PJ_SUCCESS) status = pjsip_inv_send_msg(invite, response);
    return status;
}

void AnswerTimer(pj_timer_heap_t *, pj_timer_entry *entry) {
    auto *peer = static_cast<Peer *>(entry->user_data);
    if (peer->answer_stage == -1) {
        pjsip_tx_data *request = nullptr;
        pj_status_t status = pjsip_inv_end_session(peer->invite, PJSIP_SC_OK,
                                                    nullptr, &request);
        if (status == PJ_SUCCESS && request != nullptr)
            status = pjsip_inv_send_msg(peer->invite, request);
        if (status != PJ_SUCCESS) atomic_inc(&callback_failures);
        peer->answer_stage = 0;
        return;
    }
    if (peer->answer_stage == -2) {
        if (peer->transport == nullptr ||
            pjsip_transport_shutdown2(peer->transport, PJ_TRUE) != PJ_SUCCESS)
            atomic_inc(&callback_failures);
        peer->transport = nullptr;
        peer->answer_stage = 0;
        return;
    }
    pj_status_t status = SendAnswer(peer->invite, nullptr, peer->answer_stage,
                                    false);
    if (status != PJ_SUCCESS) atomic_inc(&callback_failures);
    if (peer->answer_stage == 180) {
        if (peer->mode == Peer::Mode::cancel) {
            peer->answer_stage = 0;
            return;
        }
        peer->answer_stage = peer->mode == Peer::Mode::busy ? 486
            : peer->mode == Peer::Mode::unsupported ? 488 : 200;
        pj_time_val delay{0, 30};
        if (pjsip_endpt_schedule_timer(peer->endpoint, &peer->timer, &delay) !=
            PJ_SUCCESS) atomic_inc(&callback_failures);
    } else {
        if (peer->answer_stage == 200 &&
            (peer->mode == Peer::Mode::remote_bye ||
             peer->mode == Peer::Mode::close_confirmed)) {
            peer->answer_stage = peer->mode == Peer::Mode::remote_bye ? -1 : -2;
            pj_time_val delay{0, 100};
            if (pjsip_endpt_schedule_timer(peer->endpoint, &peer->timer, &delay) !=
                PJ_SUCCESS) atomic_inc(&callback_failures);
        } else {
            peer->answer_stage = 0;
        }
    }
}

pj_bool_t OnPeerRequest(pjsip_rx_data *data);

pjsip_module peer_module = {
    .name = {const_cast<char *>("voip-phase5-peer"), 16},
    .id = -1,
    .priority = PJSIP_MOD_PRIORITY_APPLICATION + 1,
    .on_rx_request = OnPeerRequest,
};

pj_bool_t OnPeerRequest(pjsip_rx_data *data) {
    Peer *peer = active_peer;
    if (peer == nullptr) return PJ_FALSE;
    if (data->msg_info.msg->line.req.method.id == PJSIP_REGISTER_METHOD) {
        atomic_inc(&peer->register_requests);
        auto *expires = static_cast<pjsip_expires_hdr *>(pjsip_msg_find_hdr(
            data->msg_info.msg, PJSIP_H_EXPIRES, nullptr));
        const unsigned expiration = expires == nullptr ? 4U
            : static_cast<unsigned>(expires->ivalue);
        atomic_inc(expiration == 0 ? &peer->unregistrations
                                   : &peer->registrations);
        pjsip_transaction *transaction = nullptr;
        pjsip_tx_data *response = nullptr;
        pj_status_t status = pjsip_tsx_create_uas(&peer_module, data,
                                                   &transaction);
        if (status == PJ_SUCCESS) pjsip_tsx_recv_msg(transaction, data);
        if (status == PJ_SUCCESS)
            status = pjsip_endpt_create_response(peer->endpoint, data, 200,
                                                  nullptr, &response);
        if (status == PJ_SUCCESS) {
            auto *response_expires = pjsip_expires_hdr_create(response->pool,
                                                               expiration);
            pjsip_msg_add_hdr(response->msg,
                              reinterpret_cast<pjsip_hdr *>(response_expires));
            auto *request_contact = static_cast<pjsip_contact_hdr *>(
                pjsip_msg_find_hdr(data->msg_info.msg, PJSIP_H_CONTACT, nullptr));
            if (request_contact != nullptr) {
                auto *contact = reinterpret_cast<pjsip_contact_hdr *>(
                    pjsip_hdr_clone(response->pool,
                                    reinterpret_cast<pjsip_hdr *>(request_contact)));
                contact->expires = static_cast<int>(expiration);
                pjsip_msg_add_hdr(response->msg,
                                  reinterpret_cast<pjsip_hdr *>(contact));
            }
            status = pjsip_tsx_send_msg(transaction, response);
        }
        if (status != PJ_SUCCESS) atomic_inc(&callback_failures);
        return PJ_TRUE;
    }
    if (data->msg_info.msg->line.req.method.id != PJSIP_INVITE_METHOD ||
        data->msg_info.to == nullptr || data->msg_info.to->tag.slen != 0)
        return PJ_FALSE;
    auto *uri = static_cast<pjsip_sip_uri *>(
        pjsip_uri_get_uri(data->msg_info.to->uri));
    if (uri == nullptr || pj_strcmp2(&uri->user, "peer") != 0) return PJ_FALSE;
    atomic_inc(&peer->invites);
    peer->transport = data->tp_info.transport;
    if (data->tp_info.transport->key.type == PJSIP_TRANSPORT_TCP)
        atomic_inc(&peer->tcp_requests);
    pjsip_dialog *dialog = nullptr;
    pjmedia_sdp_session *answer = nullptr;
    pj_str_t contact = pj_str(peer->contact);
    pj_status_t status = pjsip_dlg_create_uas_and_inc_lock(
        pjsip_ua_instance(), data, &contact, &dialog);
    if (status == PJ_SUCCESS)
        status = peer->mode == Peer::Mode::pcma
            ? ParsePcmaSdp(dialog->pool, &answer)
            : ParseSdp(dialog->pool, &answer);
    if (status == PJ_SUCCESS)
        status = pjsip_inv_create_uas(dialog, data, answer, 0, &peer->invite);
    if (dialog != nullptr) pjsip_dlg_dec_lock(dialog);
    if (status == PJ_SUCCESS) status = SendAnswer(peer->invite, data, 100, true);
    if (status == PJ_SUCCESS && peer->mode == Peer::Mode::close_early) {
        status = pjsip_transport_shutdown2(peer->transport, PJ_TRUE);
        peer->transport = nullptr;
    } else if (status == PJ_SUCCESS && peer->mode != Peer::Mode::cancel) {
        peer->answer_stage = 180;
        pj_timer_entry_init(&peer->timer, 180, peer, &AnswerTimer);
        pj_time_val delay{0, 30};
        status = pjsip_endpt_schedule_timer(peer->endpoint, &peer->timer, &delay);
    }
    if (status != PJ_SUCCESS) atomic_inc(&callback_failures);
    return PJ_TRUE;
}

class RecordingObserver final : public voip::Observer {
public:
    atomic_t registered{};
    atomic_t registration_disabled{};
    atomic_t incoming{};
    atomic_t outgoing{};
    atomic_t early{};
    atomic_t established{};
    atomic_t disconnecting{};
    atomic_t disconnected{};
    atomic_t failed{};
    voip::CallInfo last{};

    void OnRegistrationState(voip::RegistrationState state,
                             const voip::Status &) override {
        if (state == voip::RegistrationState::registered)
            atomic_inc(&registered);
        else if (state == voip::RegistrationState::disabled)
            atomic_inc(&registration_disabled);
    }

    void OnIncomingCall(const voip::CallInfo &info) override {
        last = info;
        atomic_inc(&incoming);
    }

    void OnCallState(const voip::CallInfo &info,
                     const voip::Status &) override {
        last = info;
        switch (info.state) {
        case voip::CallState::outgoing: atomic_inc(&outgoing); break;
        case voip::CallState::early: atomic_inc(&early); break;
        case voip::CallState::established: atomic_inc(&established); break;
        case voip::CallState::disconnecting: atomic_inc(&disconnecting); break;
        case voip::CallState::disconnected: atomic_inc(&disconnected); break;
        case voip::CallState::failed: atomic_inc(&failed); break;
        default: break;
        }
    }
};

bool WaitFor(atomic_t &value, atomic_val_t expected = 1) {
    for (unsigned elapsed = 0; elapsed < wait_ms; elapsed += 10) {
        if (atomic_get(&value) >= expected) return true;
        k_msleep(10);
    }
    return false;
}

bool WaitQuiescent(pjsip_endpoint *endpoint) {
    (void)endpoint;
    for (unsigned elapsed = 0; elapsed < wait_ms; elapsed += 10) {
        if (pjsip_tsx_layer_get_tsx_count() == 0 &&
            pjsip_ua_get_dlg_set_count() == 0)
            return true;
        k_msleep(10);
    }
    return false;
}

pj_status_t StartIncomingPeerCall(Peer &peer, unsigned port,
                                  bool with_offer = true) {
    pjsip_dialog *dialog = nullptr;
    pjmedia_sdp_session *offer = nullptr;
    pjsip_tx_data *request = nullptr;
    char remote_text[96];
    pj_ansi_snprintf(remote_text, sizeof(remote_text),
                     "sip:alice@127.0.0.1:%u;transport=tcp", port);
    pj_str_t local = pj_str(const_cast<char *>("sip:peer@127.0.0.1"));
    pj_str_t contact = pj_str(peer.contact);
    pj_str_t remote = pj_str(remote_text);
    pj_status_t status = pjsip_dlg_create_uac(pjsip_ua_instance(), &local,
                                               &contact, &remote, &remote,
                                               &dialog);
    if (status != PJ_SUCCESS) return status;
    pjsip_dlg_inc_lock(dialog);
    if (with_offer) status = ParseSdp(dialog->pool, &offer);
    if (status == PJ_SUCCESS)
        status = pjsip_inv_create_uac(dialog, offer, 0, &peer.invite);
    pjsip_dlg_dec_lock(dialog);
    if (status == PJ_SUCCESS) status = pjsip_inv_invite(peer.invite, &request);
    if (status == PJ_SUCCESS) status = pjsip_inv_send_msg(peer.invite, request);
    return status;
}

pj_status_t EndPeerCall(Peer &peer) {
    pjsip_tx_data *request = nullptr;
    pj_status_t status = pjsip_inv_end_session(peer.invite, PJSIP_SC_OK,
                                                nullptr, &request);
    if (status == PJ_SUCCESS && request != nullptr)
        status = pjsip_inv_send_msg(peer.invite, request);
    return status;
}

void Lifecycle(unsigned number) {
    voip::PjVoipBackend backend;
    voip::VoipManager manager(backend);
    RecordingObserver observer;
    Peer peer;
    CHECK(manager.Initialize(&observer) == voip::Error::ok);
    peer.endpoint = static_cast<pjsip_endpoint *>(
        backend.NativeSipEndpointForValidation());
    active_peer = &peer;
    pj_ansi_snprintf(peer.contact, sizeof(peer.contact),
                     "<sip:peer@127.0.0.1:%u;transport=tcp>", backend.TcpPort());
    CHECK(pjsip_endpt_register_module(peer.endpoint, &peer_module) == PJ_SUCCESS);

    char registrar[96];
    char remote[96];
    pj_ansi_snprintf(registrar, sizeof(registrar),
                     "sip:127.0.0.1:%u;transport=tcp", backend.TcpPort());
    pj_ansi_snprintf(remote, sizeof(remote),
                     "sip:peer@127.0.0.1:%u;transport=tcp", backend.TcpPort());
    const voip::AccountConfig account{"<sip:alice@127.0.0.1>", registrar,
                                      "alice", "phase5-secret", 4};
    CHECK(manager.ConfigureAccount(account) == voip::Error::ok);
    CHECK(manager.RegisterAccount() == voip::Error::ok);
    CHECK(WaitFor(observer.registered));
    CHECK(manager.StartOutgoingCall(remote) == voip::Error::ok);
    CHECK(manager.StartOutgoingCall(remote) == voip::Error::busy);
    CHECK(WaitFor(observer.early));
    CHECK(WaitFor(observer.established));
    CHECK(observer.last.codec == voip::Codec::pcmu);
    CHECK(observer.last.direction == voip::MediaDirection::send_receive);
    CHECK(atomic_get(&peer.invites) == 1);
    CHECK(atomic_get(&peer.tcp_requests) == 1);
    CHECK(manager.EndCall() == voip::Error::ok);
    CHECK(WaitFor(observer.disconnected));
    CHECK(WaitQuiescent(peer.endpoint));

    peer.mode = Peer::Mode::busy;
    CHECK(manager.StartOutgoingCall(remote) == voip::Error::ok);
    CHECK(WaitFor(observer.disconnected, 2));
    CHECK(WaitQuiescent(peer.endpoint));

    peer.mode = Peer::Mode::unsupported;
    CHECK(manager.StartOutgoingCall(remote) == voip::Error::ok);
    CHECK(WaitFor(observer.disconnected, 3));
    CHECK(WaitQuiescent(peer.endpoint));

    peer.mode = Peer::Mode::pcma;
    CHECK(manager.StartOutgoingCall(remote) == voip::Error::ok);
    CHECK(WaitFor(observer.established, 2));
    CHECK(observer.last.codec == voip::Codec::pcma);
    CHECK(manager.EndCall() == voip::Error::ok);
    CHECK(WaitFor(observer.disconnected, 4));
    CHECK(WaitQuiescent(peer.endpoint));

    peer.mode = Peer::Mode::cancel;
    CHECK(manager.StartOutgoingCall(remote) == voip::Error::ok);
    k_msleep(50);
    CHECK(manager.EndCall() == voip::Error::ok);
    CHECK(WaitFor(observer.disconnected, 5));
    CHECK(WaitQuiescent(peer.endpoint));

    peer.mode = Peer::Mode::remote_bye;
    CHECK(manager.StartOutgoingCall(remote) == voip::Error::ok);
    CHECK(WaitFor(observer.established, 3));
    CHECK(WaitFor(observer.disconnected, 6));
    CHECK(WaitQuiescent(peer.endpoint));
    CHECK(atomic_get(&peer.invites) == 6);
    CHECK(atomic_get(&peer.tcp_requests) == 6);

    peer.mode = Peer::Mode::close_early;
    CHECK(manager.StartOutgoingCall(remote) == voip::Error::ok);
    CHECK(WaitFor(observer.failed));
    CHECK(WaitFor(observer.disconnected, 7));
    CHECK(WaitFor(observer.registered, 2));

    peer.mode = Peer::Mode::close_confirmed;
    CHECK(manager.StartOutgoingCall(remote) == voip::Error::ok);
    CHECK(WaitFor(observer.established, 4));
    CHECK(WaitFor(observer.failed, 2));
    CHECK(WaitFor(observer.disconnected, 8));
    CHECK(WaitFor(observer.registered, 3));

    CHECK(StartIncomingPeerCall(peer, backend.TcpPort()) == PJ_SUCCESS);
    CHECK(WaitFor(observer.incoming));
    CHECK(manager.StartOutgoingCall(remote) == voip::Error::busy);
    CHECK(manager.RejectCall(486) == voip::Error::ok);
    CHECK(WaitFor(observer.disconnected, 9));

    CHECK(StartIncomingPeerCall(peer, backend.TcpPort()) == PJ_SUCCESS);
    CHECK(WaitFor(observer.incoming, 2));
    k_msleep(50);
    CHECK(manager.AcceptCall() == voip::Error::ok);
    CHECK(WaitFor(observer.established, 5));
    CHECK(WaitFor(observer.registered, 4));
    CHECK(atomic_get(&peer.registrations) >= 4);
    CHECK(EndPeerCall(peer) == PJ_SUCCESS);
    CHECK(WaitFor(observer.disconnected, 10));

    CHECK(StartIncomingPeerCall(peer, backend.TcpPort(), false) == PJ_SUCCESS);
    CHECK(WaitFor(observer.incoming, 3));
    k_msleep(50);
    CHECK(manager.AcceptCall() == voip::Error::ok);
    CHECK(WaitFor(observer.established, 6));
    CHECK(EndPeerCall(peer) == PJ_SUCCESS);
    CHECK(WaitFor(observer.disconnected, 11));

    CHECK(StartIncomingPeerCall(peer, backend.TcpPort()) == PJ_SUCCESS);
    CHECK(WaitFor(observer.incoming, 4));
    CHECK(EndPeerCall(peer) == PJ_SUCCESS);
    CHECK(WaitFor(observer.disconnected, 12));
    CHECK(atomic_get(&observer.failed) == 2);
    CHECK(atomic_get(&callback_failures) == 0);
    CHECK(manager.UnregisterAccount() == voip::Error::ok);
    CHECK(WaitFor(observer.registration_disabled));
    CHECK(atomic_get(&peer.unregistrations) == 1);

    k_msleep(100);
    CHECK(pjsip_endpt_unregister_module(peer.endpoint, &peer_module) == PJ_SUCCESS);
    active_peer = nullptr;
    CHECK(manager.Shutdown() == voip::Error::ok);
    CHECK(!backend.HasLiveResources());
    printk("[Phase 5] lifecycle %u TCP call-control matrix: %s\n", number,
           failures == 0 ? "PASSED" : "FAILED");
}

} // namespace

int main() {
    printk("VoIP integration Phase 5 one-call TCP validation\n");
    for (unsigned lifecycle = 1; lifecycle <= 3; ++lifecycle) Lifecycle(lifecycle);
    if (failures == 0)
        printk("VOIP INTEGRATION PHASE 5 RESULT: PASSED (3 call lifecycles)\n");
    else
        printk("VOIP INTEGRATION PHASE 5 RESULT: FAILED (%d checks)\n", failures);
    return failures == 0 ? 0 : 1;
}
