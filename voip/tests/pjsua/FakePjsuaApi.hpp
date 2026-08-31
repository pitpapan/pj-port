#ifndef VOIP_TEST_FAKE_PJSUA_API_HPP
#define VOIP_TEST_FAKE_PJSUA_API_HPP

#include "../../src/pjsua/PjsuaApi.hpp"
#include <cstring>
#include <array>
#include <cassert>
#include <atomic>

namespace voip::test {
class FakePjsuaApi final {
public:
    enum class Stage { arena, create, init, no_sound, transport, start };
    FakePjsuaApi() noexcept { active_ = this; }
    ~FakePjsuaApi() noexcept { if (active_ == this) active_ = nullptr; }
    void Activate() noexcept { active_ = this; }
    void Deactivate() noexcept { if (active_ == this) active_ = nullptr; }
    void Reset() noexcept {
        failed_ = Stage::arena;
        has_failure_ = false;
        std::memset(sequence_, 0, sizeof(sequence_));
        used_ = 0;
        std::memset(teardown_sequence_, 0, sizeof(teardown_sequence_));
        teardown_used_ = 0;
        std::memset(&ua_, 0, sizeof(ua_));
        std::memset(&media_, 0, sizeof(media_));
        std::memset(&log_, 0, sizeof(log_));
        answer_count_ = 0;
        hangup_count_ = 0;
        answer_status_ = 0;
        hangup_status_ = 0;
        account_ids_ = {{2, 0, 4, 1, 3}};
        std::memset(account_configs_.data(), 0, sizeof(account_configs_));
        deleted_accounts_.fill(PJSUA_INVALID_ID);
        cleared_accounts_.fill(PJSUA_INVALID_ID);
        account_user_data_.fill(nullptr);
        account_ids_count_ = account_ids_.size();
        account_add_count_ = 0;
        account_delete_count_ = 0;
        account_clear_count_ = 0;
        account_get_user_data_count_ = 0;
        account_add_slot_ = 0;
        account_delete_slot_ = 0;
        account_clear_slot_ = 0;
        registration_count_ = 0;
        fail_account_add_ = 0;
        registration_failure_ = PJ_SUCCESS;
        registration_failure_by_account_.fill(PJ_SUCCESS);
        unregistration_failure_ = PJ_SUCCESS;
        registration_by_account_.fill(0);
        pending_unregistrations_.fill(PJSUA_INVALID_ID);
        pending_unregistration_count_ = 0;
        defer_unregistration_callbacks_ = false;
        release_unregistration_callbacks_on_next_pump_.store(false);
        unregistration_callbacks_delivered_from_pump_ = 0;
        pump_count_ = 0;
        active_ = this;
    }
    static const FakePjsuaApi *ActiveForTest() noexcept { return active_; }
    void Fail(Stage stage) noexcept { failed_ = stage; has_failure_ = true; }
    bool SequenceEquals(const char *expected) const noexcept { return std::strcmp(sequence_, expected) == 0; }
    bool TeardownSequenceEquals(const char *expected) const noexcept {
        return std::strcmp(teardown_sequence_, expected) == 0;
    }
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
    std::size_t AccountClearCount() const noexcept { return account_clear_count_; }
    std::size_t AccountGetUserDataCount() const noexcept { return account_get_user_data_count_; }
    std::size_t RegistrationCount() const noexcept { return registration_count_; }
    std::size_t RegistrationCount(pjsua_acc_id id) const noexcept {
        return id >= 0 && static_cast<std::size_t>(id) < registration_by_account_.size()
            ? registration_by_account_[static_cast<std::size_t>(id)] : 0;
    }
    void FailRegistration(pj_status_t status) noexcept { registration_failure_ = status; }
    // Composition tests need one failing REGISTER without masking later
    // accounts.  Native IDs are deliberately scrambled, so this is keyed by
    // returned PJSUA account ID rather than configuration position.
    void FailRegistrationFor(pjsua_acc_id id, pj_status_t status) noexcept {
        if (id >= 0 && static_cast<std::size_t>(id) < registration_failure_by_account_.size())
            registration_failure_by_account_[static_cast<std::size_t>(id)] = status;
    }
    void FailUnregistration(pj_status_t status) noexcept { unregistration_failure_ = status; }
    void SetUnregistrationCallbacksDeferred(bool deferred) noexcept {
        defer_unregistration_callbacks_ = deferred;
    }
    // This is intentionally only a release flag.  It never invokes a PJSUA
    // callback on the application/helper thread; HandleEvents() consumes it.
    void ReleaseUnregistrationCallbacksOnNextPump() noexcept {
        release_unregistration_callbacks_on_next_pump_.store(true,
                                                               std::memory_order_release);
    }
    std::size_t PendingUnregistrationCount() const noexcept {
        return pending_unregistration_count_;
    }
    std::size_t UnregistrationCallbacksDeliveredFromPump() const noexcept {
        return unregistration_callbacks_delivered_from_pump_;
    }
    std::size_t PumpCount() const noexcept { return pump_count_; }
    void DeliverUnregistrationCallbacks() noexcept {
        while (pending_unregistration_count_ != 0) {
            const pjsua_acc_id id = pending_unregistrations_[0];
            for (std::size_t i = 1; i < pending_unregistration_count_; ++i)
                pending_unregistrations_[i - 1] = pending_unregistrations_[i];
            --pending_unregistration_count_;
            DeliverUnregistrationCallback(id);
        }
    }
    void DeliverPendingUnregistration(pjsua_acc_id id) noexcept {
        for (std::size_t index = 0; index < pending_unregistration_count_; ++index) {
            if (pending_unregistrations_[index] != id) continue;
            for (std::size_t next = index + 1; next < pending_unregistration_count_; ++next)
                pending_unregistrations_[next - 1] = pending_unregistrations_[next];
            --pending_unregistration_count_;
            DeliverUnregistrationCallback(id);
            return;
        }
    }
    void DeliverRegistrationStarted(pjsua_acc_id id, bool renew) noexcept {
        if (ua_.cb.on_reg_started2 == nullptr) return;
        pjsua_reg_info info{};
        info.renew = renew ? PJ_TRUE : PJ_FALSE;
        ua_.cb.on_reg_started2(id, &info);
    }
    void DeliverRegistrationState(pjsua_acc_id id, pj_status_t status, int code,
                                  bool renew, bool unregistration,
                                  unsigned expiration) noexcept {
        if (ua_.cb.on_reg_state2 == nullptr) return;
        pjsip_regc_cbparam params{};
        params.status = status;
        params.code = code;
        params.is_unreg = unregistration ? PJ_TRUE : PJ_FALSE;
        params.expiration = expiration;
        static char reason[] = "test";
        params.reason.ptr = reason;
        params.reason.slen = 4;
        pjsua_reg_info info{};
        info.renew = renew ? PJ_TRUE : PJ_FALSE;
        info.cbparam = &params;
        ua_.cb.on_reg_state2(id, &info);
    }
    void DeliverRegistrationFailure(pjsua_acc_id id, pj_status_t status) noexcept {
        if (ua_.cb.on_reg_state2 == nullptr) return;
        pjsip_regc_cbparam params{};
        params.status = status;
        params.code = 408;
        params.is_unreg = PJ_FALSE;
        static char reason[] = "timeout";
        params.reason.ptr = reason;
        params.reason.slen = 7;
        pjsua_reg_info info{};
        info.renew = PJ_TRUE;
        info.cbparam = &params;
        ua_.cb.on_reg_state2(id, &info);
    }
    const pjsua_acc_config &AccountConfig(std::size_t index) const noexcept { return account_configs_[index]; }
    pjsua_acc_id DeletedAccount(std::size_t index) const noexcept { return deleted_accounts_[index]; }
    pjsua_acc_id ClearedAccount(std::size_t index) const noexcept { return cleared_accounts_[index]; }
    void SetAccountUserData(pjsua_acc_id id, void *value) noexcept {
        if (id >= 0 && static_cast<std::size_t>(id) < account_user_data_.size())
            account_user_data_[static_cast<std::size_t>(id)] = value;
    }
private:
    static FakePjsuaApi *active_; Stage failed_ = Stage::arena; bool has_failure_ = false;
    // Task 5 includes five account add/register/unregister/delete records;
    // retain the complete ordering trace without overwriting fake state.
    char sequence_[512]{}; std::size_t used_ = 0;
    char teardown_sequence_[256]{}; std::size_t teardown_used_ = 0;
    pjsua_config ua_{}; pjsua_media_config media_{}; pjsua_logging_config log_{};
    unsigned answer_count_ = 0; unsigned hangup_count_ = 0;
    unsigned answer_status_ = 0; unsigned hangup_status_ = 0;
    std::array<pjsua_acc_id, 5> account_ids_{{2, 0, 4, 1, 3}};
    std::array<pjsua_acc_config, 5> account_configs_{};
    std::array<pjsua_acc_id, 5> deleted_accounts_{};
    std::array<pjsua_acc_id, 5> cleared_accounts_{};
    std::array<void *, 5> account_user_data_{};
    std::size_t account_ids_count_ = account_ids_.size();
    std::size_t account_add_count_ = 0, account_delete_count_ = 0, account_clear_count_ = 0, account_get_user_data_count_ = 0;
    // The counters above intentionally retain total calls for assertions;
    // these indexes model the native account table, which is fresh after a
    // successful pjsua_destroy()/reinitialize lifecycle.
    std::size_t account_add_slot_ = 0, account_delete_slot_ = 0, account_clear_slot_ = 0;
    std::size_t registration_count_ = 0, fail_account_add_ = 0;
    pj_status_t registration_failure_ = PJ_SUCCESS;
    std::array<pj_status_t, 5> registration_failure_by_account_{{PJ_SUCCESS, PJ_SUCCESS,
                                                                   PJ_SUCCESS, PJ_SUCCESS,
                                                                   PJ_SUCCESS}};
    pj_status_t unregistration_failure_ = PJ_SUCCESS;
    std::array<std::size_t, 5> registration_by_account_{};
    std::array<pjsua_acc_id, 5> pending_unregistrations_{};
    std::size_t pending_unregistration_count_ = 0;
    bool defer_unregistration_callbacks_ = false;
    std::atomic<bool> release_unregistration_callbacks_on_next_pump_{false};
    std::size_t unregistration_callbacks_delivered_from_pump_ = 0;
    std::size_t pump_count_ = 0;
    static FakePjsuaApi &Active() noexcept { assert(active_ != nullptr); return *active_; }
    void Record(const char *word) noexcept {
        const std::size_t word_length = std::strlen(word);
        const std::size_t separator_length = used_ == 0 ? 0 : 1;
        if (used_ + separator_length + word_length >= sizeof(sequence_)) return;
        if (used_ != 0) sequence_[used_++] = ',';
        while (*word) sequence_[used_++] = *word++;
        sequence_[used_] = 0;
    }
    void RecordTeardown(const char *word) noexcept {
        const std::size_t word_length = std::strlen(word);
        const std::size_t separator_length = teardown_used_ == 0 ? 0 : 1;
        assert(teardown_used_ + separator_length + word_length < sizeof(teardown_sequence_));
        if (teardown_used_ != 0) teardown_sequence_[teardown_used_++] = ',';
        while (*word) teardown_sequence_[teardown_used_++] = *word++;
        teardown_sequence_[teardown_used_] = 0;
    }
    bool Failed(Stage s) const noexcept { return has_failure_ && failed_ == s; }
    static pj_status_t ArenaInstall() { Active().Record("arena"); return Active().Failed(Stage::arena) ? PJ_EUNKNOWN : PJ_SUCCESS; }
    static pj_status_t ArenaReset() { Active().Record("reset"); Active().RecordTeardown("reset"); return PJ_SUCCESS; }
    static pj_status_t Create() { Active().Record("create"); return Active().Failed(Stage::create) ? PJ_EUNKNOWN : PJ_SUCCESS; }
    static void ConfigDefault(pjsua_config *x) { std::memset(x, 0, sizeof(*x)); Active().Record("defaults"); }
    static void LogDefault(pjsua_logging_config *x) { std::memset(x, 0, sizeof(*x)); (void)Active(); }
    static void MediaDefault(pjsua_media_config *x) { std::memset(x, 0, sizeof(*x)); (void)Active(); }
    static void TransportDefault(pjsua_transport_config *x) { std::memset(x, 0, sizeof(*x)); (void)Active(); }
    static pj_status_t Init(const pjsua_config *ua, const pjsua_logging_config *log, const pjsua_media_config *media) { Active().ua_ = *ua; Active().log_ = *log; Active().media_ = *media; Active().Record("init"); return Active().Failed(Stage::init) ? PJ_EUNKNOWN : PJ_SUCCESS; }
    static pjmedia_port *NoSound() { Active().Record("nosnd"); return Active().Failed(Stage::no_sound) ? nullptr : reinterpret_cast<pjmedia_port *>(1); }
    static pj_status_t TransportCreate(pjsip_transport_type_e, const pjsua_transport_config *, pjsua_transport_id *id) { Active().Record("tcp"); if (Active().Failed(Stage::transport)) return PJ_EUNKNOWN; *id = 4; return PJ_SUCCESS; }
    static pj_status_t TransportClose(pjsua_transport_id, pj_bool_t) { Active().Record("close"); Active().RecordTeardown("close"); return PJ_SUCCESS; }
    static pj_status_t Start() { Active().Record("start"); return Active().Failed(Stage::start) ? PJ_EUNKNOWN : PJ_SUCCESS; }
    static int Pump(unsigned) {
        FakePjsuaApi &fake = Active();
        ++fake.pump_count_;
        fake.Record("pump");
        if (fake.release_unregistration_callbacks_on_next_pump_.exchange(
                false, std::memory_order_acq_rel)) {
            const std::size_t pending = fake.pending_unregistration_count_;
            fake.DeliverUnregistrationCallbacks();
            fake.unregistration_callbacks_delivered_from_pump_ += pending;
        }
        return 0;
    }
    static pj_status_t Destroy() {
        Active().Record("destroy");
        Active().RecordTeardown("destroy");
        Active().account_add_slot_ = 0;
        Active().account_delete_slot_ = 0;
        Active().account_clear_slot_ = 0;
        Active().account_user_data_ = {};
        return PJ_SUCCESS;
    }
    static void AccountConfigDefault(pjsua_acc_config *x) { (void)Active(); std::memset(x, 0, sizeof(*x)); x->transport_id = PJSUA_INVALID_ID; x->register_on_acc_add = PJ_TRUE; }
    static pj_status_t AccountAdd(const pjsua_acc_config *config, pj_bool_t, pjsua_acc_id *id) {
        Active().Record("add");
        const std::size_t index = Active().account_add_count_++;
        assert(Active().account_add_slot_ < Active().account_configs_.size());
        if (Active().fail_account_add_ != 0 && index + 1 == Active().fail_account_add_) return PJ_EUNKNOWN;
        const std::size_t slot = Active().account_add_slot_++;
        Active().account_configs_[slot] = *config;
        *id = slot < Active().account_ids_count_ ? Active().account_ids_[slot] : PJSUA_INVALID_ID;
        if (*id >= 0 && static_cast<std::size_t>(*id) < Active().account_user_data_.size())
            Active().account_user_data_[static_cast<std::size_t>(*id)] = config->user_data;
        return PJ_SUCCESS;
    }
    static pj_status_t AccountSetUserData(pjsua_acc_id id, void *value) { Active().Record("clear"); Active().RecordTeardown("clear"); ++Active().account_clear_count_; assert(Active().account_clear_slot_ < Active().cleared_accounts_.size()); Active().cleared_accounts_[Active().account_clear_slot_++] = id; Active().SetAccountUserData(id, value); return PJ_SUCCESS; }
    static void *AccountGetUserData(pjsua_acc_id id) {
        ++Active().account_get_user_data_count_;
        if (id < 0 || static_cast<std::size_t>(id) >= Active().account_user_data_.size()) return nullptr;
        return Active().account_user_data_[static_cast<std::size_t>(id)];
    }
    static pj_status_t AccountDelete(pjsua_acc_id id) { Active().Record("del"); Active().RecordTeardown("del"); ++Active().account_delete_count_; assert(Active().account_delete_slot_ < Active().deleted_accounts_.size()); Active().deleted_accounts_[Active().account_delete_slot_++] = id; return PJ_SUCCESS; }
    void DeliverUnregistrationCallback(pjsua_acc_id id) noexcept {
        if (ua_.cb.on_reg_state2 == nullptr) return;
        pjsip_regc_cbparam params{};
        params.status = PJ_SUCCESS;
        params.code = 200;
        params.is_unreg = PJ_TRUE;
        pjsua_reg_info info{};
        info.renew = PJ_FALSE;
        info.cbparam = &params;
        ua_.cb.on_reg_state2(id, &info);
    }
    static pj_status_t AccountSetRegistration(pjsua_acc_id id, pj_bool_t renew) {
        Active().Record("reg");
        ++Active().registration_count_;
        if (id >= 0 && static_cast<std::size_t>(id) < Active().registration_by_account_.size())
            ++Active().registration_by_account_[static_cast<std::size_t>(id)];
        if (renew != PJ_FALSE) {
            if (id >= 0 && static_cast<std::size_t>(id) < Active().registration_failure_by_account_.size() &&
                Active().registration_failure_by_account_[static_cast<std::size_t>(id)] != PJ_SUCCESS)
                return Active().registration_failure_by_account_[static_cast<std::size_t>(id)];
            return Active().registration_failure_;
        }
        if (Active().unregistration_failure_ != PJ_SUCCESS)
            return Active().unregistration_failure_;
        if (Active().defer_unregistration_callbacks_) {
            if (Active().pending_unregistration_count_ < Active().pending_unregistrations_.size())
                Active().pending_unregistrations_[Active().pending_unregistration_count_++] = id;
        } else {
            Active().DeliverUnregistrationCallback(id);
        }
        return PJ_SUCCESS;
    }
    static pj_status_t CallAnswer(pjsua_call_id, unsigned status, const pj_str_t *, const pjsua_msg_data *) { ++Active().answer_count_; Active().answer_status_ = status; Active().Record("answer"); return PJ_SUCCESS; }
    static pj_status_t CallHangup(pjsua_call_id, unsigned status, const pj_str_t *, const pjsua_msg_data *) { ++Active().hangup_count_; Active().hangup_status_ = status; Active().Record("hangup"); return PJ_SUCCESS; }
    PjsuaApi api_{ArenaInstall, ArenaReset, Create, ConfigDefault, LogDefault, MediaDefault, TransportDefault, Init, NoSound, TransportCreate, TransportClose, Start, Pump, Destroy, AccountConfigDefault, AccountAdd, AccountSetUserData, AccountGetUserData, AccountDelete, AccountSetRegistration, CallAnswer, CallHangup};
};
inline FakePjsuaApi *FakePjsuaApi::active_ = nullptr;
} // namespace voip::test
#endif
