#include "FakePjsuaApi.hpp"
#include <zephyr/sys/printk.h>
#include <cassert>

namespace voip::test {
void RunPjsuaCallApiTests() {
    FakePjsuaApi fake;
    fake.Reset();
    const PjsuaApi &api = fake.Api();
    assert(api.call_setting_default != nullptr);
    assert(api.call_make_call != nullptr);
    assert(api.call_set_user_data != nullptr);
    assert(api.call_get_user_data != nullptr);
    assert(api.call_get_info != nullptr);
    assert(api.call_answer2 != nullptr);
    assert(api.call_set_hold2 != nullptr);
    assert(api.call_reinvite2 != nullptr);

    pjsua_call_setting setting{};
    api.call_setting_default(&setting);
    char uri_text[] = "sip:call-api@example.test";
    pj_str_t uri{uri_text, static_cast<pj_ssize_t>(sizeof(uri_text) - 1)};
    pjsua_call_id call_id = PJSUA_INVALID_ID;
    assert(api.call_make_call(0, &uri, &setting, nullptr, nullptr, &call_id) == PJ_SUCCESS);
    int user_data = 42;
    assert(api.call_set_user_data(call_id, &user_data) == PJ_SUCCESS);
    assert(api.call_get_user_data(call_id) == &user_data);
    pjsua_call_info info{};
    assert(api.call_get_info(call_id, &info) == PJ_SUCCESS);
    assert(api.call_answer2(call_id, &setting, 180, nullptr, nullptr) == PJ_SUCCESS);
    assert(api.call_answer2(call_id, &setting, 200, nullptr, nullptr) == PJ_SUCCESS);
    assert(api.call_set_hold2(call_id, 0, nullptr) == PJ_SUCCESS);
    assert(api.call_reinvite2(call_id, &setting, nullptr) == PJ_SUCCESS);
    assert(api.call_hangup(call_id, 0, nullptr, nullptr) == PJ_SUCCESS);
    assert(fake.SequenceEquals(
        "call-defaults,make-call,set-call-data,get-call-data,get-call-info,"
        "answer-180,answer-200,hold,reinvite,hangup"));

    fake.Reset();
    api.call_setting_default(&setting);
    for (unsigned index = 0; index < 7; ++index) {
        assert(api.call_make_call(0, &uri, &setting, nullptr, nullptr, &call_id) == PJ_SUCCESS);
    }
    assert(api.call_make_call(0, &uri, &setting, nullptr, nullptr, &call_id) == PJ_ETOOMANY);
    assert(api.call_set_user_data(PJSUA_INVALID_ID, &user_data) != PJ_SUCCESS);
    assert(api.call_get_user_data(PJSUA_INVALID_ID) == nullptr);

    fake.FailCall(FakePjsuaApi::CallEntry::get_info, PJ_EUNKNOWN);
    assert(api.call_get_info(0, &info) == PJ_EUNKNOWN);
    fake.ClearCallFailures();
    assert(api.call_set_user_data(0, &user_data) == PJ_SUCCESS);
    fake.Reset();
    assert(api.call_get_user_data(0) == nullptr);
    printk("PjsuaCallApiTest PASSED\n");
}
} // namespace voip::test
