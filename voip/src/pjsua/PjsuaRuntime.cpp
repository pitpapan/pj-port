#include "PjsuaRuntime.hpp"

#include <cassert>
#include <cstring>

namespace voip {
namespace {
bool ValidFormat(const PcmFormat &format) noexcept {
    return format.sample_rate_hz != 0 && format.channels != 0 &&
           format.samples_per_frame != 0 &&
           (format.samples_per_frame * 1000U) % format.sample_rate_hz == 0;
}
} // namespace

void PjsuaRuntime::AssertActor() const noexcept {
#ifndef NDEBUG
    assert(actor_thread_ == std::thread::id{} || actor_thread_ == std::this_thread::get_id());
#endif
}

Error PjsuaRuntime::EnsureActorRegistered() noexcept {
    AssertActor();
    if (!created_) return Error::invalid_state;
    if (api_.thread_is_registered() != PJ_FALSE) return Error::ok;
    return PjsuaStatus(api_.thread_register("voip-core", actor_thread_desc_,
                                             &actor_pj_thread_));
}

Error PjsuaRuntime::CreateAndInitialize(const pjsua_callback &callbacks,
                                        const PcmFormat &format) noexcept {
    AssertActor();
    if (actor_thread_ == std::thread::id{}) actor_thread_ = std::this_thread::get_id();
    if (arena_ || created_ || !ValidFormat(format)) return Error::invalid_state;
    if (PjsuaStatus(api_.arena_install()) != Error::ok) return Error::internal_failure;
    arena_ = true;
    if (PjsuaStatus(api_.create()) != Error::ok) { (void)Destroy(); return Error::internal_failure; }
    created_ = true;
    if (EnsureActorRegistered() != Error::ok) { (void)Destroy(); return Error::internal_failure; }
    pjsua_config ua{}; pjsua_logging_config log{}; pjsua_media_config media{};
    api_.config_default(&ua); api_.logging_config_default(&log); api_.media_config_default(&media);
    ua.cb = callbacks; ua.max_calls = 7; ua.thread_cnt = 0; ua.stun_srv_cnt = 0;
    ua.enable_unsolicited_mwi = PJ_FALSE;
    ua.enable_upnp = PJ_FALSE; ua.use_srtp = PJMEDIA_SRTP_DISABLED;
    media.thread_cnt = 0; media.has_ioqueue = PJ_FALSE; media.max_media_ports = 12;
    media.clock_rate = format.sample_rate_hz; media.channel_count = format.channels;
    media.audio_frame_ptime = (format.samples_per_frame * 1000U) / format.sample_rate_hz;
    log.msg_logging = PJ_FALSE;
    if (PjsuaStatus(api_.init(&ua, &log, &media)) != Error::ok) { (void)Destroy(); return Error::internal_failure; }
    initialized_ = true;
    if (api_.set_no_sound() == nullptr) { (void)Destroy(); return Error::internal_failure; }
    return Error::ok;
}
Error PjsuaRuntime::Start() noexcept {
    AssertActor();
    if (!initialized_ || started_) return Error::invalid_state;
    if (EnsureActorRegistered() != Error::ok) return Error::internal_failure;
    if (PjsuaStatus(api_.start()) != Error::ok) return Error::internal_failure;
    started_ = true; return Error::ok;
}
Error PjsuaRuntime::Pump(std::uint32_t timeout_ms) noexcept {
    AssertActor();
    if (!started_) return Error::invalid_state;
    if (EnsureActorRegistered() != Error::ok) return Error::internal_failure;
    return api_.handle_events(timeout_ms) < 0 ? Error::internal_failure : Error::ok;
}
Error PjsuaRuntime::Destroy() noexcept {
    AssertActor();
    if (created_) {
        const Error registered = EnsureActorRegistered();
        // Native destruction uses PJLIB TLS. Retain native and arena state
        // after registration failure so a later actor turn can destroy it.
        if (registered != Error::ok) return registered;
        // Callback-owned storage and the PJ pool remain live until PJSUA
        // itself confirms destruction.  A failed destroy is recoverable on a
        // later actor turn, but resetting the arena here would not be.
        if (PjsuaStatus(api_.destroy()) != Error::ok) return Error::internal_failure;
    }
    created_ = initialized_ = started_ = false;
    // Native callbacks are gone, but a refused reset still owns arena state.
    // Let the actor retry cleanup without destroying PJSUA a second time or
    // announcing completed shutdown before zeroization succeeds.
    if (arena_ && api_.arena_reset() != PJ_SUCCESS) return Error::internal_failure;
    arena_ = false;
    std::memset(actor_thread_desc_, 0, sizeof(actor_thread_desc_));
    actor_pj_thread_ = nullptr;
    actor_thread_ = {};
    return Error::ok;
}
} // namespace voip
