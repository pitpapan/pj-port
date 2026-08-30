#include "FakeRuntimeAdapter.hpp"

#if !defined(__ZEPHYR__) || defined(CONFIG_VOIP_SERVICE_FAKE_ADAPTER)

#include <cstring>

namespace voip {

void FakeRuntimeAdapter::FailNext(RuntimeRequest::Type type, Error error) noexcept {
    const std::size_t index = static_cast<std::size_t>(type);
    if (index < failures_.size()) failures_[index] = error;
}

Error FakeRuntimeAdapter::ConsumeFailure(RuntimeRequest::Type type) noexcept {
    const std::size_t index = static_cast<std::size_t>(type);
    if (index >= failures_.size()) return Error::ok;
    const Error error = failures_[index];
    failures_[index] = Error::ok;
    return error;
}

void FakeRuntimeAdapter::Record(const RuntimeRequest &request) noexcept {
    if (request_count_ < capacity) {
        requests_[request_count_++] = request;
        return;
    }
    for (std::size_t index = 1; index < capacity; ++index)
        requests_[index - 1] = requests_[index];
    requests_[capacity - 1] = request;
}

bool FakeRuntimeAdapter::GetRequest(std::size_t index,
                                    RuntimeRequest *request) const noexcept {
    if (request == nullptr || index >= request_count_) return false;
    *request = requests_[index];
    return true;
}

Error FakeRuntimeAdapter::Enqueue(const RuntimeNotification &notification) noexcept {
    if (callbacks_deferred_) {
        if (deferred_count_ == capacity) return Error::resource_exhausted;
        deferred_notifications_[deferred_count_++] = notification;
        return Error::ok;
    }
    return EnqueueReady(notification);
}

Error FakeRuntimeAdapter::EnqueueReady(
    const RuntimeNotification &notification) noexcept {
    if (count_ == capacity || stopped_) return Error::resource_exhausted;
    notifications_[write_] = notification;
    write_ = (write_ + 1) % capacity;
    ++count_;
    return Error::ok;
}

void FakeRuntimeAdapter::SetCallbacksDeferred(bool deferred) noexcept {
    callbacks_deferred_ = deferred;
}

void FakeRuntimeAdapter::DrainDeferredCallbacks() noexcept {
    while (deferred_count_ != 0 && count_ != capacity) {
        (void)EnqueueReady(deferred_notifications_[0]);
        for (std::size_t i = 1; i < deferred_count_; ++i)
            deferred_notifications_[i - 1] = deferred_notifications_[i];
        --deferred_count_;
    }
}

Error FakeRuntimeAdapter::InitializeAccount(const AgentContext &context) noexcept {
    if (stopped_) {
        stopped_ = false;
        read_ = 0;
        write_ = 0;
        count_ = 0;
    }
    RuntimeNotification notification{};
    RuntimeRequest request{};
    request.type = RuntimeRequest::Type::initialize_account;
    request.agent = context.handle;
    Record(request);
    const Error failure = ConsumeFailure(request.type);
    if (failure != Error::ok) return failure;
    notification.type = RuntimeNotification::Type::agent_registered;
    notification.agent = context.handle;
    return Enqueue(notification);
}

Error FakeRuntimeAdapter::PromoteOutgoing(AgentHandle agent, const char *uri,
                                          std::uint32_t *token) noexcept {
    if (token == nullptr || uri == nullptr) return Error::invalid_argument;
    RuntimeRequest request{};
    request.type = RuntimeRequest::Type::promote_outgoing;
    request.agent = agent;
    std::strncpy(request.remote_uri, uri, max_uri_length);
    Record(request);
    const Error failure = ConsumeFailure(request.type);
    if (failure != Error::ok) return failure;
    *token = next_token_++;
    if (next_token_ == 0) next_token_ = 1;
    RuntimeNotification notification{};
    notification.type = RuntimeNotification::Type::call_accepted;
    notification.agent = agent;
    notification.token = *token;
    std::strncpy(notification.remote_uri, uri, max_uri_length);
    return Enqueue(notification);
}

Error FakeRuntimeAdapter::PromoteIncoming(AgentHandle agent, std::uint32_t token,
                                          std::uint32_t *native_token) noexcept {
    if (native_token == nullptr) return Error::invalid_argument;
    RuntimeRequest request{};
    request.type = RuntimeRequest::Type::promote_incoming;
    request.agent = agent;
    request.token = token;
    Record(request);
    const Error failure = ConsumeFailure(request.type);
    if (failure != Error::ok) return failure;
    *native_token = token != 0 ? token : next_token_++;
    (void)agent;
    return Error::ok;
}

Error FakeRuntimeAdapter::Answer(std::uint32_t token) noexcept {
    RuntimeRequest request{};
    request.type = RuntimeRequest::Type::answer;
    request.token = token;
    Record(request);
    const Error failure = ConsumeFailure(request.type);
    if (failure != Error::ok) return failure;
    RuntimeNotification notification{};
    notification.type = RuntimeNotification::Type::call_accepted;
    notification.token = token;
    return Enqueue(notification);
}

Error FakeRuntimeAdapter::Reject(std::uint32_t token, std::uint16_t status) noexcept {
    RuntimeRequest request{};
    request.type = RuntimeRequest::Type::reject;
    request.token = token;
    request.sip_status = status;
    Record(request);
    const Error failure = ConsumeFailure(request.type);
    if (failure != Error::ok) return failure;
    RuntimeNotification notification{};
    notification.type = RuntimeNotification::Type::call_rejected;
    notification.token = token;
    notification.sip_status = status;
    notification.error = Error::remote_rejected;
    return Enqueue(notification);
}

Error FakeRuntimeAdapter::Cancel(std::uint32_t token) noexcept {
    RuntimeRequest request{};
    request.type = RuntimeRequest::Type::cancel;
    request.token = token;
    Record(request);
    const Error failure = ConsumeFailure(request.type);
    if (failure != Error::ok) return failure;
    RuntimeNotification notification{};
    notification.type = RuntimeNotification::Type::call_rejected;
    notification.token = token;
    notification.error = Error::cancelled;
    return Enqueue(notification);
}

Error FakeRuntimeAdapter::Hangup(std::uint32_t token) noexcept {
    RuntimeRequest request{};
    request.type = RuntimeRequest::Type::hangup;
    request.token = token;
    Record(request);
    const Error failure = ConsumeFailure(request.type);
    if (failure != Error::ok) return failure;
    RuntimeNotification notification{};
    notification.type = RuntimeNotification::Type::call_finished;
    notification.token = token;
    return Enqueue(notification);
}

Error FakeRuntimeAdapter::SetHeld(std::uint32_t token, bool held) noexcept {
    RuntimeRequest request{};
    request.type = RuntimeRequest::Type::set_held;
    request.token = token;
    request.held = held;
    Record(request);
    const Error failure = ConsumeFailure(request.type);
    if (failure != Error::ok) return failure;
    RuntimeNotification notification{};
    notification.type = held ? RuntimeNotification::Type::call_held
                             : RuntimeNotification::Type::call_resumed;
    notification.token = token;
    return Enqueue(notification);
}

bool FakeRuntimeAdapter::Poll(RuntimeNotification *notification) noexcept {
    if (notification == nullptr || count_ == 0) return false;
    *notification = notifications_[read_];
    read_ = (read_ + 1) % capacity;
    --count_;
    return true;
}

Error FakeRuntimeAdapter::BeginCallTeardown(std::uint32_t token) noexcept {
    RuntimeRequest request{};
    request.type = RuntimeRequest::Type::teardown;
    request.token = token;
    Record(request);
    const Error failure = ConsumeFailure(request.type);
    if (failure != Error::ok) return failure;
    RuntimeNotification notification{};
    notification.type = RuntimeNotification::Type::call_finished;
    notification.token = token;
    return Enqueue(notification);
}

Error FakeRuntimeAdapter::Shutdown() noexcept {
    RuntimeRequest request{};
    request.type = RuntimeRequest::Type::shutdown;
    Record(request);
    const Error failure = ConsumeFailure(request.type);
    if (failure != Error::ok) return failure;
    stopped_ = true;
    count_ = 0;
    return Error::ok;
}

Error FakeRuntimeAdapter::Inject(const RuntimeNotification &notification) noexcept {
    // Test/native injection represents an already-delivered callback and is
    // ready immediately; only callbacks generated by adapter requests defer.
    return EnqueueReady(notification);
}

} // namespace voip

#endif
