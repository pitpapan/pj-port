#ifndef VOIP_TEST_FAKE_PJSUA_API_HPP
#define VOIP_TEST_FAKE_PJSUA_API_HPP

#include "../../src/pjsua/PjsuaApi.hpp"
#include <cstring>
#include <array>

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
    unsigned AnswerCount() const noexcept { return answer_count_; }
    unsigned HangupCount() const noexcept { return hangup_count_; }
    unsigned AnswerStatus() const noexcept { return answer_status_; }
    unsigned HangupStatus() const noexcept { return hangup_status_; }
    void SetAccountIds(const pjsua_acc_id *ids, std::size_t count) noexcept {
        account_ids_count_ = count > account_ids_.size() ? account_ids_.size() : count;
        for (std::size_t i = 0; i < account_ids_count_; ++i) account_ids_[i] = ids[i];
    }
    void FailAccountAdd(std::size_t add_number) noexcept { fail_account_add_ = add_number; }
    std::size_t AccountAddCount() const noexcept { return account_add_count_; }
    std::size_t AccountDeleteCount() const noexcept { return account_delete_count_; }
    std::size_t RegistrationCount() const noexcept { return registration_count_; }
    const pjsua_acc_config &AccountConfig(std::size_t index) const noexcept { return account_configs_[index]; }
    pjsua_acc_id DeletedAccount(std::size_t index) const noexcept { return deleted_accounts_[index]; }
private:
    static FakePjsuaApi *active_; Stage failed_ = Stage::arena; bool has_failure_ = false;
    char sequence_[160]{}; std::size_t used_ = 0;
    pjsua_config ua_{}; pjsua_media_config media_{}; pjsua_logging_config log_{};
    unsigned answer_count_ = 0; unsigned hangup_count_ = 0;
    unsigned answer_status_ = 0; unsigned hangup_status_ = 0;
    std::array<pjsua_acc_id, 5> account_ids_{{11, 3, 17, 1, 9}};
    std::array<pjsua_acc_config, 5> account_configs_{};
    std::array<pjsua_acc_id, 5> deleted_accounts_{};
    std::size_t account_ids_count_ = account_ids_.size();
    std::size_t account_add_count_ = 0, account_delete_count_ = 0;
    std::size_t registration_count_ = 0, fail_account_add_ = 0;
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
    static void AccountConfigDefault(pjsua_acc_config *x) { std::memset(x, 0, sizeof(*x)); x->transport_id = PJSUA_INVALID_ID; x->register_on_acc_add = PJ_TRUE; }
    static pj_status_t AccountAdd(const pjsua_acc_config *config, pj_bool_t, pjsua_acc_id *id) {
        active_->Record("add");
        const std::size_t index = active_->account_add_count_++;
        if (active_->fail_account_add_ != 0 && index + 1 == active_->fail_account_add_) return PJ_EUNKNOWN;
        active_->account_configs_[index] = *config;
        *id = index < active_->account_ids_count_ ? active_->account_ids_[index] : PJSUA_INVALID_ID;
        return PJ_SUCCESS;
    }
    static pj_status_t AccountSetUserData(pjsua_acc_id, void *) { active_->Record("clear"); return PJ_SUCCESS; }
    static pj_status_t AccountDelete(pjsua_acc_id id) { active_->Record("del"); active_->deleted_accounts_[active_->account_delete_count_++] = id; return PJ_SUCCESS; }
    static pj_status_t AccountSetRegistration(pjsua_acc_id, pj_bool_t) { active_->Record("reg"); ++active_->registration_count_; return PJ_SUCCESS; }
    static pj_status_t CallAnswer(pjsua_call_id, unsigned status, const pj_str_t *, const pjsua_msg_data *) { ++active_->answer_count_; active_->answer_status_ = status; active_->Record("answer"); return PJ_SUCCESS; }
    static pj_status_t CallHangup(pjsua_call_id, unsigned status, const pj_str_t *, const pjsua_msg_data *) { ++active_->hangup_count_; active_->hangup_status_ = status; active_->Record("hangup"); return PJ_SUCCESS; }
    PjsuaApi api_{ArenaInstall, ArenaReset, Create, ConfigDefault, LogDefault, MediaDefault, TransportDefault, Init, NoSound, TransportCreate, TransportClose, Start, Pump, Destroy, AccountConfigDefault, AccountAdd, AccountSetUserData, AccountDelete, AccountSetRegistration, CallAnswer, CallHangup};
};
inline FakePjsuaApi *FakePjsuaApi::active_ = nullptr;
} // namespace voip::test
#endif
