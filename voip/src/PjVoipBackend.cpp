#include <voip/PjVoipBackend.hpp>

#include <pjlib-util.h>
#include <pjlib.h>
#include <pjmedia/endpoint.h>
#include <pjsip.h>
#include <pjsip-ua/sip_regc.h>
#include <pjsip/sip_transport_tcp.h>

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
          retry_attempts(0), retry_scheduled(false) {
        k_msgq_init(&queue, reinterpret_cast<char *>(queue_buffer),
                    sizeof(Command), command_capacity);
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

    pj_caching_pool_init(&impl_->caching_pool, nullptr, 0);
    impl_->pool_initialized = true;
    if (failure_ == RuntimeFailurePoint::after_pool_factory) goto injected_failure;

    status = pjsip_endpt_create(&impl_->caching_pool.factory, "voip", &impl_->sip_endpoint);
    if (status != PJ_SUCCESS) goto status_failure;
#if defined(CONFIG_VOIP_PJ_REGISTRATION_NETWORK)
    status = pjsip_tsx_layer_init_module(impl_->sip_endpoint);
    if (status != PJ_SUCCESS) goto status_failure;
    impl_->transaction_layer_initialized = true;
#endif
    if (failure_ == RuntimeFailurePoint::after_sip_endpoint) goto injected_failure;

    status = pjmedia_endpt_create2(&impl_->caching_pool.factory,
                                   pjsip_endpt_get_ioqueue(impl_->sip_endpoint),
                                   0, &impl_->media_endpoint);
    if (status != PJ_SUCCESS) goto status_failure;
    if (failure_ == RuntimeFailurePoint::after_media_endpoint) goto injected_failure;

    pjsip_tcp_transport_cfg_default(&tcp_config, pj_AF_INET());
    status = pj_sockaddr_in_init(&tcp_config.bind_addr.ipv4, &loopback, 0);
    if (status != PJ_SUCCESS) goto status_failure;
    tcp_config.async_cnt = 1;
    status = pjsip_tcp_transport_start3(impl_->sip_endpoint, &tcp_config,
                                        &impl_->tcp_factory);
    if (status != PJ_SUCCESS) goto status_failure;
    impl_->tcp_port = impl_->tcp_factory->addr_name.port;
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
Error PjVoipBackend::StartOutgoingCall(const char *) { return Error::invalid_state; }
Error PjVoipBackend::AcceptCall() { return Error::invalid_state; }
Error PjVoipBackend::RejectCall(std::uint16_t) { return Error::invalid_state; }
Error PjVoipBackend::EndCall() { return Error::invalid_state; }
Error PjVoipBackend::SetHeld(bool) { return Error::invalid_state; }
RegistrationState PjVoipBackend::GetRegistrationState() const {
    return impl_ == nullptr ? RegistrationState::disabled
                            : impl_->registration_state;
}
CallInfo PjVoipBackend::GetCallInfo() const {
    CallInfo info{};
    info.state = CallState::idle;
    info.codec = Codec::pcmu;
    info.direction = MediaDirection::inactive;
    return info;
}

void *PjVoipBackend::NativeSipEndpointForValidation() const noexcept {
    return impl_ == nullptr ? nullptr : impl_->sip_endpoint;
}

} // namespace voip
