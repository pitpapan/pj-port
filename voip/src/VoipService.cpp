#include <voip/VoipService.hpp>

#include "SipManager.hpp"

#include <cstring>
#include <new>

#if defined(__ZEPHYR__)
#include <zephyr/kernel.h>
#endif

namespace voip {
namespace {

#if defined(CONFIG_VOIP_SDK_EVENT_CAPACITY)
constexpr std::size_t max_events = CONFIG_VOIP_SDK_EVENT_CAPACITY;
#else
constexpr std::size_t max_events = max_sdk_event_capacity;
#endif

bool ValidString(const char *source, std::size_t capacity) noexcept {
    if (source == nullptr) return false;
    return std::strlen(source) < capacity;
}

} // namespace

class VoipService::Impl {
public:
    explicit Impl(Backend &backend) noexcept
        : sip(backend), observer(nullptr), initialized(false),
          event_queue(nullptr), event_capacity(0), event_count(0),
          next_operation(1), pending_operation(0), account_live(false), call_live(false),
          account_handle{}, call_handle{}, registration_state(
              RegistrationState::disabled), call_info{} {
        bridge.owner = this;
#if defined(__ZEPHYR__)
        event_lock = {};
        k_work_init(&dispatch_work, &DispatchWork);
        dispatch_instance = this;
#endif
    }

    SipManager sip;

    class ObserverBridge final : public Observer {
    public:
        ObserverBridge() noexcept : owner(nullptr) {}
        Impl *owner;

        void OnRegistrationState(RegistrationState state,
                                 const Status &status) override {
            if (owner != nullptr) owner->Registration(state, status);
        }

        void OnIncomingCall(const CallInfo &info) override {
            if (owner != nullptr) owner->Incoming(info);
        }

        void OnCallState(const CallInfo &info,
                         const Status &status) override {
            if (owner != nullptr) owner->Call(info, status);
        }

        void OnMediaState(const CallInfo &info,
                          const Status &status) override {
            if (owner != nullptr) owner->Media(info, status);
        }
    } bridge;

    EventHandler *observer;
    bool initialized;
    k_work_q *event_queue;
    std::uint16_t event_capacity;
    Event events[max_events];
    std::uint16_t event_count;
    OperationId next_operation;
    OperationId pending_operation;
    bool account_live;
    bool call_live;
    AccountHandle account_handle;
    CallHandle call_handle;
    RegistrationState registration_state;
    CallInfo call_info;

#if defined(__ZEPHYR__)
    struct k_work dispatch_work;
    struct k_spinlock event_lock;
    static Impl *dispatch_instance;
#endif

    OperationId AllocateOperation() noexcept {
        const OperationId result = next_operation++;
        if (next_operation == 0) next_operation = 1;
        return result == 0 ? AllocateOperation() : result;
    }

    void Enqueue(const Event &event) noexcept {
#if defined(__ZEPHYR__)
        k_spinlock_key_t key = k_spin_lock(&event_lock);
        if (event_count < event_capacity) {
            events[event_count++] = event;
        } else {
            bool replaced = false;
            for (std::uint16_t i = 0; i < event_count; ++i) {
                if (events[i].type == event.type &&
                    events[i].account.slot == event.account.slot &&
                    events[i].account.generation == event.account.generation &&
                    events[i].call.slot == event.call.slot &&
                    events[i].call.generation == event.call.generation) {
                    events[i] = event;
                    replaced = true;
                    break;
                }
            }
            if (!replaced) {
                k_spin_unlock(&event_lock, key);
                return;
            }
        }
        k_spin_unlock(&event_lock, key);
        (void)k_work_submit_to_queue(event_queue, &dispatch_work);
#else
        if (observer != nullptr) observer->OnEvent(event);
#endif
    }

#if defined(__ZEPHYR__)
    static void DispatchWork(struct k_work *work) noexcept {
        (void)work;
        Impl *self = dispatch_instance;
        if (self == nullptr) return;
        for (;;) {
            Event event{};
            k_spinlock_key_t key = k_spin_lock(&self->event_lock);
            if (self->event_count == 0) {
                k_spin_unlock(&self->event_lock, key);
                return;
            }
            event = self->events[0];
            for (std::uint16_t i = 1; i < self->event_count; ++i)
                self->events[i - 1] = self->events[i];
            --self->event_count;
            k_spin_unlock(&self->event_lock, key);
            if (self->observer != nullptr) self->observer->OnEvent(event);
        }
    }
#endif

    void Registration(RegistrationState state, const Status &status) noexcept {
        registration_state = state;
        account_handle = sip.CurrentAccount();
        Event event{};
        event.type = EventType::account_state;
        event.account = account_handle;
        event.operation = pending_operation;
        event.registration_state = state;
        event.status = status;
        Enqueue(event);
        // Keep one operation id across the registering/unregistering
        // transition, then detach it from later unsolicited notifications.
        if (state == RegistrationState::registered ||
            state == RegistrationState::failed ||
            state == RegistrationState::disabled ||
            state == RegistrationState::connection_lost)
            pending_operation = 0;
    }

    void Incoming(const CallInfo &info) noexcept {
        call_live = true;
        account_handle = sip.CurrentAccount();
        call_handle = sip.CurrentCall();
        call_info = info;
        Event event{};
        event.type = EventType::incoming_call;
        event.account = account_handle;
        event.call = call_handle;
        event.operation = pending_operation;
        event.call_info = info;
        Enqueue(event);
    }

    void Call(const CallInfo &info, const Status &status) noexcept {
        call_live = info.state != CallState::disconnected &&
                    info.state != CallState::failed;
        account_handle = sip.CurrentAccount();
        call_handle = sip.CurrentCall();
        call_info = info;
        Event event{};
        event.type = EventType::call_state;
        event.account = account_handle;
        event.call = call_handle;
        event.operation = pending_operation;
        event.call_info = info;
        event.status = status;
        Enqueue(event);
        if (info.state == CallState::disconnected ||
            info.state == CallState::failed)
            pending_operation = 0;
    }

    void Media(const CallInfo &info, const Status &status) noexcept {
        account_handle = sip.CurrentAccount();
        call_handle = sip.CurrentCall();
        call_info = info;
        Event event{};
        event.type = EventType::media_state;
        event.account = account_handle;
        event.call = call_handle;
        event.operation = pending_operation;
        event.call_info = info;
        event.status = status;
        Enqueue(event);
    }

};

#if defined(__ZEPHYR__)
VoipService::Impl *VoipService::Impl::dispatch_instance = nullptr;
#endif

VoipService::VoipService(Backend &compatibility_backend) noexcept
    : impl_(new (std::nothrow) Impl(compatibility_backend)) {}

VoipService::~VoipService() {
    (void)Shutdown();
#if defined(__ZEPHYR__)
    if (Impl::dispatch_instance == impl_) Impl::dispatch_instance = nullptr;
#endif
    delete impl_;
}

Error VoipService::Initialize(const ServiceConfig &config,
                              EventHandler *handler) noexcept {
    if (impl_ == nullptr) return Error::internal_failure;
    if (impl_->initialized) return Error::invalid_state;
    if (handler == nullptr || config.event_queue == nullptr ||
        config.command_capacity == 0 || config.event_capacity == 0)
        return Error::invalid_argument;
    impl_->event_queue = config.event_queue;
    if (config.event_capacity > max_events) return Error::invalid_argument;
    impl_->event_capacity = config.event_capacity;
    impl_->observer = handler;
    const Error result = impl_->sip.Initialize(&impl_->bridge);
    if (result != Error::ok) {
        impl_->observer = nullptr;
        impl_->event_queue = nullptr;
        return result;
    }
    impl_->initialized = true;
    return Error::ok;
}

Error VoipService::Shutdown() noexcept {
    if (impl_ == nullptr || !impl_->initialized) return Error::ok;
    const Error result = impl_->sip.Shutdown();
#if defined(__ZEPHYR__)
    struct k_work_sync sync;
    k_work_flush(&impl_->dispatch_work, &sync);
#endif
    impl_->call_live = false;
    impl_->account_live = false;
    impl_->initialized = false;
    impl_->observer = nullptr;
    impl_->event_queue = nullptr;
    impl_->event_count = 0;
    impl_->pending_operation = 0;
    return result;
}

Error VoipService::AddAccount(const AccountConfig &config,
                              AccountHandle *handle) noexcept {
    if (impl_ == nullptr || !impl_->initialized) return Error::not_initialized;
    if (handle == nullptr ||
        !ValidString(config.account_uri, max_uri_length + 1) ||
        !ValidString(config.registrar_uri, max_uri_length + 1) ||
        !ValidString(config.username, max_username_length + 1) ||
        !ValidString(config.password, max_password_length + 1))
        return Error::invalid_argument;
    if (impl_->account_live) return Error::busy;
    const Error result = impl_->sip.AddAccount(config, handle);
    if (result != Error::ok) return result;
    impl_->account_handle = *handle;
    impl_->account_live = true;
    impl_->registration_state = RegistrationState::disabled;
    return Error::ok;
}

Error VoipService::RemoveAccount(AccountHandle handle) noexcept {
    if (impl_ == nullptr || !impl_->initialized) return Error::not_initialized;
    if (!impl_->account_live || impl_->account_handle.slot != handle.slot ||
        impl_->account_handle.generation != handle.generation)
        return Error::invalid_argument;
    if (impl_->call_live) return Error::busy;
    const Error result = impl_->sip.RemoveAccount(handle);
    if (result != Error::ok) return result;
    impl_->account_live = false;
    return Error::ok;
}

Error VoipService::SetRegistration(AccountHandle handle, bool enabled,
                                   OperationId *operation) noexcept {
    if (impl_ == nullptr || !impl_->initialized) return Error::not_initialized;
    if (!impl_->account_live || impl_->account_handle.slot != handle.slot ||
        impl_->account_handle.generation != handle.generation)
        return Error::invalid_argument;
    if (operation == nullptr) return Error::invalid_argument;
    *operation = impl_->AllocateOperation();
    impl_->pending_operation = *operation;
    return impl_->sip.SetRegistration(handle, enabled);
}

Error VoipService::Dial(AccountHandle account, const CallConfig &config,
                        CallHandle *call, OperationId *operation) noexcept {
    if (impl_ == nullptr || !impl_->initialized) return Error::not_initialized;
    if (!impl_->account_live || impl_->account_handle.slot != account.slot ||
        impl_->account_handle.generation != account.generation || call == nullptr ||
        operation == nullptr || config.source == nullptr || config.sink == nullptr)
        return Error::invalid_argument;
    if (!ValidString(config.remote_uri, max_uri_length + 1))
        return Error::invalid_argument;
    if (impl_->call_live) return Error::busy;
    impl_->account_handle = account;
    *operation = impl_->AllocateOperation();
    impl_->pending_operation = *operation;
    const Error result = impl_->sip.Dial(account, config, call);
    if (result == Error::ok) {
        impl_->call_handle = *call;
        impl_->call_live = true;
    }
    return result;
}

Error VoipService::Answer(CallHandle call, OperationId *operation) noexcept {
    if (impl_ == nullptr || !impl_->initialized) return Error::not_initialized;
    if (!impl_->call_live || impl_->call_handle.slot != call.slot ||
        impl_->call_handle.generation != call.generation || operation == nullptr)
        return Error::invalid_argument;
    *operation = impl_->AllocateOperation();
    impl_->pending_operation = *operation;
    return impl_->sip.Answer(call);
}

Error VoipService::Reject(CallHandle call, std::uint16_t status,
                          OperationId *operation) noexcept {
    if (impl_ == nullptr || !impl_->initialized) return Error::not_initialized;
    if (!impl_->call_live || impl_->call_handle.slot != call.slot ||
        impl_->call_handle.generation != call.generation || operation == nullptr)
        return Error::invalid_argument;
    *operation = impl_->AllocateOperation();
    impl_->pending_operation = *operation;
    return impl_->sip.Reject(call, status);
}

Error VoipService::Hangup(CallHandle call, OperationId *operation) noexcept {
    if (impl_ == nullptr || !impl_->initialized) return Error::not_initialized;
    if (!impl_->call_live || impl_->call_handle.slot != call.slot ||
        impl_->call_handle.generation != call.generation || operation == nullptr)
        return Error::invalid_argument;
    *operation = impl_->AllocateOperation();
    impl_->pending_operation = *operation;
    return impl_->sip.Hangup(call);
}

Error VoipService::SetHeld(CallHandle call, bool held,
                           OperationId *operation) noexcept {
    if (impl_ == nullptr || !impl_->initialized) return Error::not_initialized;
    if (!impl_->call_live || impl_->call_handle.slot != call.slot ||
        impl_->call_handle.generation != call.generation || operation == nullptr)
        return Error::invalid_argument;
    *operation = impl_->AllocateOperation();
    impl_->pending_operation = *operation;
    return impl_->sip.SetHeld(call, held);
}

Error VoipService::GetAccountState(AccountHandle handle,
                                   RegistrationState *state) const noexcept {
    if (impl_ == nullptr || !impl_->initialized) return Error::not_initialized;
    if (state == nullptr || !impl_->account_live ||
        impl_->account_handle.slot != handle.slot ||
        impl_->account_handle.generation != handle.generation)
        return Error::invalid_argument;
    *state = impl_->registration_state;
    return Error::ok;
}

Error VoipService::GetCallState(CallHandle handle,
                                CallInfo *info) const noexcept {
    if (impl_ == nullptr || !impl_->initialized) return Error::not_initialized;
    if (info == nullptr || !impl_->call_live ||
        impl_->call_handle.slot != handle.slot ||
        impl_->call_handle.generation != handle.generation)
        return Error::invalid_argument;
    *info = impl_->call_info;
    return Error::ok;
}

ResourceUsage VoipService::GetResourceUsage() const noexcept {
    ResourceUsage usage{};
    if (impl_ != nullptr) {
        usage.active_accounts = impl_->account_live ? 1 : 0;
        usage.active_calls = impl_->call_live ? 1 : 0;
    }
    return usage;
}

} // namespace voip
