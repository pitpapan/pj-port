#include "CallScheduler.hpp"

#include <cstring>

namespace voip {
namespace {

std::size_t BoundedLength(const char *value) noexcept {
    if (value == nullptr) return max_uri_length + 1;
    for (std::size_t i = 0; i <= max_uri_length; ++i) {
        if (value[i] == '\0') return i;
    }
    return max_uri_length + 1;
}

} // namespace

bool CallScheduler::CopyUri(char (&destination)[max_uri_length + 1],
                            const char *source) noexcept {
    const std::size_t length = BoundedLength(source);
    if (length > max_uri_length) return false;
    if (length != 0) std::memcpy(destination, source, length);
    destination[length] = '\0';
    return true;
}

Error CallScheduler::AdmitOutgoing(AgentHandle agent, const char *remote_uri,
                                   CallHandle *handle,
                                   bool *promoted,
                                   SchedulerEffects &effects) noexcept {
    return Admit(agent, CallDirection::outgoing, 0, remote_uri, handle,
                 promoted, effects);
}

Error CallScheduler::AdmitIncoming(AgentHandle agent, std::uint32_t token,
                                   const char *remote_uri,
                                   CallHandle *handle,
                                   bool *promoted,
                                   SchedulerEffects &effects) noexcept {
    return Admit(agent, CallDirection::incoming, token, remote_uri, handle,
                 promoted, effects);
}

Error CallScheduler::Admit(AgentHandle agent, CallDirection direction,
                           std::uint32_t token, const char *remote_uri,
                           CallHandle *handle, bool *promoted,
                           SchedulerEffects &effects) noexcept {
    effects.Clear();
    if (handle == nullptr) return Error::invalid_argument;
    if (agents_.Resolve(agent) == nullptr) return Error::invalid_handle;
    if (remote_uri == nullptr) return Error::invalid_argument;
    if (direction == CallDirection::outgoing && remote_uri[0] == '\0')
        return Error::invalid_argument;
    if (direction == CallDirection::outgoing &&
        agents_.Resolve(agent)->registration != RegistrationState::registered)
        return Error::agent_unavailable;

    SchedulerEffects &sink = effects;

    const bool can_promote_now = fifo_count_ == 0 && CanPromote(agent);
    if (can_promote_now && direction == CallDirection::outgoing &&
        agents_.Resolve(agent)->registration != RegistrationState::registered)
        return Error::agent_unavailable;
    if (!can_promote_now && fifo_count_ >= fifo_capacity)
        return Error::queue_full;

    CallHandle candidate{};
    CallContext *context = calls_.Allocate(&candidate);
    if (context == nullptr) return Error::resource_exhausted;
    if (!CopyUri(context->remote_uri, remote_uri)) {
        calls_.Release(candidate);
        return Error::invalid_argument;
    }

    context->handle = candidate;
    context->agent = agent;
    context->direction = direction;
    context->runtime_token = token;
    context->phase = can_promote_now
                         ? Phase::promoting
                         : (direction == CallDirection::incoming
                                ? Phase::queued_incoming
                                : Phase::queued_outgoing);
    AppliedCallTransition initiation{};
    if (context->state_machine.Apply(CallTransition::initiation, &initiation) !=
        Error::ok) {
        calls_.Release(candidate);
        return Error::internal_failure;
    }
    ++live_count_;
    if (can_promote_now) {
        AgentContext *agent_context = agents_.Resolve(agent);
        agent_context->promoted_call = candidate;
        ++promoted_count_;
        context->phase = direction == CallDirection::incoming
                             ? Phase::incoming
                             : Phase::outgoing;
        AddPromotionEffect(*context, nullptr, sink);
        if (promoted != nullptr) *promoted = true;
    } else {
        AppliedCallTransition waiting{};
        if (context->state_machine.Apply(CallTransition::wait, &waiting) !=
            Error::ok) {
            calls_.Release(candidate);
            --live_count_;
            return Error::internal_failure;
        }
        fifo_[fifo_count_++] = candidate;
        if (promoted != nullptr) *promoted = false;
    }
    *handle = candidate;
    return Error::ok;
}

bool CallScheduler::CanPromote(const AgentHandle &agent) const noexcept {
    if (promoted_count_ >= promoted_capacity) return false;
    const AgentContext *context = agents_.Resolve(agent);
    return context != nullptr && !context->promoted_call.IsValid();
}

bool CallScheduler::SignalingEligible(const CallContext &context) const noexcept {
    const AgentContext *agent = agents_.Resolve(context.agent);
    return agent != nullptr &&
           (context.direction == CallDirection::incoming ||
            agent->registration == RegistrationState::registered);
}

bool CallScheduler::IsQueued(const CallContext &context) const noexcept {
    return context.phase == Phase::queued_incoming ||
           context.phase == Phase::queued_outgoing;
}

bool CallScheduler::RemoveFromFifo(CallHandle handle) noexcept {
    for (std::size_t i = 0; i < fifo_count_; ++i) {
        if (fifo_[i].slot != handle.slot ||
            fifo_[i].generation != handle.generation)
            continue;
        for (std::size_t j = i + 1; j < fifo_count_; ++j)
            fifo_[j - 1] = fifo_[j];
        fifo_[--fifo_count_] = CallHandle{};
        return true;
    }
    return false;
}

bool CallScheduler::PromoteContext(CallContext &context,
                                   SchedulerEffects &effects) noexcept {
    if (!CanPromote(context.agent) || !SignalingEligible(context) ||
        !effects.CanAppend())
        return false;

    AgentContext *agent = agents_.Resolve(context.agent);
    agent->promoted_call = context.handle;
    ++promoted_count_;
    context.phase = Phase::promoting;
    AddPromotionEffect(context, nullptr, effects);
    effects.entries[effects.count - 1].answer_on_promotion =
        context.answer_on_promotion;
    context.answer_on_promotion = false;
    if (context.phase == Phase::promoting)
        context.phase = context.direction == CallDirection::incoming
                             ? Phase::incoming
                             : Phase::outgoing;
    return true;
}

bool CallScheduler::PromoteHead(SchedulerEffects &effects) noexcept {
    while (fifo_count_ != 0) {
        CallContext *head = calls_.Resolve(fifo_[0]);
        if (head == nullptr) {
            for (std::size_t i = 1; i < fifo_count_; ++i)
                fifo_[i - 1] = fifo_[i];
            fifo_[--fifo_count_] = CallHandle{};
            continue;
        }
        if (!CanPromote(head->agent) || !SignalingEligible(*head) ||
            !effects.CanAppend()) return false;
        if (!PromoteContext(*head, effects)) return false;
        for (std::size_t i = 1; i < fifo_count_; ++i) fifo_[i - 1] = fifo_[i];
        fifo_[--fifo_count_] = CallHandle{};
        return true;
    }
    return false;
}

bool CallScheduler::OnCapacityChanged(SchedulerEffects &effects) noexcept {
    effects.Clear();
    bool promoted_any = false;
    while (promoted_count_ < promoted_capacity && fifo_count_ != 0) {
        if (!PromoteHead(effects)) break;
        promoted_any = true;
    }
    return promoted_any;
}

void CallScheduler::FillTransition(const CallContext &context,
                                   const AppliedCallTransition &transition,
                                   ScheduledTransition *result) const noexcept {
    if (result == nullptr) return;
    result->handle = context.handle;
    result->transition = transition;
    result->snapshot = {};
    result->snapshot.handle = context.handle;
    result->snapshot.agent = context.agent;
    result->snapshot.state = transition.after.state;
    result->snapshot.hold_reason = transition.after.hold_reason;
    std::memcpy(result->snapshot.remote_uri, context.remote_uri,
                sizeof(context.remote_uri));
    result->snapshot.remote_address[0] = '\0';
    result->snapshot.sip_status = 0;
    result->handle_invalidated = false;
}

Error CallScheduler::Apply(CallContext &context, CallTransition cause,
                           ScheduledTransition *result) noexcept {
    AppliedCallTransition transition{};
    const Error error = context.state_machine.Apply(cause, &transition);
    if (error != Error::ok) return error;
    FillTransition(context, transition, result);
    return Error::ok;
}

void CallScheduler::AddPromotionEffect(
    const CallContext &context, const AppliedCallTransition *acceptance,
    SchedulerEffects &effects) noexcept {
    PromotionEffect &entry = effects.entries[effects.count++];
    entry = {};
    entry.handle = context.handle;
    entry.direction = context.direction;
    entry.runtime_token = context.runtime_token;
    if (acceptance != nullptr) {
        entry.acceptance_applied = true;
        FillTransition(context, *acceptance, &entry.acceptance);
    }
}

CallTransition CallScheduler::CauseFor(
    const CallContext &context, CallTransition requested) const noexcept {
    const CallState state = context.state_machine.Snapshot().state;
    if (state == CallState::established &&
        (requested == CallTransition::rejection ||
         requested == CallTransition::timeout))
        return CallTransition::finish;
    if (state == CallState::initiated && requested == CallTransition::finish)
        return CallTransition::rejection;
    return requested;
}

void CallScheduler::ClearAgentLease(const CallContext &context) noexcept {
    AgentContext *agent = agents_.Resolve(context.agent);
    if (agent != nullptr && agent->promoted_call.slot == context.handle.slot &&
        agent->promoted_call.generation == context.handle.generation) {
        agent->promoted_call = CallHandle{};
        if (promoted_count_ != 0) --promoted_count_;
    }
}

bool CallScheduler::ReleaseContext(CallContext &context,
                                   ScheduledTransition *result) noexcept {
    const CallHandle handle = context.handle;
    const bool held_lease = context.phase != Phase::queued_incoming &&
                            context.phase != Phase::queued_outgoing &&
                            context.phase != Phase::free;
    if (context.state_machine.Snapshot().state == CallState::terminated) {
        AppliedCallTransition cleanup{};
        (void)context.state_machine.Apply(CallTransition::cleanup, &cleanup);
    }
    if (held_lease) ClearAgentLease(context);
    if (!calls_.Release(handle)) return false;
    if (live_count_ != 0) --live_count_;
    if (result != nullptr) result->handle_invalidated = true;
    (void)handle;
    return true;
}

void CallScheduler::SetPhaseAfterAcceptance(CallContext &context) noexcept {
    context.phase = Phase::established;
}

Error CallScheduler::Answer(CallHandle handle,
                            ScheduledTransition *result) noexcept {
    CallContext *context = calls_.Resolve(handle);
    if (context == nullptr) return Error::invalid_handle;
    if (context->direction != CallDirection::incoming)
        return Error::invalid_state;
    if (IsQueued(*context)) {
        context->answer_on_promotion = true;
        return Error::ok;
    }
    return OnAcceptance(handle, result);
}

Error CallScheduler::OnAcceptance(CallHandle handle,
                                  ScheduledTransition *result) noexcept {
    CallContext *context = calls_.Resolve(handle);
    if (context == nullptr) return Error::invalid_handle;
    if (IsQueued(*context)) return Error::invalid_state;
    const Error error = Apply(*context, CallTransition::acceptance, result);
    if (error != Error::ok) return error;
    context->answer_on_promotion = false;
    SetPhaseAfterAcceptance(*context);
    return Error::ok;
}

Error CallScheduler::Reject(CallHandle handle, ScheduledTransition *result,
                            SchedulerEffects &effects) noexcept {
    effects.Clear();
    CallContext *context = calls_.Resolve(handle);
    if (context == nullptr) return Error::invalid_handle;
    const Error error = Apply(*context,
                              CauseFor(*context, CallTransition::rejection),
                              result);
    if (error != Error::ok) return error;
    const bool queued = IsQueued(*context);
    if (queued) {
        RemoveFromFifo(handle);
        if (!ReleaseContext(*context, result)) return Error::internal_failure;
        (void)OnCapacityChanged(effects);
    } else {
        context->phase = Phase::disconnecting;
    }
    return Error::ok;
}

Error CallScheduler::Cancel(CallHandle handle, ScheduledTransition *result,
                            SchedulerEffects &effects) noexcept {
    return Reject(handle, result, effects);
}

Error CallScheduler::Hangup(CallHandle handle, ScheduledTransition *result,
                            SchedulerEffects &effects) noexcept {
    effects.Clear();
    CallContext *context = calls_.Resolve(handle);
    if (context == nullptr) return Error::invalid_handle;
    const Error error = Apply(*context,
                              CauseFor(*context, CallTransition::finish),
                              result);
    if (error != Error::ok) return error;
    if (IsQueued(*context)) {
        RemoveFromFifo(handle);
        if (!ReleaseContext(*context, result)) return Error::internal_failure;
        (void)OnCapacityChanged(effects);
    } else {
        context->phase = Phase::disconnecting;
    }
    return Error::ok;
}

Error CallScheduler::OnTimeout(CallHandle handle, ScheduledTransition *result,
                               SchedulerEffects &effects) noexcept {
    effects.Clear();
    CallContext *context = calls_.Resolve(handle);
    if (context == nullptr) return Error::invalid_handle;
    const Error error = Apply(*context,
                              CauseFor(*context, CallTransition::timeout),
                              result);
    if (error != Error::ok) return error;
    if (context->state_machine.Snapshot().state == CallState::idle) {
        if (!ReleaseContext(*context, result)) return Error::internal_failure;
        (void)OnCapacityChanged(effects);
    } else if (IsQueued(*context)) {
        RemoveFromFifo(handle);
        context->phase = Phase::disconnecting;
        if (!ReleaseContext(*context, result)) return Error::internal_failure;
        (void)OnCapacityChanged(effects);
    } else {
        context->phase = Phase::disconnecting;
    }
    return Error::ok;
}

Error CallScheduler::ApplyDeferredTerminal(
    CallHandle handle, CallTransition requested, ScheduledTransition *result,
    SchedulerEffects &effects) noexcept {
    effects.Clear();
    CallContext *context = calls_.Resolve(handle);
    if (context == nullptr) return Error::invalid_handle;
    if (!IsQueued(*context)) return Error::invalid_state;
    const Error error = Apply(*context, CauseFor(*context, requested), result);
    if (error != Error::ok) return error;
    if (!RemoveFromFifo(handle)) return Error::internal_failure;
    // Keep the slot and context live until the runtime has committed the
    // guaranteed terminal record. No promotion is attempted in this phase.
    if (result != nullptr) result->handle_invalidated = false;
    return Error::ok;
}

Error CallScheduler::RejectDeferred(CallHandle handle,
                                    ScheduledTransition *result,
                                    SchedulerEffects &effects) noexcept {
    return ApplyDeferredTerminal(handle, CallTransition::rejection, result,
                                 effects);
}

Error CallScheduler::CancelDeferred(CallHandle handle,
                                    ScheduledTransition *result,
                                    SchedulerEffects &effects) noexcept {
    return RejectDeferred(handle, result, effects);
}

Error CallScheduler::HangupDeferred(CallHandle handle,
                                    ScheduledTransition *result,
                                    SchedulerEffects &effects) noexcept {
    return ApplyDeferredTerminal(handle, CallTransition::finish, result,
                                 effects);
}

Error CallScheduler::OnTimeoutDeferred(
    CallHandle handle, ScheduledTransition *result,
    SchedulerEffects &effects) noexcept {
    return ApplyDeferredTerminal(handle, CallTransition::timeout, result,
                                 effects);
}

Error CallScheduler::FinalizeTerminal(CallHandle handle,
                                      SchedulerEffects &effects) noexcept {
    effects.Clear();
    CallContext *context = calls_.Resolve(handle);
    if (context == nullptr) return Error::invalid_handle;
    if (context->state_machine.Snapshot().state != CallState::terminated)
        return Error::invalid_state;
    if (!ReleaseContext(*context, nullptr)) return Error::internal_failure;
    (void)OnCapacityChanged(effects);
    return Error::ok;
}

Error CallScheduler::SetHeld(CallHandle handle, bool held,
                             ScheduledTransition *result) noexcept {
    CallContext *context = calls_.Resolve(handle);
    if (context == nullptr) return Error::invalid_handle;
    const CallProjection projection = context->state_machine.Snapshot();
    const CallTransition cause = held ? CallTransition::hold
                                      : CallTransition::resume;
    const Error error = Apply(*context, cause, result);
    if (error != Error::ok) return error;
    context->phase = held ? Phase::held : Phase::established;
    (void)projection;
    return Error::ok;
}

Error CallScheduler::OnTeardownComplete(
    CallHandle handle, ScheduledTransition *result,
    SchedulerEffects &effects) noexcept {
    effects.Clear();
    CallContext *context = calls_.Resolve(handle);
    if (context == nullptr) return Error::invalid_handle;
    if (context->phase != Phase::disconnecting)
        return Error::invalid_state;
    const CallState state = context->state_machine.Snapshot().state;
    if (state != CallState::terminated && state != CallState::idle)
        return Error::invalid_state;
    AppliedCallTransition cleanup{};
    if (state == CallState::terminated &&
        context->state_machine.Apply(CallTransition::cleanup, &cleanup) != Error::ok)
        return Error::internal_failure;
    if (result != nullptr) {
        result->handle = context->handle;
        result->transition = cleanup;
        result->snapshot = Snapshot(handle);
        result->handle_invalidated = false;
    }
    if (!ReleaseContext(*context, result)) return Error::internal_failure;
    if (result != nullptr) result->handle_invalidated = true;
    (void)OnCapacityChanged(effects);
    return Error::ok;
}

bool CallScheduler::IsLive(CallHandle handle) const noexcept {
    return calls_.Resolve(handle) != nullptr;
}

bool CallScheduler::IsPromoted(CallHandle handle) const noexcept {
    const CallContext *context = calls_.Resolve(handle);
    if (context == nullptr) return false;
    return context->phase != Phase::queued_incoming &&
           context->phase != Phase::queued_outgoing &&
           context->phase != Phase::free;
}

CallSnapshot CallScheduler::Snapshot(CallHandle handle) const noexcept {
    CallSnapshot snapshot{};
    const CallContext *context = calls_.Resolve(handle);
    if (context == nullptr) return snapshot;
    const CallProjection projection = context->state_machine.Snapshot();
    snapshot.handle = context->handle;
    snapshot.agent = context->agent;
    snapshot.state = projection.state;
    snapshot.hold_reason = projection.hold_reason;
    std::memcpy(snapshot.remote_uri, context->remote_uri,
                sizeof(context->remote_uri));
    snapshot.remote_address[0] = '\0';
    return snapshot;
}

Error CallScheduler::GetSnapshot(CallHandle handle,
                                 CallSnapshot *snapshot) const noexcept {
    if (snapshot == nullptr) return Error::invalid_argument;
    if (!IsLive(handle)) return Error::invalid_handle;
    *snapshot = Snapshot(handle);
    return Error::ok;
}

bool CallScheduler::QueuePosition(CallHandle handle,
                                  std::size_t *position) const noexcept {
    if (position == nullptr) return false;
    for (std::size_t i = 0; i < fifo_count_; ++i) {
        if (fifo_[i].slot == handle.slot &&
            fifo_[i].generation == handle.generation) {
            *position = i;
            return true;
        }
    }
    return false;
}

} // namespace voip
