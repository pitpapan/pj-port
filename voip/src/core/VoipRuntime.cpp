#include "VoipRuntime.hpp"

#include <chrono>
#include <cstring>

namespace voip {

VoipRuntime::VoipRuntime() noexcept : scheduler_(agents_) {}

VoipRuntime::~VoipRuntime() noexcept {
    (void)Shutdown();
    actor_.Stop();
}

Error VoipRuntime::Initialize(const ServiceConfig &config) noexcept {
    CoreLockGuard lock(mutex_);
    if (initialized_) return Error::invalid_state;
    // Terminal and diagnostic records remain readable after shutdown. A new
    // lifecycle is explicit only after the caller drains the prior queue.
    if (events_.Size() != 0) return Error::invalid_state;
    const Error error = agents_.Initialize(config);
    if (error != Error::ok) return error;
    mailbox_.Reset();
    events_.ResetLifecycle();
    shutting_down_ = false;
    stopped_ = false;
    shutdown_complete_ = false;
    shutdown_error_ = Error::ok;
    event_publication_failed_ = false;
    shutdown_signal_.Clear();
    call_count_ = 0;
    calls_ = {};
#if defined(__ZEPHYR__)
    now_ms_ = static_cast<std::uint64_t>(k_uptime_get());
#else
    now_ms_ = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
    queue_timeout_ms_ = config.queue_timeout_ms;
    answer_timeout_ms_ = config.answer_timeout_ms;
    for (std::size_t i = 0; i < agents_.Count(); ++i) {
        AgentHandle handle{};
        (void)agents_.GetAgentHandle(static_cast<std::uint8_t>(i), &handle);
        AgentContext *agent = agents_.Resolve(handle);
        if (agent == nullptr) continue;
        const Error adapter_error = adapter_.InitializeAccount(*agent);
        if (adapter_error != Error::ok) {
            agents_.Reset();
            // Roll back snapshots emitted for earlier accounts so a failed
            // initialization cannot strand a nonempty event lifecycle.
            events_.ResetLifecycle();
            return adapter_error;
        }
        // The deterministic adapter has no asynchronous registration delay,
        // but disabled accounts remain disabled until a future control path.
        if (agent->registration == RegistrationState::registering)
            agent->registration = RegistrationState::registered;
        PublishAgent(*agent);
    }
    initialized_ = true;
    RefreshResources();
    if (!actor_.Start(*this)) {
        initialized_ = false;
        agents_.Reset();
        return Error::internal_failure;
    }
    return Error::ok;
}

Error VoipRuntime::ReserveOperation(OperationId *operation) noexcept {
    if (operation == nullptr) return Error::invalid_argument;
    *operation = 0;
    if (!operations_.Reserve(events_, operation)) {
        return operations_.ActiveCount() == OperationTable::capacity
                   ? Error::resource_exhausted
                   : Error::queue_full;
    }
    return Error::ok;
}

Error VoipRuntime::EnqueueControl(VoipCommand &command,
                                  OperationId *operation) noexcept {
    OperationId id = 0;
    const Error error = ReserveOperation(&id);
    if (error != Error::ok) return error;
    command.operation = id;
    if (!operations_.AcceptAdmission(id)) {
        (void)operations_.RollbackAdmission(id);
        return Error::internal_failure;
    }
    if (!mailbox_.TryPush(command)) {
        (void)operations_.Abandon(id);
        return Error::queue_full;
    }
    *operation = id;
    return Error::ok;
}

void VoipRuntime::PublishAgent(const AgentContext &agent) noexcept {
    Event event{};
    event.type = EventType::agent_snapshot;
    event.agent = agent.handle;
    event.agent_snapshot.handle = agent.handle;
    event.agent_snapshot.registration = agent.registration;
    std::strncpy(event.agent_snapshot.identity_uri, agent.sip.identity_uri,
                 max_uri_length);
    (void)PublishEvent(event);
}

bool VoipRuntime::PublishEvent(const Event &event) noexcept {
    const bool published = events_.Publish(event);
    if (!published) event_publication_failed_ = true;
    return published;
}

bool VoipRuntime::ReserveCallTerminal(VoipEventQueue::Reservation *reservation,
                                      std::size_t *index) noexcept {
    if (reservation == nullptr || index == nullptr) return false;
    Event terminal{};
    terminal.type = EventType::call_state;
    terminal.destination_state = CallState::terminated;
    terminal.call_snapshot.state = CallState::terminated;
    for (std::size_t i = 0; i < call_terminals_.size(); ++i) {
        if (call_terminal_live_[i]) continue;
        if (!events_.ReserveGuaranteed(terminal, reservation)) return false;
        call_terminal_live_[i] = true;
        *index = i;
        return true;
    }
    return false;
}

bool VoipRuntime::CommitCallTerminal(CallHandle handle,
                                     const Event &event) noexcept {
    for (std::size_t i = 0; i < call_terminals_.size(); ++i) {
        if (!call_terminal_live_[i] ||
            call_terminal_handles_[i].slot != handle.slot ||
            call_terminal_handles_[i].generation != handle.generation)
            continue;
        const bool committed = call_terminals_[i].Commit(event);
        if (committed) call_terminal_live_[i] = false;
        return committed;
    }
    return false;
}

void VoipRuntime::PublishAdmission(CallHandle handle, bool waiting) noexcept {
    const CallSnapshot snapshot = scheduler_.Snapshot(handle);
    Event event{};
    event.type = EventType::call_state;
    event.call = handle;
    const CallContext *context = scheduler_.Resolve(handle);
    if (context != nullptr) event.operation = context->operation;
    event.call_snapshot = snapshot;
    event.source_state = waiting ? CallState::initiated : CallState::idle;
    event.destination_state = waiting ? CallState::hold : CallState::initiated;
    event.transition = waiting ? CallTransition::wait
                               : CallTransition::initiation;
    (void)PublishEvent(event);
}

void VoipRuntime::PublishTransition(const ScheduledTransition &transition) noexcept {
    Event event{};
    event.type = EventType::call_state;
    event.call = transition.handle;
    const CallContext *context = scheduler_.Resolve(transition.handle);
    if (context != nullptr) event.operation = context->operation;
    event.call_snapshot = transition.snapshot;
    event.source_state = transition.transition.before.state;
    event.destination_state = transition.transition.after.state;
    event.transition = transition.transition.cause;
    if (transition.transition.terminal_event_required) {
        if (!CommitCallTerminal(transition.handle, event))
            event_publication_failed_ = true;
    } else {
        (void)PublishEvent(event);
    }
}

void VoipRuntime::ApplyEffects(const SchedulerEffects &effects) noexcept {
    for (std::size_t i = 0; i < effects.count; ++i) {
        const PromotionEffect &effect = effects.entries[i];
        CallContext *context = scheduler_.Resolve(effect.handle);
        if (context == nullptr) continue;
        const OperationId signaling_operation = context->signaling_operation;
        const OperationId operation = context->operation;
        std::uint32_t token = 0;
        Error error = Error::internal_failure;
        if (effect.direction == CallDirection::incoming)
            error = adapter_.PromoteIncoming(effect.handle.IsValid() ? context->agent : AgentHandle{},
                                              context->runtime_token, &token);
        else
            error = adapter_.PromoteOutgoing(context->agent, context->remote_uri,
                                             &token);
        if (error != Error::ok) {
            SchedulerEffects ignored{};
            ScheduledTransition transition{};
            if (scheduler_.OnTimeout(effect.handle, &transition, ignored) == Error::ok)
                PublishTransition(transition);
            CompleteOperationIds(signaling_operation, operation, error);
            if (transition.handle_invalidated) {
                ForgetCall(effect.handle);
            } else {
                ScheduledTransition cleanup{};
                SchedulerEffects cleanup_effects{};
                if (scheduler_.OnTeardownComplete(effect.handle, &cleanup,
                                                  cleanup_effects) == Error::ok) {
                    ForgetCall(cleanup.handle);
                    ApplyEffects(cleanup_effects);
                }
            }
            continue;
        }
        context->runtime_token = token;
        if (effect.answer_on_promotion) {
            // A queued incoming Answer is a local decision. Native 200/Answer
            // must succeed before the business state accepts the call; the
            // resulting copied callback is processed on a later Step().
            if (adapter_.Answer(token) != Error::ok) {
                SchedulerEffects timeout_effects{};
                ScheduledTransition timeout{};
                if (scheduler_.OnTimeout(effect.handle, &timeout,
                                         timeout_effects) == Error::ok) {
                    PublishTransition(timeout);
                    CompleteOperationIds(signaling_operation, operation,
                                         Error::signaling_failed);
                    if (timeout.handle_invalidated) {
                        ForgetCall(effect.handle);
                    } else {
                        context->teardown_pending = false;
                        if (adapter_.BeginCallTeardown(token) == Error::ok)
                            context->teardown_pending = true;
                    }
                } else {
                    CompleteOperationIds(signaling_operation, operation,
                                         Error::signaling_failed);
                }
                continue;
            }
        }
        if (effect.acceptance_applied) {
            PublishTransition(effect.acceptance);
            if (context->pending_operation == CallContext::PendingOperation::answer)
                CompleteCallOperations(*context, Error::ok);
        }
    }
}

CallContext *VoipRuntime::FindCall(std::uint32_t token) noexcept {
    for (std::size_t i = 0; i < call_count_; ++i) {
        CallContext *context = scheduler_.Resolve(calls_[i]);
        if (context != nullptr && context->runtime_token == token) return context;
    }
    return nullptr;
}

const CallContext *VoipRuntime::FindCall(CallHandle handle) const noexcept {
    return scheduler_.Resolve(handle);
}

void VoipRuntime::ForgetCall(CallHandle handle) noexcept {
    for (std::size_t i = 0; i < call_count_; ++i) {
        if (calls_[i].slot != handle.slot || calls_[i].generation != handle.generation)
            continue;
        for (std::size_t j = i + 1; j < call_count_; ++j) calls_[j - 1] = calls_[j];
        calls_[--call_count_] = CallHandle{};
        return;
    }
}

void VoipRuntime::CompleteCallOperation(const CallContext &context, Error error) noexcept {
    if (context.operation != 0) (void)operations_.Complete(context.operation, error);
}

void VoipRuntime::BindCallOperation(CallContext &context,
                                    OperationId operation) noexcept {
    // Keep the original dial operation alive until native signaling resolves;
    // a later control command has its own terminal outcome. Superseded
    // controls are cancelled so every accepted operation remains observable.
    if (context.operation != 0 &&
        context.operation != context.signaling_operation)
        (void)operations_.Complete(context.operation, Error::cancelled);
    context.operation = operation;
}

void VoipRuntime::CompleteCallOperations(CallContext &context,
                                         Error error) noexcept {
    const OperationId signaling = context.signaling_operation;
    const OperationId current = context.operation;
    CompleteOperationIds(signaling, current, error);
    context.signaling_operation = 0;
    context.operation = 0;
    context.pending_operation = CallContext::PendingOperation::none;
}

void VoipRuntime::CompleteOperationIds(OperationId signaling,
                                       OperationId current,
                                       Error error) noexcept {
    if (signaling != 0) (void)operations_.Complete(signaling, error);
    if (current != 0 && current != signaling)
        (void)operations_.Complete(current, error);
}

void VoipRuntime::FinalizeTerminal(CallHandle handle,
                                   SchedulerEffects &effects) noexcept {
    if (scheduler_.FinalizeTerminal(handle, effects) != Error::ok) return;
    ForgetCall(handle);
}

void VoipRuntime::ProcessNotification(const RuntimeNotification &notification) noexcept {
    if (notification.type == RuntimeNotification::Type::agent_registered) return;
    if (notification.type == RuntimeNotification::Type::incoming_call) {
        Event incoming{};
        incoming.type = EventType::incoming_call;
        incoming.agent = notification.agent;
        incoming.call = {};
        incoming.call_snapshot.agent = notification.agent;
        std::strncpy(incoming.call_snapshot.remote_uri, notification.remote_uri,
                     max_uri_length);
        VoipEventQueue::Reservation admission_reservation;
        if (!events_.ReserveGuaranteed(incoming, &admission_reservation)) {
            (void)adapter_.Reject(notification.token, 486);
            return;
        }
        std::size_t terminal_index = 0;
        VoipEventQueue::Reservation *terminal_reservation = nullptr;
        for (std::size_t i = 0; i < call_terminals_.size(); ++i) {
            if (!call_terminal_live_[i]) {
                terminal_reservation = &call_terminals_[i];
                break;
            }
        }
        if (terminal_reservation == nullptr ||
            !ReserveCallTerminal(terminal_reservation, &terminal_index)) {
            (void)admission_reservation.Cancel();
            (void)adapter_.Reject(notification.token, 486);
            return;
        }
        CallHandle handle{};
        bool promoted = false;
        SchedulerEffects effects{};
        const Error error = scheduler_.AdmitIncoming(
            notification.agent, notification.token, notification.remote_uri,
            &handle, &promoted, effects);
        if (error != Error::ok) {
            (void)terminal_reservation->Cancel();
            call_terminal_live_[terminal_index] = false;
            (void)admission_reservation.Cancel();
            (void)adapter_.Reject(notification.token, 486);
            return;
        }
        call_terminal_handles_[terminal_index] = handle;
        if (call_count_ < calls_.size()) {
            calls_[call_count_++] = handle;
            if (CallContext *context = scheduler_.Resolve(handle))
                context->admitted_at_ms = now_ms_;
        }
        incoming.call = handle;
        incoming.call_snapshot = scheduler_.Snapshot(handle);
        if (!admission_reservation.Commit(incoming)) {
            event_publication_failed_ = true;
            ScheduledTransition rollback{};
            SchedulerEffects rollback_effects{};
            const Error rollback_error = promoted
                                             ? scheduler_.Cancel(handle, &rollback,
                                                                  rollback_effects)
                                             : scheduler_.CancelDeferred(
                                                   handle, &rollback,
                                                   rollback_effects);
            if (rollback_error == Error::ok) {
                PublishTransition(rollback);
                if (rollback.handle_invalidated) {
                    ForgetCall(handle);
                } else {
                    ScheduledTransition cleanup{};
                    SchedulerEffects cleanup_effects{};
                    if (scheduler_.OnTeardownComplete(handle, &cleanup,
                                                     cleanup_effects) == Error::ok) {
                        ForgetCall(cleanup.handle);
                        ApplyEffects(cleanup_effects);
                    }
                }
            }
            (void)admission_reservation.Cancel();
            (void)adapter_.Reject(notification.token, 486);
            return;
        }
        PublishAdmission(handle, !promoted);
        ApplyEffects(effects);
        RefreshResources();
        return;
    }
    CallContext *context = FindCall(notification.token);
    if (context == nullptr) return;
    ScheduledTransition transition{};
    SchedulerEffects effects{};
    Error error = Error::ok;
    switch (notification.type) {
    case RuntimeNotification::Type::call_accepted:
        error = scheduler_.OnAcceptance(context->handle, &transition);
        if (error == Error::ok) {
            PublishTransition(transition);
            if (context->signaling_operation != 0) {
                (void)operations_.Complete(context->signaling_operation, Error::ok);
                context->signaling_operation = 0;
            }
            if (context->pending_operation == CallContext::PendingOperation::answer)
                CompleteCallOperations(*context, Error::ok);
        }
        break;
    case RuntimeNotification::Type::call_rejected:
        {
        const OperationId signaling_operation = context->signaling_operation;
        const OperationId operation = context->operation;
        const std::uint32_t token = context->runtime_token;
        const bool queued = !scheduler_.IsPromoted(context->handle);
        error = queued
                    ? scheduler_.RejectDeferred(context->handle, &transition,
                                                effects)
                    : scheduler_.Reject(context->handle, &transition, effects);
        if (error == Error::ok) {
            PublishTransition(transition);
            CompleteOperationIds(signaling_operation, operation,
                                 notification.error == Error::ok
                                     ? Error::remote_rejected
                                     : notification.error);
            if (!queued && !transition.handle_invalidated && token != 0) {
                if (adapter_.BeginCallTeardown(token) == Error::ok) {
                    context->teardown_pending = true;
                } else {
                    context->teardown_pending = false;
                }
            }
            if (queued) FinalizeTerminal(context->handle, effects);
            ApplyEffects(effects);
        }
        }
        break;
    case RuntimeNotification::Type::call_finished:
        {
        const OperationId signaling_operation = context->signaling_operation;
        const OperationId operation = context->operation;
        const CallHandle handle = context->handle;
        if (!scheduler_.IsPromoted(handle)) {
            const bool queued = !scheduler_.IsPromoted(handle);
            const Error cancel_error = queued
                                           ? scheduler_.CancelDeferred(
                                                 handle, &transition, effects)
                                           : scheduler_.Cancel(handle, &transition,
                                                               effects);
            if (cancel_error == Error::ok) {
                PublishTransition(transition);
                CompleteOperationIds(signaling_operation, operation, Error::cancelled);
                if (queued) FinalizeTerminal(handle, effects);
                else ForgetCall(handle);
                ApplyEffects(effects);
            }
            break;
        }
        if (scheduler_.IsPromoted(context->handle) &&
            context->state_machine.Snapshot().state != CallState::terminated) {
            error = scheduler_.Hangup(context->handle, &transition, effects);
            if (error == Error::ok) PublishTransition(transition);
        }
        if (scheduler_.IsLive(context->handle)) {
            context->teardown_pending = false;
            ScheduledTransition cleanup{};
            if (scheduler_.OnTeardownComplete(context->handle, &cleanup, effects) == Error::ok) {
                CompleteOperationIds(signaling_operation, operation, Error::ok);
                ForgetCall(cleanup.handle);
                ApplyEffects(effects);
            }
        }
        }
        break;
    case RuntimeNotification::Type::call_timeout:
        {
        const OperationId signaling = context->signaling_operation;
        const OperationId operation = context->operation;
        const std::uint32_t token = context->runtime_token;
        const bool queued = !scheduler_.IsPromoted(context->handle);
        error = queued
                    ? scheduler_.OnTimeoutDeferred(context->handle, &transition,
                                                   effects)
                    : scheduler_.OnTimeout(context->handle, &transition,
                                            effects);
        if (error == Error::ok) {
            PublishTransition(transition);
            CompleteOperationIds(signaling, operation, Error::timed_out);
            if (queued) {
                FinalizeTerminal(context->handle, effects);
            } else if (transition.handle_invalidated) {
                ForgetCall(transition.handle);
            } else if (CallContext *live = scheduler_.Resolve(transition.handle)) {
                live->signaling_operation = 0;
                live->operation = 0;
                live->pending_operation = CallContext::PendingOperation::none;
                if (token != 0) (void)adapter_.BeginCallTeardown(token);
            }
            ApplyEffects(effects);
        }
        }
        break;
    case RuntimeNotification::Type::call_held:
        error = scheduler_.SetHeld(context->handle, true, &transition);
        if (error == Error::ok) {
            PublishTransition(transition);
            CompleteCallOperations(*context, Error::ok);
        }
        break;
    case RuntimeNotification::Type::call_resumed:
        error = scheduler_.SetHeld(context->handle, false, &transition);
        if (error == Error::ok) {
            PublishTransition(transition);
            CompleteCallOperations(*context, Error::ok);
        }
        break;
    case RuntimeNotification::Type::incoming_call:
        break;
    case RuntimeNotification::Type::agent_registered:
        break;
    }
}

void VoipRuntime::ProcessCommand(const VoipCommand &command) noexcept {
    if (command.type == CommandType::shutdown) {
        CancelAllCalls();
        const Error adapter_error = adapter_.Shutdown();
        if (adapter_error != Error::ok) {
            shutdown_error_ = adapter_error;
            shutdown_complete_ = true;
            shutdown_signal_.Notify();
            return;
        }
        VoipEventQueue::Reservation stopped;
        if (!events_.ReserveServiceStopped(&stopped) ||
            !events_.CommitServiceStopped(&stopped)) {
            event_publication_failed_ = true;
            return;
        }
        stopped_ = true;
        initialized_ = false;
        shutdown_complete_ = true;
        shutdown_signal_.Notify();
        return;
    }
    if (command.type == CommandType::dial) {
        std::size_t terminal_index = 0;
        VoipEventQueue::Reservation *terminal_reservation = nullptr;
        for (std::size_t i = 0; i < call_terminals_.size(); ++i) {
            if (!call_terminal_live_[i]) {
                terminal_reservation = &call_terminals_[i];
                break;
            }
        }
        if (terminal_reservation == nullptr ||
            !ReserveCallTerminal(terminal_reservation, &terminal_index)) {
            (void)operations_.Complete(command.operation, Error::resource_exhausted);
            return;
        }
        SchedulerEffects effects{};
        CallHandle handle{};
        bool promoted = false;
        const Error error = scheduler_.AdmitOutgoing(
            command.dial.agent, command.dial.remote_uri, &handle, &promoted,
            effects);
        if (error != Error::ok) {
            (void)terminal_reservation->Cancel();
            call_terminal_live_[terminal_index] = false;
            (void)operations_.Complete(command.operation, error);
            return;
        }
        call_terminal_handles_[terminal_index] = handle;
        CallContext *context = scheduler_.Resolve(handle);
        if (context == nullptr) {
            (void)terminal_reservation->Cancel();
            call_terminal_live_[terminal_index] = false;
            (void)operations_.Complete(command.operation, Error::internal_failure);
            return;
        }
        context->admitted_at_ms = now_ms_;
        BindCallOperation(*context, command.operation);
        context->signaling_operation = command.operation;
        if (call_count_ < calls_.size()) calls_[call_count_++] = handle;
        PublishAdmission(handle, !promoted);
        ApplyEffects(effects);
        return;
    }
    CallContext *context = nullptr;
    CallHandle handle{};
    switch (command.type) {
    case CommandType::answer: handle = command.answer.call; break;
    case CommandType::reject: handle = command.reject.call; break;
    case CommandType::cancel: handle = command.cancel.call; break;
    case CommandType::hangup: handle = command.hangup.call; break;
    case CommandType::set_held: handle = command.set_held.call; break;
    case CommandType::shutdown:
    case CommandType::dial: break;
    }
    context = scheduler_.Resolve(handle);
    if (context == nullptr) {
        (void)operations_.Complete(command.operation, Error::invalid_handle);
        return;
    }
    const OperationId signaling_operation = context->signaling_operation;
    const CallContext::PendingOperation previous_pending =
        context->pending_operation;
    SchedulerEffects effects{};
    ScheduledTransition transition{};
    Error error = Error::ok;
    bool deferred_native = false;
    bool local_transition = false;
    bool local_answer = false;
    switch (command.type) {
    case CommandType::answer:
        context->pending_operation = CallContext::PendingOperation::answer;
        if (scheduler_.IsPromoted(handle))
            error = adapter_.Answer(context->runtime_token);
        else {
            error = scheduler_.Answer(handle, &transition);
            local_answer = error == Error::ok;
        }
        break;
    case CommandType::reject:
        context->pending_operation = CallContext::PendingOperation::reject;
        if (scheduler_.IsPromoted(handle)) {
            error = adapter_.Reject(context->runtime_token,
                                    command.reject.sip_status);
        } else if (context->direction == CallDirection::incoming &&
                   context->runtime_token != 0) {
            deferred_native = true;
            error = adapter_.Reject(context->runtime_token,
                                    command.reject.sip_status);
        } else {
            const bool queued = !scheduler_.IsPromoted(handle);
            error = queued
                        ? scheduler_.RejectDeferred(handle, &transition, effects)
                        : scheduler_.Reject(handle, &transition, effects);
            local_transition = error == Error::ok;
        }
        break;
    case CommandType::cancel:
        context->pending_operation = CallContext::PendingOperation::cancel;
        if (scheduler_.IsPromoted(handle)) error = adapter_.Cancel(context->runtime_token);
        else if (context->direction == CallDirection::incoming &&
                 context->runtime_token != 0) {
            deferred_native = true;
            error = adapter_.Cancel(context->runtime_token);
        }
        else if (!scheduler_.IsPromoted(handle))
            error = scheduler_.CancelDeferred(handle, &transition, effects);
        else error = scheduler_.Cancel(handle, &transition, effects);
        local_transition = !scheduler_.IsPromoted(handle) && error == Error::ok;
        break;
    case CommandType::hangup:
        context->pending_operation = CallContext::PendingOperation::hangup;
        if (scheduler_.IsPromoted(handle)) error = adapter_.Hangup(context->runtime_token);
        else if (!scheduler_.IsPromoted(handle))
            error = scheduler_.HangupDeferred(handle, &transition, effects);
        else error = scheduler_.Hangup(handle, &transition, effects);
        local_transition = !scheduler_.IsPromoted(handle) && error == Error::ok;
        break;
    case CommandType::set_held:
        context->pending_operation = CallContext::PendingOperation::set_held;
        if (!scheduler_.IsPromoted(handle))
            error = scheduler_.SetHeld(handle, command.set_held.held, &transition);
        else
            error = adapter_.SetHeld(context->runtime_token, command.set_held.held);
        break;
    case CommandType::dial:
    case CommandType::shutdown:
        break;
    }
    if (error != Error::ok) {
        (void)operations_.Complete(command.operation, error);
        context->pending_operation = previous_pending;
        return;
    }
    const OperationId previous_operation = context->operation;
    if (local_answer) {
        (void)operations_.Complete(command.operation, Error::ok);
        context->pending_operation = CallContext::PendingOperation::none;
    } else if (!transition.handle_invalidated)
        BindCallOperation(*context, command.operation);
    if (command.type == CommandType::set_held) {
        // The state transition and operation completion occur only when the
        // adapter reports the negotiated media change.
    } else if (local_transition && !deferred_native && !scheduler_.IsPromoted(handle)) {
        PublishTransition(transition);
        const Error terminal = command.type == CommandType::cancel
                                   ? Error::cancelled
                                   : command.type == CommandType::reject
                                         ? Error::remote_rejected
                                         : Error::ok;
        CompleteOperationIds(signaling_operation, command.operation, terminal);
        if (previous_operation != 0 && previous_operation != signaling_operation)
            (void)operations_.Complete(previous_operation, terminal);
        if (transition.handle_invalidated) {
            ForgetCall(handle);
        } else {
            FinalizeTerminal(handle, effects);
        }
    }
    ApplyEffects(effects);
}

Error VoipRuntime::Dial(AgentHandle agent, const DialRequest &request,
                        OperationId *operation) noexcept {
    CoreLockGuard lock(mutex_);
    if (!initialized_ || shutting_down_) return Error::shutting_down;
    if (operation == nullptr || request.remote_uri == nullptr)
        return Error::invalid_argument;
    *operation = 0;
    if (agents_.Resolve(agent) == nullptr) return Error::invalid_handle;
    VoipCommand command{};
    command.type = CommandType::dial;
    command.dial.agent = agent;
    std::size_t length = 0;
    while (length <= max_uri_length && request.remote_uri[length] != '\0') ++length;
    if (length == 0 || length > max_uri_length) return Error::invalid_argument;
    std::memcpy(command.dial.remote_uri, request.remote_uri, length + 1);
    const Error error = EnqueueControl(command, operation);
    RefreshResources();
    return error;
}

Error VoipRuntime::Answer(CallHandle call, OperationId *operation) noexcept {
    CoreLockGuard lock(mutex_);
    if (!initialized_ || shutting_down_) return Error::shutting_down;
    if (operation == nullptr) return Error::invalid_argument;
    if (scheduler_.Resolve(call) == nullptr) return Error::invalid_handle;
    VoipCommand command{};
    command.type = CommandType::answer;
    command.answer.call = call;
    const Error error = EnqueueControl(command, operation);
    RefreshResources();
    return error;
}

Error VoipRuntime::Reject(CallHandle call, std::uint16_t status,
                          OperationId *operation) noexcept {
    CoreLockGuard lock(mutex_);
    if (!initialized_ || shutting_down_) return Error::shutting_down;
    if (operation == nullptr) return Error::invalid_argument;
    if (scheduler_.Resolve(call) == nullptr) return Error::invalid_handle;
    VoipCommand command{};
    command.type = CommandType::reject;
    command.reject.call = call;
    command.reject.sip_status = status;
    const Error error = EnqueueControl(command, operation);
    RefreshResources();
    return error;
}

Error VoipRuntime::Cancel(CallHandle call, OperationId *operation) noexcept {
    CoreLockGuard lock(mutex_);
    if (!initialized_ || shutting_down_) return Error::shutting_down;
    if (operation == nullptr) return Error::invalid_argument;
    if (scheduler_.Resolve(call) == nullptr) return Error::invalid_handle;
    VoipCommand command{};
    command.type = CommandType::cancel;
    command.cancel.call = call;
    const Error error = EnqueueControl(command, operation);
    RefreshResources();
    return error;
}

Error VoipRuntime::Hangup(CallHandle call, OperationId *operation) noexcept {
    CoreLockGuard lock(mutex_);
    if (!initialized_ || shutting_down_) return Error::shutting_down;
    if (operation == nullptr) return Error::invalid_argument;
    if (scheduler_.Resolve(call) == nullptr) return Error::invalid_handle;
    VoipCommand command{};
    command.type = CommandType::hangup;
    command.hangup.call = call;
    const Error error = EnqueueControl(command, operation);
    RefreshResources();
    return error;
}

Error VoipRuntime::SetHeld(CallHandle call, bool held, OperationId *operation) noexcept {
    CoreLockGuard lock(mutex_);
    if (!initialized_ || shutting_down_) return Error::shutting_down;
    if (operation == nullptr) return Error::invalid_argument;
    if (scheduler_.Resolve(call) == nullptr) return Error::invalid_handle;
    VoipCommand command{};
    command.type = CommandType::set_held;
    command.set_held.call = call;
    command.set_held.held = held;
    const Error error = EnqueueControl(command, operation);
    RefreshResources();
    return error;
}

void VoipRuntime::CancelAllCalls() noexcept {
    for (std::size_t index = call_count_; index != 0; --index) {
        const CallHandle handle = calls_[index - 1];
        CallContext *context = scheduler_.Resolve(handle);
        if (context == nullptr) { ForgetCall(handle); continue; }
        const OperationId signaling_operation = context->signaling_operation;
        const OperationId operation = context->operation;
        const bool promoted = scheduler_.IsPromoted(handle);
        const std::uint32_t token = context->runtime_token;
        const bool queued_incoming = !promoted &&
                                     context->direction == CallDirection::incoming &&
                                     token != 0;
        if (promoted && token != 0) {
            (void)adapter_.Hangup(token);
            (void)adapter_.BeginCallTeardown(token);
        } else if (queued_incoming) {
            (void)adapter_.Reject(token, 486);
        }
        ScheduledTransition transition{}; SchedulerEffects effects{};
        const Error cancel_error = (!promoted)
                                       ? scheduler_.CancelDeferred(
                                             handle, &transition, effects)
                                       : scheduler_.Cancel(handle, &transition,
                                                           effects);
        if (cancel_error == Error::ok) {
            PublishTransition(transition);
            if (transition.handle_invalidated) {
                CompleteOperationIds(signaling_operation, operation, Error::cancelled);
                ForgetCall(handle);
            } else if (!promoted) {
                CompleteOperationIds(signaling_operation, operation,
                                     Error::cancelled);
                FinalizeTerminal(handle, effects);
            } else if (scheduler_.OnTeardownComplete(handle, nullptr, effects) == Error::ok) {
                CompleteOperationIds(signaling_operation, operation, Error::cancelled);
                ForgetCall(handle);
            }
            // No promotion is permitted once shutdown starts. Any effects
            // returned by cancellation are intentionally discarded; the
            // remaining FIFO entries are cancelled by this same pass.
        }
    }
}

Error VoipRuntime::Shutdown() noexcept {
    {
        CoreLockGuard lock(mutex_);
        if (shutdown_complete_) return shutdown_error_;
        if (!initialized_ || stopped_) return Error::ok;
        shutting_down_ = true;
        shutdown_complete_ = false;
        if (!mailbox_.TryPushShutdown()) return Error::shutdown_timeout;
    }
    if (!shutdown_signal_.Wait(1000)) return Error::shutdown_timeout;
    actor_.Stop();
    Error result = Error::ok;
    {
        CoreLockGuard lock(mutex_);
        result = shutdown_error_;
        if (result == Error::ok) {
            agents_.Reset();
            RefreshResources();
        }
    }
    return result;
}

void VoipRuntime::ApplyTimers(std::uint64_t now_ms) noexcept {
    for (std::size_t index = call_count_; index != 0; --index) {
        const CallHandle handle = calls_[index - 1];
        CallContext *context = scheduler_.Resolve(handle);
        if (context == nullptr) {
            ForgetCall(handle);
            continue;
        }
        const CallState state = context->state_machine.Snapshot().state;
        const bool queued = !scheduler_.IsPromoted(handle);
        const std::uint32_t timeout = queued ? queue_timeout_ms_
                                             : answer_timeout_ms_;
        const bool waiting_for_acceptance =
            state == CallState::initiated ||
            (state == CallState::hold &&
             context->state_machine.Snapshot().hold_reason == HoldReason::waiting);
        if (timeout == 0 || now_ms < context->admitted_at_ms ||
            now_ms - context->admitted_at_ms < timeout ||
            !waiting_for_acceptance)
            continue;
        ScheduledTransition transition{};
        SchedulerEffects effects{};
        const OperationId signaling = context->signaling_operation;
        const OperationId operation = context->operation;
        const std::uint32_t token = context->runtime_token;
        const Error timeout_error = queued
                                        ? scheduler_.OnTimeoutDeferred(
                                              handle, &transition, effects)
                                        : scheduler_.OnTimeout(handle, &transition,
                                                               effects);
        if (timeout_error != Error::ok)
            continue;
        PublishTransition(transition);
        CompleteOperationIds(signaling, operation, Error::timed_out);
        if (queued) {
            FinalizeTerminal(handle, effects);
        } else if (transition.handle_invalidated) {
            ForgetCall(handle);
        } else if (CallContext *live = scheduler_.Resolve(handle)) {
            live->signaling_operation = 0;
            live->operation = 0;
            live->pending_operation = CallContext::PendingOperation::none;
            live->teardown_pending = false;
            if (token != 0 && adapter_.BeginCallTeardown(token) == Error::ok) {
                live->teardown_pending = true;
            }
        }
        ApplyEffects(effects);
    }
}

void VoipRuntime::RetryPendingTeardowns() noexcept {
    for (std::size_t index = 0; index < call_count_; ++index) {
        CallContext *context = scheduler_.Resolve(calls_[index]);
        if (context == nullptr || context->teardown_pending ||
            context->state_machine.Snapshot().state != CallState::terminated)
            continue;
        if (context->runtime_token != 0 &&
            adapter_.BeginCallTeardown(context->runtime_token) == Error::ok)
            context->teardown_pending = true;
    }
}

void VoipRuntime::Step(std::uint64_t now_ms) noexcept {
    CoreLockGuard lock(mutex_);
    if (!initialized_ || stopped_) return;
    now_ms_ = now_ms;
    // Public admission copies into the fixed mailbox before actor-side
    // effects are applied. Current host composition performs those effects
    // under the same bounded runtime lock; still drain any queued records so
    // a producer cannot leave command capacity permanently consumed.
    VoipCommand command{};
    while (mailbox_.TryPop(&command)) {
        ProcessCommand(command);
    }
    ApplyTimers(now_ms);
    RetryPendingTeardowns();
    RuntimeNotification notification{};
    for (std::size_t i = 0; i < RuntimeAdapter::notification_capacity; ++i) {
        if (!adapter_.Poll(&notification)) break;
        ProcessNotification(notification);
    }
    RefreshResources();
}

Error VoipRuntime::InjectNotification(
    const RuntimeNotification &notification) noexcept {
#if !defined(__ZEPHYR__) || defined(CONFIG_VOIP_SERVICE_FAKE_ADAPTER)
    CoreLockGuard lock(mutex_);
    if (!initialized_ || shutting_down_) return Error::shutting_down;
    return adapter_.Inject(notification);
#else
    (void)notification;
    return Error::unsupported_configuration;
#endif
}

void VoipRuntime::FailNextAdapter(RuntimeRequest::Type type, Error error) noexcept {
#if !defined(__ZEPHYR__) || defined(CONFIG_VOIP_SERVICE_FAKE_ADAPTER)
    CoreLockGuard lock(mutex_);
    adapter_.FailNext(type, error);
#else
    (void)type;
    (void)error;
#endif
}

void VoipRuntime::RefreshResources() noexcept {
    ResourceSnapshot snapshot{};
    snapshot.active_agents = static_cast<std::uint8_t>(agents_.Count());
    snapshot.active_calls = static_cast<std::uint8_t>(scheduler_.LiveCount());
    snapshot.promoted_calls = static_cast<std::uint8_t>(scheduler_.PromotedCount());
    snapshot.queued_calls = static_cast<std::uint8_t>(scheduler_.QueuedCount());
    snapshot.available_commands = static_cast<std::uint16_t>(mailbox_.Available());
    snapshot.available_operations = static_cast<std::uint16_t>(operations_.Available());
    snapshot.available_events = static_cast<std::uint16_t>(
        events_.AvailableRecords());
    snapshot.available_fifo_entries = static_cast<std::uint16_t>(scheduler_.AvailableFifoEntries());
    snapshot.available_logical_calls = static_cast<std::uint16_t>(scheduler_.AvailableLogicalCalls());
    snapshot.available_promoted_calls = static_cast<std::uint16_t>(scheduler_.AvailablePromotedCalls());
    snapshot.available_media_bridges = static_cast<std::uint16_t>(scheduler_.AvailablePromotedCalls());
    snapshot.available_call_slots = snapshot.available_logical_calls;
    resources_.Update(snapshot);
}

Error VoipRuntime::GetAgentHandle(std::uint8_t index, AgentHandle *handle) const noexcept {
    CoreLockGuard lock(mutex_); return agents_.GetAgentHandle(index, handle);
}

Error VoipRuntime::WaitForEvent(Event *event, std::uint32_t timeout_ms) noexcept {
    return events_.WaitPop(event, timeout_ms) ? Error::ok : Error::timed_out;
}

Error VoipRuntime::GetAgentSnapshot(AgentHandle handle, AgentSnapshot *snapshot) const noexcept {
    if (snapshot == nullptr) return Error::invalid_argument;
    CoreLockGuard lock(mutex_); const AgentContext *agent = agents_.Resolve(handle);
    if (agent == nullptr) return Error::invalid_handle;
    *snapshot = {}; snapshot->handle = handle; snapshot->registration = agent->registration;
    std::strncpy(snapshot->identity_uri, agent->sip.identity_uri, max_uri_length); return Error::ok;
}

Error VoipRuntime::GetCallSnapshot(CallHandle handle, CallSnapshot *snapshot) const noexcept {
    if (snapshot == nullptr) return Error::invalid_argument;
    CoreLockGuard lock(mutex_); return scheduler_.GetSnapshot(handle, snapshot);
}

ResourceSnapshot VoipRuntime::GetResourceSnapshot() const noexcept {
    CoreLockGuard lock(mutex_); return resources_.Snapshot();
}

bool VoipRuntime::Validate(AgentHandle handle) const noexcept {
    return agents_.Resolve(handle) != nullptr;
}
bool VoipRuntime::Validate(CallHandle handle) const noexcept {
    return scheduler_.IsLive(handle);
}

std::size_t VoipRuntime::AdapterRequestCount() const noexcept {
#if !defined(__ZEPHYR__) || defined(CONFIG_VOIP_SERVICE_FAKE_ADAPTER)
    CoreLockGuard lock(mutex_);
    return adapter_.RequestCount();
#else
    return 0;
#endif
}

bool VoipRuntime::GetAdapterRequest(std::size_t index,
                                    RuntimeRequest *request) const noexcept {
#if !defined(__ZEPHYR__) || defined(CONFIG_VOIP_SERVICE_FAKE_ADAPTER)
    CoreLockGuard lock(mutex_);
    return adapter_.GetRequest(index, request);
#else
    (void)index;
    (void)request;
    return false;
#endif
}

} // namespace voip
