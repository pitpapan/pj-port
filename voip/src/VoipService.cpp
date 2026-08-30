#include <voip/VoipService.hpp>
#include "core/VoipRuntime.hpp"
#include <new>

namespace voip {
class VoipService::Impl final { public: Impl() noexcept = default; ~Impl() noexcept = default; VoipRuntime runtime{}; };
VoipService::VoipService() noexcept : impl_(nullptr) {
    static_assert(sizeof(Impl) <= 131072, "implementation storage budget exceeded");
    static_assert(alignof(std::max_align_t) >= alignof(Impl), "placement storage alignment");
    impl_ = ::new (static_cast<void *>(storage_)) Impl();
}
VoipService::~VoipService() noexcept { if (impl_ != nullptr) { (void)impl_->runtime.Shutdown(); impl_->~Impl(); impl_ = nullptr; } }
Error VoipService::Initialize(const ServiceConfig &c) noexcept { return impl_ ? impl_->runtime.Initialize(c) : Error::internal_failure; }
Error VoipService::Shutdown() noexcept { return impl_ ? impl_->runtime.Shutdown() : Error::internal_failure; }
Error VoipService::GetAgentHandle(std::uint8_t i, AgentHandle *h) const noexcept { return impl_ ? impl_->runtime.GetAgentHandle(i, h) : Error::internal_failure; }
Error VoipService::Dial(AgentHandle a, const DialRequest &r, CallHandle *c, OperationId *o) noexcept { return impl_ ? impl_->runtime.Dial(a, r, c, o) : Error::internal_failure; }
Error VoipService::Answer(CallHandle c, OperationId *o) noexcept { return impl_ ? impl_->runtime.Answer(c, o) : Error::internal_failure; }
Error VoipService::Reject(CallHandle c, std::uint16_t s, OperationId *o) noexcept { return impl_ ? impl_->runtime.Reject(c, s, o) : Error::internal_failure; }
Error VoipService::Cancel(CallHandle c, OperationId *o) noexcept { return impl_ ? impl_->runtime.Cancel(c, o) : Error::internal_failure; }
Error VoipService::Hangup(CallHandle c, OperationId *o) noexcept { return impl_ ? impl_->runtime.Hangup(c, o) : Error::internal_failure; }
Error VoipService::SetHeld(CallHandle c, bool h, OperationId *o) noexcept { return impl_ ? impl_->runtime.SetHeld(c, h, o) : Error::internal_failure; }
Error VoipService::TryGetEvent(Event *e) noexcept { if (!e) return Error::invalid_argument; return impl_ ? impl_->runtime.TryGetEvent(e) : Error::internal_failure; }
Error VoipService::WaitForEvent(Event *e, std::uint32_t t) noexcept { if (!e) return Error::invalid_argument; return impl_ ? impl_->runtime.WaitForEvent(e, t) : Error::internal_failure; }
Error VoipService::GetAgentSnapshot(AgentHandle h, AgentSnapshot *s) const noexcept { return impl_ ? impl_->runtime.GetAgentSnapshot(h, s) : Error::internal_failure; }
Error VoipService::GetCallSnapshot(CallHandle h, CallSnapshot *s) const noexcept { return impl_ ? impl_->runtime.GetCallSnapshot(h, s) : Error::internal_failure; }
ResourceSnapshot VoipService::GetResourceSnapshot() const noexcept { return impl_ ? impl_->runtime.GetResourceSnapshot() : ResourceSnapshot{}; }
} // namespace voip
