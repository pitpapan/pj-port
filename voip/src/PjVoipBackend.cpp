#include <voip/PjVoipBackend.hpp>

#if defined(CONFIG_VOIP_PJ_HEADLESS_MEDIA)
#include "PjHeadlessMedia.hpp"
#endif

#include <pjlib-util.h>
#include <pjlib.h>
#include <pjmedia/endpoint.h>
#include <pjsip.h>
#include <pjsip-ua/sip_regc.h>
#include <pjsip-ua/sip_100rel.h>
#include <pjsip-ua/sip_inv.h>
#include <pjsip-ua/sip_timer.h>
#include <pjsip/sip_transport_tcp.h>
#include <pjmedia/sdp.h>
#include <pjmedia/sdp_neg.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include <cerrno>
#include <new>

namespace voip {

namespace {

constexpr std::size_t command_capacity = 8;

enum class CommandType : std::uint8_t {
    probe,
    configure_account,
    register_account,
    unregister_account,
    start_call,
    accept_call,
    reject_call,
    end_call,
    inject_registration,
    stop,
};

struct Completion {
    k_sem done;
    Error result;
};

struct Command {
    CommandType type;
    std::uint32_t value;
    std::uint16_t sip_status;
    RegistrationState registration_state;
    Error error;
    Completion *completion;
    char uri[max_uri_length + 1];
};

Error TranslateStatus(pj_status_t status) noexcept {
    if (status == PJ_SUCCESS) return Error::ok;
    if (status == PJ_ENOMEM) return Error::internal_failure;
    if (status == PJ_EINVAL) return Error::invalid_argument;
    if (status == PJ_ETOOMANY) return Error::queue_full;
    return Error::internal_failure;
}

} // namespace

class PjVoipBackend::Impl {
public:
    Impl() noexcept
        : observer(nullptr), pj_initialized(false), util_initialized(false),
          pool_initialized(false), sip_endpoint(nullptr), media_endpoint(nullptr),
          tcp_factory(nullptr), registration(nullptr), thread_pool(nullptr),
          event_thread(nullptr),
          queue{}, queue_buffer{}, accepting(0), stop(0), started(0),
          event_error(0), processing_paused(0), processed(0), last_value(0),
          tcp_port(0), account_configured(false), account_uri{}, registrar_uri{},
          username{}, password{}, expires(0),
          registration_state(RegistrationState::disabled), retry_timer{},
          retry_attempts(0), retry_scheduled(false), invite_initialized(false),
          call_module{}, active_invite(nullptr), call_state(CallState::idle),
          negotiated_codec(Codec::pcmu), remote_uri{}, call_transport(nullptr),
          negotiated_rtp_port(0), media_negotiated(false), media_started(false),
          previous_transport_callback(nullptr),
          transport_callback_installed(false)
#if defined(CONFIG_VOIP_PJ_HEADLESS_MEDIA)
          , headless_media(nullptr)
#endif
          {
        k_msgq_init(&queue, reinterpret_cast<char *>(queue_buffer),
                    sizeof(Command), command_capacity);
#if defined(CONFIG_VOIP_PJ_CALL_CONTROL)
        pj_bzero(&call_module, sizeof(call_module));
        call_module.name = pj_str(const_cast<char *>("voip-call-owner"));
        call_module.id = -1;
        call_module.priority = PJSIP_MOD_PRIORITY_APPLICATION;
        call_module.on_rx_request = &IncomingRequest;
#endif
    }

    Observer *observer;
    bool pj_initialized;
    bool util_initialized;
    bool pool_initialized;
    pj_caching_pool caching_pool;
    pjsip_endpoint *sip_endpoint;
    bool transaction_layer_initialized{};
    pjmedia_endpt *media_endpoint;
    pjsip_tpfactory *tcp_factory;
    pjsip_regc *registration;
    pj_pool_t *thread_pool;
    pj_thread_t *event_thread;
    k_msgq queue;
    alignas(Command) unsigned char queue_buffer[command_capacity * sizeof(Command)];
    atomic_t accepting;
    atomic_t stop;
    atomic_t started;
    atomic_t event_error;
    atomic_t processing_paused;
    atomic_t processed;
    atomic_t last_value;
    std::uint16_t tcp_port;
    bool account_configured;
    char account_uri[max_uri_length + 1];
    char registrar_uri[max_uri_length + 1];
    char username[max_username_length + 1];
    char password[max_password_length + 1];
    std::uint32_t expires;
    RegistrationState registration_state;
    pj_timer_entry retry_timer;
    unsigned retry_attempts;
    bool retry_scheduled;
    bool invite_initialized;
    pjsip_module call_module;
    pjsip_inv_session *active_invite;
    CallState call_state;
    Codec negotiated_codec;
    char remote_uri[max_uri_length + 1];
    pjsip_transport *call_transport;
    unsigned negotiated_rtp_port;
    char negotiated_rtp_address[64]{};
    bool media_negotiated;
    bool media_started;
    pjsip_tp_state_callback previous_transport_callback;
    bool transport_callback_installed;
#if defined(CONFIG_VOIP_PJ_HEADLESS_MEDIA)
    PjHeadlessMedia *headless_media;
#endif
    static Impl *active_instance;

    static pj_status_t ParseCallSdp(pj_pool_t *pool, unsigned rtp_port,
                                    pjmedia_sdp_session **session) noexcept {
        char text[320];
        const int length = pj_ansi_snprintf(text, sizeof(text),
            "v=0\r\no=voip 1 1 IN IP4 127.0.0.1\r\ns=voip-call\r\n"
            "c=IN IP4 127.0.0.1\r\nt=0 0\r\n"
            "m=audio %u RTP/AVP 0 8\r\na=sendrecv\r\n"
            "a=rtpmap:0 PCMU/8000\r\na=rtpmap:8 PCMA/8000\r\n", rtp_port);
        if (length <= 0 || static_cast<unsigned>(length) >= sizeof(text))
            return PJ_ETOOBIG;
        char *copy = static_cast<char *>(pj_pool_alloc(pool, length + 1));
        if (copy == nullptr) return PJ_ENOMEM;
        pj_memcpy(copy, text, length + 1);
        return pjmedia_sdp_parse(pool, copy, length, session);
    }

    Error PrepareCallMedia() noexcept {
#if defined(CONFIG_VOIP_PJ_HEADLESS_MEDIA)
        if (headless_media == nullptr) return Error::not_initialized;
        const Error result = headless_media->Prepare();
        if (result == Error::ok) {
            media_negotiated = false;
            media_started = false;
            negotiated_rtp_port = 0;
            negotiated_rtp_address[0] = '\0';
        }
        return result;
#else
        return Error::ok;
#endif
    }

    unsigned LocalCallRtpPort() const noexcept {
#if defined(CONFIG_VOIP_PJ_HEADLESS_MEDIA)
        return headless_media == nullptr ? 0U : headless_media->LocalRtpPort();
#else
        return 4000U;
#endif
    }

    void NotifyMedia(MediaDirection direction, Error error = Error::ok) noexcept {
        if (observer == nullptr) return;
        CallInfo info{};
        info.state = call_state;
        info.codec = negotiated_codec;
        info.direction = direction;
        pj_ansi_strncpy(info.remote_uri, remote_uri, max_uri_length);
        Status status{};
        status.error = error;
        observer->OnMediaState(info, status);
    }

    Error StartNegotiatedMedia() noexcept {
#if defined(CONFIG_VOIP_PJ_HEADLESS_MEDIA)
        if (media_started || !media_negotiated || call_state != CallState::established)
            return Error::invalid_state;
        const Error result = headless_media->StartPrepared(negotiated_codec,
                                                            negotiated_rtp_address,
                                                            negotiated_rtp_port);
        if (result == Error::ok) {
            media_started = true;
            NotifyMedia(MediaDirection::send_receive);
        } else {
            NotifyMedia(MediaDirection::inactive, result);
        }
        return result;
#else
        return Error::ok;
#endif
    }

    void StopCallMedia() noexcept {
#if defined(CONFIG_VOIP_PJ_HEADLESS_MEDIA)
        if (headless_media != nullptr &&
            (media_started || headless_media->LocalRtpPort() != 0)) {
            (void)headless_media->StopCall();
            if (media_started) NotifyMedia(MediaDirection::inactive);
        }
        media_started = false;
        media_negotiated = false;
        negotiated_rtp_port = 0;
        negotiated_rtp_address[0] = '\0';
#endif
    }

    void NotifyCall(CallState state, Error error = Error::ok,
                    std::uint16_t sip_status = 0) noexcept {
        call_state = state;
        Observer *current = observer;
        if (current == nullptr) return;
        CallInfo info{};
        info.state = state;
        info.codec = negotiated_codec;
        info.direction = state == CallState::established
                           ? MediaDirection::send_receive
                           : MediaDirection::inactive;
        pj_ansi_strncpy(info.remote_uri, remote_uri, max_uri_length);
        Status status{};
        status.error = error;
        status.sip_status = sip_status;
        current->OnCallState(info, status);
    }

    static void InviteStateChanged(pjsip_inv_session *invite,
                                   pjsip_event *event) noexcept {
        Impl *self = active_instance;
        if (self == nullptr || invite != self->active_invite) return;
        std::uint16_t code = 0;
        if (event != nullptr && event->type == PJSIP_EVENT_TSX_STATE &&
            event->body.tsx_state.tsx != nullptr) {
            code = static_cast<std::uint16_t>(event->body.tsx_state.tsx->status_code);
            if (event->body.tsx_state.tsx->transport != nullptr)
                self->call_transport = event->body.tsx_state.tsx->transport;
        }
        switch (invite->state) {
        case PJSIP_INV_STATE_CALLING: self->NotifyCall(CallState::outgoing); break;
        case PJSIP_INV_STATE_INCOMING: self->NotifyCall(CallState::incoming); break;
        case PJSIP_INV_STATE_EARLY: self->NotifyCall(CallState::early, Error::ok, code); break;
        case PJSIP_INV_STATE_CONFIRMED:
            self->NotifyCall(CallState::established, Error::ok, code);
            if (self->media_negotiated && !self->media_started)
                (void)self->StartNegotiatedMedia();
            break;
        case PJSIP_INV_STATE_DISCONNECTED:
            self->StopCallMedia();
            self->NotifyCall(CallState::disconnected,
                             code >= 300 ? Error::transport_failure : Error::ok,
                             code);
            self->active_invite = nullptr;
            self->call_transport = nullptr;
            break;
        default: break;
        }
    }

    static void TransportState(pjsip_transport *transport,
                               pjsip_transport_state state,
                               const pjsip_transport_state_info *info) noexcept {
        Impl *self = active_instance;
        pjsip_tp_state_callback previous = self == nullptr
            ? nullptr : self->previous_transport_callback;
        if (self != nullptr && state == PJSIP_TP_STATE_DISCONNECTED &&
            self->active_invite != nullptr && transport == self->call_transport) {
            self->StopCallMedia();
            self->NotifyCall(CallState::failed, Error::transport_failure,
                             info == nullptr ? 0 :
                             static_cast<std::uint16_t>(info->status));
            (void)pjsip_inv_terminate(self->active_invite,
                                      PJSIP_SC_SERVICE_UNAVAILABLE, PJ_TRUE);
        }
        if (previous != nullptr && previous != &TransportState)
            previous(transport, state, info);
    }

    static void InviteMediaUpdate(pjsip_inv_session *invite,
                                  pj_status_t status) noexcept {
        Impl *self = active_instance;
        if (self == nullptr || invite != self->active_invite) return;
        if (status != PJ_SUCCESS) {
            self->StopCallMedia();
            self->NotifyCall(CallState::failed, Error::negotiation_failure);
            return;
        }
        const pjmedia_sdp_session *local = nullptr;
        const pjmedia_sdp_session *remote = nullptr;
        if (invite->neg != nullptr &&
            pjmedia_sdp_neg_get_active_local(invite->neg, &local) == PJ_SUCCESS &&
            local != nullptr && local->media_count != 0 &&
            local->media[0]->desc.fmt_count != 0 &&
            pj_strcmp2(&local->media[0]->desc.fmt[0], "8") == 0)
            self->negotiated_codec = Codec::pcma;
        const pjmedia_sdp_conn *connection = nullptr;
        if (invite->neg == nullptr ||
            pjmedia_sdp_neg_get_active_remote(invite->neg, &remote) != PJ_SUCCESS ||
            remote == nullptr || remote->media_count == 0 ||
            remote->media[0]->desc.port == 0) {
            self->StopCallMedia();
            self->NotifyCall(CallState::failed, Error::negotiation_failure);
            return;
        }
        connection = remote->media[0]->conn != nullptr
            ? remote->media[0]->conn : remote->conn;
        if (connection == nullptr || connection->addr.slen <= 0 ||
            static_cast<std::size_t>(connection->addr.slen) >=
                sizeof(self->negotiated_rtp_address)) {
            self->StopCallMedia();
            self->NotifyCall(CallState::failed, Error::negotiation_failure);
            return;
        }
        pj_ansi_snprintf(self->negotiated_rtp_address,
                         sizeof(self->negotiated_rtp_address), "%.*s",
                         static_cast<int>(connection->addr.slen),
                         connection->addr.ptr);
        self->negotiated_rtp_port = remote->media[0]->desc.port;
        self->media_negotiated = true;
        if (self->call_state == CallState::established && !self->media_started)
            (void)self->StartNegotiatedMedia();
    }

    static void InviteRxOffer(pjsip_inv_session *invite,
                              const pjmedia_sdp_session *) noexcept {
        pjmedia_sdp_session *answer = nullptr;
        if (ParseCallSdp(invite->pool_prov,
                         active_instance == nullptr ? 4000U :
                         active_instance->LocalCallRtpPort(), &answer) == PJ_SUCCESS)
            (void)pjsip_inv_set_sdp_answer(invite, answer);
    }

    static pj_bool_t IncomingRequest(pjsip_rx_data *data) noexcept {
        Impl *self = active_instance;
        if (self == nullptr || self->active_invite != nullptr ||
            data->msg_info.msg->line.req.method.id != PJSIP_INVITE_METHOD ||
            data->msg_info.to == nullptr || data->msg_info.to->tag.slen != 0)
            return PJ_FALSE;
        auto *uri = static_cast<pjsip_sip_uri *>(
            pjsip_uri_get_uri(data->msg_info.to->uri));
        if (uri == nullptr || pj_strcmp2(&uri->user, self->username) != 0)
            return PJ_FALSE;
        pjsip_dialog *dialog = nullptr;
        pjsip_inv_session *invite = nullptr;
        pjmedia_sdp_session *answer = nullptr;
        char contact_text[max_username_length + 64];
        pj_ansi_snprintf(contact_text, sizeof(contact_text),
                         "<sip:%s@127.0.0.1:%u;transport=tcp>", self->username,
                         self->tcp_port);
        pj_str_t contact = pj_str(contact_text);
        pj_status_t status = pjsip_dlg_create_uas_and_inc_lock(
            pjsip_ua_instance(), data, &contact, &dialog);
        if (status == PJ_SUCCESS) {
            const Error media = self->PrepareCallMedia();
            if (media != Error::ok) status = PJ_EUNKNOWN;
        }
        if (status == PJ_SUCCESS)
            status = ParseCallSdp(dialog->pool, self->LocalCallRtpPort(), &answer);
        if (status == PJ_SUCCESS)
            status = pjsip_inv_create_uas(dialog, data, answer, 0, &invite);
        if (dialog != nullptr) pjsip_dlg_dec_lock(dialog);
        if (status != PJ_SUCCESS) {
            self->StopCallMedia();
            return PJ_FALSE;
        }
        self->active_invite = invite;
        self->negotiated_codec = Codec::pcmu;
        pj_ansi_strncpy(self->remote_uri, "<sip:peer@127.0.0.1>",
                        max_uri_length);
        pjsip_tx_data *response = nullptr;
        status = pjsip_inv_initial_answer(invite, data, 100, nullptr, nullptr,
                                          &response);
        if (status == PJ_SUCCESS) status = pjsip_inv_send_msg(invite, response);
        self->NotifyCall(CallState::incoming);
        CallInfo info{};
        info.state = CallState::incoming;
        info.codec = Codec::pcmu;
        info.direction = MediaDirection::inactive;
        pj_ansi_strncpy(info.remote_uri, self->remote_uri, max_uri_length);
        if (self->observer != nullptr) self->observer->OnIncomingCall(info);
        return PJ_TRUE;
    }

    Error StartCallOnEventThread(const char *uri) noexcept {
        if (registration_state != RegistrationState::registered)
            return Error::invalid_state;
        if (active_invite != nullptr) return Error::busy;
        const Error media = PrepareCallMedia();
        if (media != Error::ok) return media;
        negotiated_codec = Codec::pcmu;
        pjsip_dialog *dialog = nullptr;
        pjmedia_sdp_session *offer = nullptr;
        pjsip_tx_data *request = nullptr;
        char contact_text[max_username_length + 64];
        pj_ansi_snprintf(contact_text, sizeof(contact_text),
                         "<sip:%s@127.0.0.1:%u;transport=tcp>", username,
                         tcp_port);
        pj_str_t local = pj_str(account_uri);
        pj_str_t contact = pj_str(contact_text);
        pj_str_t remote = pj_str(const_cast<char *>(uri));
        pj_status_t status = pjsip_dlg_create_uac(pjsip_ua_instance(), &local,
                                                   &contact, &remote, &remote,
                                                   &dialog);
        if (status != PJ_SUCCESS) return TranslateStatus(status);
        pjsip_dlg_inc_lock(dialog);
        status = ParseCallSdp(dialog->pool, LocalCallRtpPort(), &offer);
        if (status == PJ_SUCCESS)
            status = pjsip_inv_create_uac(dialog, offer, 0, &active_invite);
        pjsip_dlg_dec_lock(dialog);
        if (status == PJ_SUCCESS) status = pjsip_inv_invite(active_invite, &request);
        if (status == PJ_SUCCESS) status = pjsip_inv_send_msg(active_invite, request);
        if (status != PJ_SUCCESS) {
            StopCallMedia();
            active_invite = nullptr;
            return TranslateStatus(status);
        }
        pj_ansi_strncpy(remote_uri, uri, max_uri_length);
        NotifyCall(CallState::outgoing);
        return Error::ok;
    }

    Error AnswerCallOnEventThread(int code) noexcept {
        if (active_invite == nullptr || active_invite->role != PJSIP_ROLE_UAS)
            return Error::invalid_state;
        pjsip_tx_data *response = nullptr;
        pj_status_t status = pjsip_inv_answer(active_invite, code, nullptr,
                                               nullptr, &response);
        if (status == PJ_SUCCESS) status = pjsip_inv_send_msg(active_invite, response);
        return TranslateStatus(status);
    }

    Error EndCallOnEventThread() noexcept {
        if (active_invite == nullptr) return Error::invalid_state;
        StopCallMedia();
        pjsip_tx_data *request = nullptr;
        pj_status_t status = pjsip_inv_end_session(active_invite,
            PJSIP_SC_REQUEST_TERMINATED, nullptr, &request);
        if (status == PJ_SUCCESS && request != nullptr)
            status = pjsip_inv_send_msg(active_invite, request);
        if (status == PJ_SUCCESS) NotifyCall(CallState::disconnecting);
        return TranslateStatus(status);
    }

    static void Complete(Command &command, Error result) noexcept {
        if (command.completion != nullptr) {
            command.completion->result = result;
            k_sem_give(&command.completion->done);
        }
    }

    void DeliverRegistration(RegistrationState state, Error error,
                             std::uint16_t sip_status) noexcept {
        registration_state = state;
        Observer *current = observer;
        if (current == nullptr) return;
        Status status{};
        status.error = error;
        status.sip_status = sip_status;
        current->OnRegistrationState(state, status);
    }

    static void RegistrationCallback(pjsip_regc_cbparam *param) noexcept {
#if !defined(CONFIG_VOIP_PJ_REGISTRATION_NETWORK)
        (void)param;
#else
        auto *self = static_cast<Impl *>(param->token);
        if (self == nullptr) return;
        if (param->status != PJ_SUCCESS) {
            if (param->status == PJSIP_EFAILEDCREDENTIAL) {
                self->DeliverRegistration(RegistrationState::failed,
                                          Error::authentication_failure,
                                          static_cast<std::uint16_t>(param->code));
            } else {
                self->DeliverRegistration(RegistrationState::connection_lost,
                                          Error::transport_failure,
                                          static_cast<std::uint16_t>(param->code));
                self->ScheduleRegistrationRetry();
            }
        } else if (param->code >= 200 && param->code < 300) {
            self->retry_attempts = 0;
            self->DeliverRegistration(param->is_unreg
                                          ? RegistrationState::disabled
                                          : RegistrationState::registered,
                                      Error::ok,
                                      static_cast<std::uint16_t>(param->code));
        } else if (param->code >= 300) {
            if (param->code == 401 || param->code == 403) {
                self->DeliverRegistration(RegistrationState::failed,
                                          Error::authentication_failure,
                                          static_cast<std::uint16_t>(param->code));
            } else {
                self->DeliverRegistration(RegistrationState::connection_lost,
                                          Error::transport_failure,
                                          static_cast<std::uint16_t>(param->code));
                self->ScheduleRegistrationRetry();
            }
        }
#endif
    }

    static void RetryTimer(pj_timer_heap_t *, pj_timer_entry *entry) noexcept {
        auto *self = static_cast<Impl *>(entry->user_data);
        if (self == nullptr) return;
        self->retry_scheduled = false;
        if (atomic_get(&self->accepting) != 0)
            (void)self->SendRegistration(false);
    }

    void ScheduleRegistrationRetry() noexcept {
        constexpr unsigned maximum_retries = 3;
        if (retry_scheduled || retry_attempts >= maximum_retries ||
            atomic_get(&accepting) == 0) return;
        const unsigned delay_ms = 250U << retry_attempts++;
        pj_time_val delay{static_cast<long>(delay_ms / 1000U),
                          static_cast<long>(delay_ms % 1000U)};
        pj_timer_entry_init(&retry_timer, 1, this, &RetryTimer);
        if (pjsip_endpt_schedule_timer(sip_endpoint, &retry_timer, &delay) ==
            PJ_SUCCESS) retry_scheduled = true;
    }

    Error SendRegistration(bool unregister) noexcept {
#if defined(CONFIG_VOIP_PJ_REGISTRATION_NETWORK)
        if (registration == nullptr || !account_configured)
            return Error::invalid_state;
        pjsip_tx_data *data = nullptr;
        pj_status_t status = unregister
            ? pjsip_regc_unregister(registration, &data)
            : pjsip_regc_register(registration, PJ_TRUE, &data);
        if (status == PJ_SUCCESS) status = pjsip_regc_send(registration, data);
        if (status != PJ_SUCCESS) return TranslateStatus(status);
        DeliverRegistration(unregister ? RegistrationState::unregistering
                                       : RegistrationState::registering,
                            Error::ok, 0);
        return Error::ok;
#else
        (void)unregister;
        return Error::invalid_state;
#endif
    }

    Error ConfigureAccountOnEventThread() noexcept {
#if defined(CONFIG_PJSIP_REGC)
        pjsip_cred_info credential;
        if (retry_scheduled) {
            pjsip_endpt_cancel_timer(sip_endpoint, &retry_timer);
            retry_scheduled = false;
        }
        retry_attempts = 0;
        if (registration != nullptr) {
            const pj_status_t destroy_status =
                pjsip_regc_destroy2(registration, PJ_TRUE);
            registration = nullptr;
            if (destroy_status != PJ_SUCCESS) return TranslateStatus(destroy_status);
        }

        pj_status_t status = pjsip_regc_create(
            sip_endpoint, this, &RegistrationCallback, &registration);
        if (status != PJ_SUCCESS) return TranslateStatus(status);

        pj_str_t registrar = pj_str(registrar_uri);
        pj_str_t identity = pj_str(account_uri);
        pj_str_t contact = pj_str(account_uri);
        status = pjsip_regc_init(registration, &registrar, &identity, &identity,
                                 1, &contact, expires);
        if (status != PJ_SUCCESS) goto failure;

        pj_bzero(&credential, sizeof(credential));
        credential.realm = pj_str(const_cast<char *>("*"));
        credential.scheme = pj_str(const_cast<char *>("digest"));
        credential.username = pj_str(username);
        credential.data_type = PJSIP_CRED_DATA_PLAIN_PASSWD;
        credential.data = pj_str(password);
        status = pjsip_regc_set_credentials(registration, 1, &credential);
        if (status != PJ_SUCCESS) goto failure;
#if defined(CONFIG_VOIP_PJ_REGISTRATION_NETWORK)
        status = pjsip_regc_set_delay_before_refresh(registration, 1);
        if (status != PJ_SUCCESS) goto failure;
#endif

        account_configured = true;
        registration_state = RegistrationState::disabled;
        return Error::ok;

failure:
        (void)pjsip_regc_destroy2(registration, PJ_TRUE);
        registration = nullptr;
        return TranslateStatus(status);
#else
        return Error::invalid_state;
#endif
    }

    void NotifyRegistration(const Command &command) noexcept {
        registration_state = command.registration_state;
        Observer *current = observer;
        if (current == nullptr) return;
        Status status{};
        status.error = command.error;
        status.sip_status = command.sip_status;
        current->OnRegistrationState(registration_state, status);
    }

    static int EventThread(void *argument) {
        auto *self = static_cast<Impl *>(argument);
        atomic_set(&self->started, 1);
        while (atomic_get(&self->stop) == 0) {
            if (atomic_get(&self->processing_paused) != 0) {
                pj_thread_sleep(1);
                continue;
            }
            Command command{};
            while (k_msgq_get(&self->queue, &command, K_NO_WAIT) == 0) {
                if (command.type == CommandType::stop) {
                    atomic_set(&self->stop, 1);
                    break;
                }
                if (command.type == CommandType::probe) {
                    atomic_set(&self->last_value,
                               static_cast<atomic_val_t>(command.value));
                    atomic_inc(&self->processed);
                    Complete(command, Error::ok);
                } else if (command.type == CommandType::configure_account) {
                    Complete(command, self->ConfigureAccountOnEventThread());
                } else if (command.type == CommandType::register_account) {
                    Complete(command, self->SendRegistration(false));
                } else if (command.type == CommandType::unregister_account) {
                    Complete(command, self->SendRegistration(true));
#if defined(CONFIG_VOIP_PJ_CALL_CONTROL)
                } else if (command.type == CommandType::start_call) {
                    Complete(command, self->StartCallOnEventThread(command.uri));
                } else if (command.type == CommandType::accept_call) {
                    Complete(command, self->AnswerCallOnEventThread(200));
                } else if (command.type == CommandType::reject_call) {
                    Complete(command, self->AnswerCallOnEventThread(
                        static_cast<int>(command.sip_status)));
                } else if (command.type == CommandType::end_call) {
                    Complete(command, self->EndCallOnEventThread());
#endif
                } else if (command.type == CommandType::inject_registration) {
                    self->NotifyRegistration(command);
                    Complete(command, Error::ok);
                }
            }
            if (atomic_get(&self->stop) != 0) break;
            pj_time_val timeout{0, 10};
            const pj_status_t status =
                pjsip_endpt_handle_events(self->sip_endpoint, &timeout);
            if (status != PJ_SUCCESS &&
                status != PJ_STATUS_FROM_OS(EBADF)) {
                atomic_set(&self->event_error, status);
                atomic_set(&self->stop, 1);
            }
        }
        return 0;
    }

    void Cleanup() noexcept {
        atomic_set(&accepting, 0);

#if defined(CONFIG_VOIP_PJ_HEADLESS_MEDIA)
        if (headless_media != nullptr) {
            headless_media->Destroy();
            delete headless_media;
            headless_media = nullptr;
        }
#endif

#if defined(CONFIG_VOIP_PJ_CALL_CONTROL)
        if (active_invite != nullptr) {
            pjsip_tx_data *request = nullptr;
            if (pjsip_inv_end_session(active_invite,
                    PJSIP_SC_REQUEST_TERMINATED, nullptr, &request) == PJ_SUCCESS &&
                request != nullptr)
                (void)pjsip_inv_send_msg(active_invite, request);
            active_invite = nullptr;
        }
        if (call_module.id >= 0 && sip_endpoint != nullptr)
            (void)pjsip_endpt_unregister_module(sip_endpoint, &call_module);
        if (sip_endpoint != nullptr && transport_callback_installed) {
            (void)pjsip_tpmgr_set_state_cb(pjsip_endpt_get_tpmgr(sip_endpoint),
                                            previous_transport_callback);
            transport_callback_installed = false;
        }
        active_instance = nullptr;
#endif

#if defined(CONFIG_PJSIP_REGC)
        if (retry_scheduled && sip_endpoint != nullptr) {
            pjsip_endpt_cancel_timer(sip_endpoint, &retry_timer);
            retry_scheduled = false;
        }
        if (registration != nullptr) {
            (void)pjsip_regc_destroy2(registration, PJ_TRUE);
            registration = nullptr;
        }
#endif

        if (tcp_factory != nullptr) {
            (void)tcp_factory->destroy(tcp_factory);
            tcp_factory = nullptr;
            if (event_thread != nullptr) pj_thread_sleep(50);
        }

        if (event_thread != nullptr) {
            Command command{CommandType::stop, 0, 0,
                            RegistrationState::disabled, Error::ok, nullptr};
            if (k_msgq_put(&queue, &command, K_NO_WAIT) != 0) {
                atomic_set(&stop, 1);
            }
            (void)pj_thread_join(event_thread);
            (void)pj_thread_destroy(event_thread);
            event_thread = nullptr;
        }

        if (thread_pool != nullptr) {
            pj_pool_release(thread_pool);
            thread_pool = nullptr;
        }
        if (media_endpoint != nullptr) {
            (void)pjmedia_endpt_destroy2(media_endpoint);
            media_endpoint = nullptr;
        }
        if (sip_endpoint != nullptr) {
#if defined(CONFIG_VOIP_PJ_REGISTRATION_NETWORK)
            if (transaction_layer_initialized &&
                pjsip_tsx_layer_get_tsx_count() == 0) {
                (void)pjsip_tsx_layer_destroy();
                transaction_layer_initialized = false;
            }
#endif
            pjsip_endpt_destroy(sip_endpoint);
            sip_endpoint = nullptr;
        }
        if (pool_initialized) {
            pj_caching_pool_destroy(&caching_pool);
            pool_initialized = false;
        }
        if (pj_initialized) {
            pj_shutdown();
            pj_initialized = false;
            util_initialized = false;
        }
        observer = nullptr;
        tcp_port = 0;
        account_configured = false;
        registration_state = RegistrationState::disabled;
        call_state = CallState::idle;
        pj_bzero(remote_uri, sizeof(remote_uri));
        pj_bzero(account_uri, sizeof(account_uri));
        pj_bzero(registrar_uri, sizeof(registrar_uri));
        pj_bzero(username, sizeof(username));
        pj_bzero(password, sizeof(password));
        expires = 0;
        k_msgq_purge(&queue);
    }

    Error RunSync(Command command) noexcept {
        Completion completion{};
        k_sem_init(&completion.done, 0, 1);
        completion.result = Error::internal_failure;
        command.completion = &completion;
        if (k_msgq_put(&queue, &command, K_NO_WAIT) != 0) return Error::queue_full;
        if (k_sem_take(&completion.done, K_MSEC(2000)) != 0) {
            return Error::internal_failure;
        }
        return completion.result;
    }
};

PjVoipBackend::Impl *PjVoipBackend::Impl::active_instance = nullptr;

PjVoipBackend::PjVoipBackend(RuntimeFailurePoint failure) noexcept
    : impl_(nullptr), failure_(failure) {}

PjVoipBackend::~PjVoipBackend() {
    (void)Shutdown();
    delete impl_;
}

Error PjVoipBackend::Initialize(Observer *observer) {
    pjsip_tcp_transport_cfg tcp_config;
    pj_str_t loopback = pj_str(const_cast<char *>("127.0.0.1"));

    if (impl_ != nullptr && impl_->pj_initialized) return Error::invalid_state;
    if (impl_ == nullptr) {
        impl_ = new (std::nothrow) Impl();
        if (impl_ == nullptr) return Error::internal_failure;
    }
    impl_->observer = observer;
    atomic_set(&impl_->stop, 0);
    atomic_set(&impl_->started, 0);
    atomic_set(&impl_->event_error, 0);
    atomic_set(&impl_->processing_paused, 0);
    atomic_set(&impl_->processed, 0);
    atomic_set(&impl_->last_value, 0);

    pj_status_t status = pj_init();
    if (status != PJ_SUCCESS) return TranslateStatus(status);
    impl_->pj_initialized = true;
    if (failure_ == RuntimeFailurePoint::after_pjlib) goto injected_failure;

    status = pjlib_util_init();
    if (status != PJ_SUCCESS) goto status_failure;
    impl_->util_initialized = true;
    if (failure_ == RuntimeFailurePoint::after_pjlib_util) goto injected_failure;

    // Bound released per-call transport/stream pools instead of retaining an
    // unbounded sequence across repeated calls.
    pj_caching_pool_init(&impl_->caching_pool, nullptr, 1);
    impl_->pool_initialized = true;
    if (failure_ == RuntimeFailurePoint::after_pool_factory) goto injected_failure;

    status = pjsip_endpt_create(&impl_->caching_pool.factory, "voip", &impl_->sip_endpoint);
    if (status != PJ_SUCCESS) goto status_failure;
#if defined(CONFIG_VOIP_PJ_REGISTRATION_NETWORK)
    status = pjsip_tsx_layer_init_module(impl_->sip_endpoint);
    if (status != PJ_SUCCESS) goto status_failure;
    impl_->transaction_layer_initialized = true;
#endif
#if defined(CONFIG_VOIP_PJ_CALL_CONTROL)
    {
        pjsip_ua_init_param ua{};
        status = pjsip_ua_init_module(impl_->sip_endpoint, &ua);
        if (status != PJ_SUCCESS) goto status_failure;
        status = pjsip_100rel_init_module(impl_->sip_endpoint);
        if (status != PJ_SUCCESS) goto status_failure;
        status = pjsip_timer_init_module(impl_->sip_endpoint);
        if (status != PJ_SUCCESS) goto status_failure;
        pjsip_inv_callback callbacks{};
        callbacks.on_state_changed = &Impl::InviteStateChanged;
        callbacks.on_media_update = &Impl::InviteMediaUpdate;
        callbacks.on_rx_offer = &Impl::InviteRxOffer;
        status = pjsip_inv_usage_init(impl_->sip_endpoint, &callbacks);
        if (status != PJ_SUCCESS) goto status_failure;
        Impl::active_instance = impl_;
        status = pjsip_endpt_register_module(impl_->sip_endpoint,
                                              &impl_->call_module);
        if (status != PJ_SUCCESS) goto status_failure;
        impl_->invite_initialized = true;
    }
#endif
    if (failure_ == RuntimeFailurePoint::after_sip_endpoint) goto injected_failure;

    status = pjmedia_endpt_create2(&impl_->caching_pool.factory,
                                   pjsip_endpt_get_ioqueue(impl_->sip_endpoint),
                                   0, &impl_->media_endpoint);
    if (status != PJ_SUCCESS) goto status_failure;
#if defined(CONFIG_VOIP_PJ_HEADLESS_MEDIA)
    impl_->headless_media = new (std::nothrow) PjHeadlessMedia(
        &impl_->caching_pool.factory, impl_->media_endpoint);
    if (impl_->headless_media == nullptr) {
        status = PJ_ENOMEM;
        goto status_failure;
    }
    if (impl_->headless_media->Initialize() != Error::ok) {
        status = PJ_EUNKNOWN;
        goto status_failure;
    }
#endif
    if (failure_ == RuntimeFailurePoint::after_media_endpoint) goto injected_failure;

    pjsip_tcp_transport_cfg_default(&tcp_config, pj_AF_INET());
    status = pj_sockaddr_in_init(&tcp_config.bind_addr.ipv4, &loopback, 0);
    if (status != PJ_SUCCESS) goto status_failure;
    tcp_config.async_cnt = 1;
    status = pjsip_tcp_transport_start3(impl_->sip_endpoint, &tcp_config,
                                        &impl_->tcp_factory);
    if (status != PJ_SUCCESS) goto status_failure;
    impl_->tcp_port = impl_->tcp_factory->addr_name.port;
#if defined(CONFIG_VOIP_PJ_CALL_CONTROL)
    impl_->previous_transport_callback = pjsip_tpmgr_get_state_cb(
        pjsip_endpt_get_tpmgr(impl_->sip_endpoint));
    status = pjsip_tpmgr_set_state_cb(pjsip_endpt_get_tpmgr(impl_->sip_endpoint),
                                      &Impl::TransportState);
    if (status != PJ_SUCCESS) goto status_failure;
    impl_->transport_callback_installed = true;
#endif
    if (failure_ == RuntimeFailurePoint::after_tcp_factory) goto injected_failure;

    impl_->thread_pool = pjsip_endpt_create_pool(impl_->sip_endpoint,
                                                 "voip-event", 4096, 4096);
    if (impl_->thread_pool == nullptr) {
        status = PJ_ENOMEM;
        goto status_failure;
    }
    status = pj_thread_create(impl_->thread_pool, "voip-event",
                              &Impl::EventThread, impl_,
                              PJ_THREAD_DEFAULT_STACK_SIZE, 0,
                              &impl_->event_thread);
    if (status != PJ_SUCCESS) goto status_failure;
    for (unsigned wait = 0; wait < 200 && atomic_get(&impl_->started) == 0; ++wait) {
        pj_thread_sleep(1);
    }
    if (atomic_get(&impl_->started) == 0) {
        status = PJ_ETIMEDOUT;
        goto status_failure;
    }
    if (failure_ == RuntimeFailurePoint::after_event_thread) goto injected_failure;

    atomic_set(&impl_->accepting, 1);
    return Error::ok;

injected_failure:
    impl_->Cleanup();
    return Error::internal_failure;
status_failure:
    {
        const Error error = TranslateStatus(status);
        impl_->Cleanup();
        return error;
    }
}

Error PjVoipBackend::BeginShutdown() noexcept {
    if (impl_ == nullptr || !impl_->pj_initialized) return Error::not_initialized;
    if (!atomic_cas(&impl_->accepting, 1, 0)) return Error::shutting_down;
    return Error::ok;
}

Error PjVoipBackend::Shutdown() {
    if (impl_ == nullptr || !impl_->pj_initialized) return Error::ok;
    atomic_set(&impl_->accepting, 0);
    impl_->Cleanup();
    return Error::ok;
}

Error PjVoipBackend::SubmitProbe(std::uint32_t value) noexcept {
    if (impl_ == nullptr || !impl_->pj_initialized) return Error::not_initialized;
    if (atomic_get(&impl_->accepting) == 0) return Error::shutting_down;
    const Command command{CommandType::probe, value, 0,
                          RegistrationState::disabled, Error::ok, nullptr};
    return k_msgq_put(&impl_->queue, &command, K_NO_WAIT) == 0
               ? Error::ok : Error::queue_full;
}

std::uint32_t PjVoipBackend::ProcessedProbeCount() const noexcept {
    return impl_ == nullptr ? 0U : static_cast<std::uint32_t>(atomic_get(&impl_->processed));
}
std::uint32_t PjVoipBackend::LastProbeValue() const noexcept {
    return impl_ == nullptr ? 0U : static_cast<std::uint32_t>(atomic_get(&impl_->last_value));
}
std::uint16_t PjVoipBackend::TcpPort() const noexcept {
    return impl_ == nullptr ? 0U : impl_->tcp_port;
}
bool PjVoipBackend::IsRunning() const noexcept {
    return impl_ != nullptr && impl_->pj_initialized &&
           atomic_get(&impl_->started) != 0 && atomic_get(&impl_->event_error) == 0;
}
bool PjVoipBackend::HasLiveResources() const noexcept {
    return impl_ != nullptr &&
           (impl_->pj_initialized || impl_->pool_initialized ||
            impl_->sip_endpoint != nullptr || impl_->media_endpoint != nullptr ||
            impl_->tcp_factory != nullptr || impl_->event_thread != nullptr ||
            impl_->thread_pool != nullptr);
}
void PjVoipBackend::SetProbeProcessingPaused(bool paused) noexcept {
    if (impl_ != nullptr) atomic_set(&impl_->processing_paused, paused ? 1 : 0);
}
bool PjVoipBackend::HasRegistrationClient() const noexcept {
#if defined(CONFIG_PJSIP_REGC)
    return impl_ != nullptr && impl_->registration != nullptr;
#else
    return false;
#endif
}
void PjVoipBackend::SetObserver(Observer *observer) noexcept {
    if (impl_ != nullptr) impl_->observer = observer;
}
Error PjVoipBackend::InjectRegistrationState(RegistrationState state,
                                             std::uint16_t sip_status,
                                             Error error) noexcept {
    if (impl_ == nullptr || !impl_->pj_initialized) return Error::not_initialized;
    if (atomic_get(&impl_->accepting) == 0) return Error::shutting_down;
    Command command{CommandType::inject_registration, 0, sip_status, state,
                    error, nullptr};
    return impl_->RunSync(command);
}

Error PjVoipBackend::ConfigureAccount(const AccountConfig &config) {
#if !defined(CONFIG_PJSIP_REGC)
    (void)config;
    return Error::invalid_state;
#else
    if (impl_ == nullptr || !impl_->pj_initialized) return Error::not_initialized;
    if (atomic_get(&impl_->accepting) == 0) return Error::shutting_down;
    if (config.account_uri == nullptr || config.registrar_uri == nullptr ||
        config.username == nullptr || config.password == nullptr ||
        config.registration_expires_seconds == 0) {
        return Error::invalid_argument;
    }
    const std::size_t account_length = pj_ansi_strlen(config.account_uri);
    const std::size_t registrar_length = pj_ansi_strlen(config.registrar_uri);
    const std::size_t username_length = pj_ansi_strlen(config.username);
    const std::size_t password_length = pj_ansi_strlen(config.password);
    if (account_length > max_uri_length || registrar_length > max_uri_length ||
        username_length > max_username_length ||
        password_length > max_password_length) {
        return Error::value_too_long;
    }
    pj_ansi_strcpy(impl_->account_uri, config.account_uri);
    pj_ansi_strcpy(impl_->registrar_uri, config.registrar_uri);
    pj_ansi_strcpy(impl_->username, config.username);
    pj_ansi_strcpy(impl_->password, config.password);
    impl_->expires = config.registration_expires_seconds;
    Command command{CommandType::configure_account, 0, 0,
                    RegistrationState::disabled, Error::ok, nullptr};
    return impl_->RunSync(command);
#endif
}
Error PjVoipBackend::RegisterAccount() {
#if !defined(CONFIG_VOIP_PJ_REGISTRATION_NETWORK)
    return Error::invalid_state;
#else
    if (impl_ == nullptr || !impl_->pj_initialized) return Error::not_initialized;
    if (atomic_get(&impl_->accepting) == 0) return Error::shutting_down;
    Command command{CommandType::register_account, 0, 0,
                    RegistrationState::disabled, Error::ok, nullptr};
    return impl_->RunSync(command);
#endif
}
Error PjVoipBackend::UnregisterAccount() {
#if !defined(CONFIG_VOIP_PJ_REGISTRATION_NETWORK)
    return Error::invalid_state;
#else
    if (impl_ == nullptr || !impl_->pj_initialized) return Error::not_initialized;
    if (atomic_get(&impl_->accepting) == 0) return Error::shutting_down;
    Command command{CommandType::unregister_account, 0, 0,
                    RegistrationState::disabled, Error::ok, nullptr};
    return impl_->RunSync(command);
#endif
}
Error PjVoipBackend::StartOutgoingCall(const char *uri) {
#if !defined(CONFIG_VOIP_PJ_CALL_CONTROL)
    (void)uri;
    return Error::invalid_state;
#else
    if (impl_ == nullptr || !impl_->pj_initialized) return Error::not_initialized;
    if (uri == nullptr) return Error::invalid_argument;
    const std::size_t length = pj_ansi_strlen(uri);
    if (length > max_uri_length) return Error::value_too_long;
    Command command{CommandType::start_call, 0, 0,
                    RegistrationState::disabled, Error::ok, nullptr, {}};
    pj_ansi_strcpy(command.uri, uri);
    return impl_->RunSync(command);
#endif
}
Error PjVoipBackend::AcceptCall() {
#if !defined(CONFIG_VOIP_PJ_CALL_CONTROL)
    return Error::invalid_state;
#else
    Command command{CommandType::accept_call, 0, 0,
                    RegistrationState::disabled, Error::ok, nullptr, {}};
    return impl_ == nullptr ? Error::not_initialized : impl_->RunSync(command);
#endif
}
Error PjVoipBackend::RejectCall(std::uint16_t code) {
#if !defined(CONFIG_VOIP_PJ_CALL_CONTROL)
    (void)code;
    return Error::invalid_state;
#else
    if (code < 300 || code > 699) return Error::invalid_argument;
    Command command{CommandType::reject_call, 0, code,
                    RegistrationState::disabled, Error::ok, nullptr, {}};
    return impl_ == nullptr ? Error::not_initialized : impl_->RunSync(command);
#endif
}
Error PjVoipBackend::EndCall() {
#if !defined(CONFIG_VOIP_PJ_CALL_CONTROL)
    return Error::invalid_state;
#else
    Command command{CommandType::end_call, 0, 0,
                    RegistrationState::disabled, Error::ok, nullptr, {}};
    return impl_ == nullptr ? Error::not_initialized : impl_->RunSync(command);
#endif
}
Error PjVoipBackend::SetHeld(bool) { return Error::invalid_state; }
Error PjVoipBackend::StartHeadlessMedia(Codec codec) {
#if !defined(CONFIG_VOIP_PJ_HEADLESS_MEDIA)
    (void)codec;
    return Error::invalid_state;
#else
    if (impl_ == nullptr || !impl_->pj_initialized ||
        impl_->headless_media == nullptr) return Error::not_initialized;
    const Error result = impl_->headless_media->Start(codec);
    if (result == Error::ok && impl_->observer != nullptr) {
        CallInfo info = GetCallInfo();
        info.codec = codec;
        info.direction = MediaDirection::send_receive;
        Status status{};
        impl_->observer->OnMediaState(info, status);
    }
    return result;
#endif
}

Error PjVoipBackend::SetMediaPaused(bool paused) {
#if !defined(CONFIG_VOIP_PJ_HEADLESS_MEDIA)
    (void)paused;
    return Error::invalid_state;
#else
    if (impl_ == nullptr || impl_->headless_media == nullptr)
        return Error::not_initialized;
    const Error result = impl_->headless_media->SetPaused(paused);
    if (result == Error::ok && impl_->observer != nullptr) {
        CallInfo info = GetCallInfo();
        info.direction = paused ? MediaDirection::inactive
                                : MediaDirection::send_receive;
        Status status{};
        impl_->observer->OnMediaState(info, status);
    }
    return result;
#endif
}

Error PjVoipBackend::StopMedia() {
#if !defined(CONFIG_VOIP_PJ_HEADLESS_MEDIA)
    return Error::invalid_state;
#else
    if (impl_ == nullptr || impl_->headless_media == nullptr)
        return Error::not_initialized;
    const Error result = impl_->headless_media->Stop();
    if (result == Error::ok && impl_->observer != nullptr) {
        CallInfo info = GetCallInfo();
        info.direction = MediaDirection::inactive;
        Status status{};
        impl_->observer->OnMediaState(info, status);
    }
    return result;
#endif
}

MediaStats PjVoipBackend::GetMediaStats() const {
#if defined(CONFIG_VOIP_PJ_HEADLESS_MEDIA)
    return impl_ != nullptr && impl_->headless_media != nullptr
        ? impl_->headless_media->Stats() : MediaStats{};
#else
    return MediaStats{};
#endif
}
RegistrationState PjVoipBackend::GetRegistrationState() const {
    return impl_ == nullptr ? RegistrationState::disabled
                            : impl_->registration_state;
}
CallInfo PjVoipBackend::GetCallInfo() const {
    CallInfo info{};
    info.state = impl_ == nullptr ? CallState::idle : impl_->call_state;
    info.codec = impl_ == nullptr ? Codec::pcmu : impl_->negotiated_codec;
    info.direction = info.state == CallState::established
                       ? MediaDirection::send_receive
                       : MediaDirection::inactive;
    if (impl_ != nullptr)
        pj_ansi_strncpy(info.remote_uri, impl_->remote_uri, max_uri_length);
    return info;
}

void *PjVoipBackend::NativeSipEndpointForValidation() const noexcept {
    return impl_ == nullptr ? nullptr : impl_->sip_endpoint;
}

Error PjVoipBackend::InjectMediaTransportFailureForValidation() noexcept {
#if defined(CONFIG_VOIP_PJ_HEADLESS_MEDIA)
    if (impl_ == nullptr || impl_->headless_media == nullptr)
        return Error::not_initialized;
    const Error result = impl_->headless_media->InjectTransportFailure();
    if (result == Error::ok && impl_->media_started) {
        impl_->media_started = false;
        impl_->NotifyMedia(MediaDirection::inactive, Error::media_failure);
    }
    return result;
#else
    return Error::invalid_state;
#endif
}

} // namespace voip
