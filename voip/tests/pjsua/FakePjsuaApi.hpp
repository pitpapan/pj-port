#ifndef VOIP_TEST_FAKE_PJSUA_API_HPP
#define VOIP_TEST_FAKE_PJSUA_API_HPP

#include "../../src/pjsua/PjsuaApi.hpp"
#include <cstring>

namespace voip::test {
class FakePjsuaApi final {
public:
    enum class Stage { arena, create, init, no_sound, transport, start };
    FakePjsuaApi() noexcept { active_ = this; }
    void Fail(Stage stage) noexcept { failed_ = stage; has_failure_ = true; }
    bool SequenceEquals(const char *expected) const noexcept { return std::strcmp(sequence_, expected) == 0; }
    const PjsuaApi &Api() const noexcept { return api_; }
    const pjsua_config &UaConfig() const noexcept { return ua_; }
    const pjsua_media_config &MediaConfig() const noexcept { return media_; }
    const pjsua_logging_config &LoggingConfig() const noexcept { return log_; }
private:
    static FakePjsuaApi *active_; Stage failed_ = Stage::arena; bool has_failure_ = false;
    char sequence_[160]{}; std::size_t used_ = 0;
    pjsua_config ua_{}; pjsua_media_config media_{}; pjsua_logging_config log_{};
    void Record(const char *word) noexcept { if (used_ != 0) sequence_[used_++] = ','; while (*word) sequence_[used_++] = *word++; sequence_[used_] = 0; }
    bool Failed(Stage s) const noexcept { return has_failure_ && failed_ == s; }
    static pj_status_t ArenaInstall() { active_->Record("arena"); return active_->Failed(Stage::arena) ? PJ_EUNKNOWN : PJ_SUCCESS; }
    static pj_status_t ArenaReset() { active_->Record("reset"); return PJ_SUCCESS; }
    static pj_status_t Create() { active_->Record("create"); return active_->Failed(Stage::create) ? PJ_EUNKNOWN : PJ_SUCCESS; }
    static void ConfigDefault(pjsua_config *x) { std::memset(x, 0, sizeof(*x)); active_->Record("defaults"); }
    static void LogDefault(pjsua_logging_config *x) { std::memset(x, 0, sizeof(*x)); }
    static void MediaDefault(pjsua_media_config *x) { std::memset(x, 0, sizeof(*x)); }
    static void TransportDefault(pjsua_transport_config *x) { std::memset(x, 0, sizeof(*x)); }
    static pj_status_t Init(const pjsua_config *ua, const pjsua_logging_config *log, const pjsua_media_config *media) { active_->ua_ = *ua; active_->log_ = *log; active_->media_ = *media; active_->Record("init"); return active_->Failed(Stage::init) ? PJ_EUNKNOWN : PJ_SUCCESS; }
    static pjmedia_port *NoSound() { active_->Record("nosnd"); return active_->Failed(Stage::no_sound) ? nullptr : reinterpret_cast<pjmedia_port *>(1); }
    static pj_status_t TransportCreate(pjsip_transport_type_e, const pjsua_transport_config *, pjsua_transport_id *id) { active_->Record("tcp"); if (active_->Failed(Stage::transport)) return PJ_EUNKNOWN; *id = 4; return PJ_SUCCESS; }
    static pj_status_t TransportClose(pjsua_transport_id, pj_bool_t) { active_->Record("close"); return PJ_SUCCESS; }
    static pj_status_t Start() { active_->Record("start"); return active_->Failed(Stage::start) ? PJ_EUNKNOWN : PJ_SUCCESS; }
    static int Pump(unsigned) { active_->Record("pump"); return 0; }
    static pj_status_t Destroy() { active_->Record("destroy"); return PJ_SUCCESS; }
    static pj_status_t CallAnswer(pjsua_call_id, unsigned, const pj_str_t *, const pjsua_msg_data *) { return PJ_SUCCESS; }
    static pj_status_t CallHangup(pjsua_call_id, unsigned, const pj_str_t *, const pjsua_msg_data *) { return PJ_SUCCESS; }
    PjsuaApi api_{ArenaInstall, ArenaReset, Create, ConfigDefault, LogDefault, MediaDefault, TransportDefault, Init, NoSound, TransportCreate, TransportClose, Start, Pump, Destroy, CallAnswer, CallHangup};
};
inline FakePjsuaApi *FakePjsuaApi::active_ = nullptr;
} // namespace voip::test
#endif
