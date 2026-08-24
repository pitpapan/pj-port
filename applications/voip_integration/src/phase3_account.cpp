#include <voip/PjVoipBackend.hpp>

#include <zephyr/sys/printk.h>

#include <cstring>

namespace {

int failures;

#define CHECK(condition) do {                                                   \
    if (!(condition)) {                                                         \
        printk("[Phase 3] FAIL line %d: %s\n", __LINE__, #condition);          \
        ++failures;                                                             \
    }                                                                           \
} while (false)

const voip::AccountConfig account_a{
    "<sip:alice@127.0.0.1>", "sip:127.0.0.1;transport=tcp",
    "alice", "account-a-secret", 300};
const voip::AccountConfig account_b{
    "<sip:bob@127.0.0.1>", "sip:127.0.0.1;transport=tcp",
    "bob", "account-b-secret", 600};

class RemovingObserver final : public voip::Observer {
public:
    explicit RemovingObserver(voip::PjVoipBackend &backend) : backend_(backend) {}
    unsigned callbacks{};
    voip::RegistrationState last{voip::RegistrationState::disabled};

    void OnRegistrationState(voip::RegistrationState state,
                             const voip::Status &) override {
        ++callbacks;
        last = state;
        backend_.SetObserver(nullptr);
    }

private:
    voip::PjVoipBackend &backend_;
};

class RecordingObserver final : public voip::Observer {
public:
    unsigned callbacks{};
    voip::RegistrationState last{voip::RegistrationState::disabled};
    void OnRegistrationState(voip::RegistrationState state,
                             const voip::Status &) override {
        ++callbacks;
        last = state;
    }
};

void TestLifecycle(unsigned lifecycle) {
    voip::PjVoipBackend backend;
    voip::VoipManager manager(backend);
    RecordingObserver observer;
    char oversized[voip::max_uri_length + 2];
    std::memset(oversized, 'x', sizeof(oversized));
    oversized[sizeof(oversized) - 1] = '\0';

    CHECK(manager.Initialize(&observer) == voip::Error::ok);
    CHECK(!backend.HasRegistrationClient());

    const voip::AccountConfig missing{nullptr, account_a.registrar_uri,
                                      account_a.username, account_a.password, 300};
    const voip::AccountConfig zero_expiry{account_a.account_uri,
                                          account_a.registrar_uri,
                                          account_a.username,
                                          account_a.password, 0};
    const voip::AccountConfig too_long{oversized, account_a.registrar_uri,
                                       account_a.username, account_a.password, 300};
    CHECK(manager.ConfigureAccount(missing) == voip::Error::invalid_argument);
    CHECK(manager.ConfigureAccount(zero_expiry) == voip::Error::invalid_argument);
    CHECK(manager.ConfigureAccount(too_long) == voip::Error::value_too_long);
    CHECK(!backend.HasRegistrationClient());

    CHECK(manager.ConfigureAccount(account_a) == voip::Error::ok);
    CHECK(backend.HasRegistrationClient());
    CHECK(manager.RegisterAccount() == voip::Error::invalid_state);

    const voip::RegistrationState states[] = {
        voip::RegistrationState::disabled,
        voip::RegistrationState::registering,
        voip::RegistrationState::registered,
        voip::RegistrationState::unregistering,
    };
    for (const auto state : states) {
        CHECK(backend.InjectRegistrationState(state, state ==
              voip::RegistrationState::registered ? 200 : 0) == voip::Error::ok);
        CHECK(observer.last == state);
        CHECK(manager.ConfigureAccount(account_b) == voip::Error::ok);
        CHECK(backend.HasRegistrationClient());
    }

    RemovingObserver removing(backend);
    backend.SetObserver(&removing);
    CHECK(backend.InjectRegistrationState(voip::RegistrationState::registering) ==
          voip::Error::ok);
    CHECK(backend.InjectRegistrationState(voip::RegistrationState::registered, 200) ==
          voip::Error::ok);
    CHECK(removing.callbacks == 1);
    CHECK(removing.last == voip::RegistrationState::registering);

    backend.SetObserver(&observer);
    CHECK(manager.Shutdown() == voip::Error::ok);
    CHECK(!backend.HasLiveResources());
    CHECK(!backend.HasRegistrationClient());
    CHECK(observer.callbacks == 4);

    printk("[Phase 3] lifecycle %u account/regc teardown: %s\n", lifecycle,
           failures == 0 ? "PASSED" : "FAILED");
}

} // namespace

int main() {
    printk("VoIP integration Phase 3 single-account validation\n");
    for (unsigned lifecycle = 1; lifecycle <= 3; ++lifecycle) {
        TestLifecycle(lifecycle);
    }
    if (failures == 0) {
        printk("VOIP INTEGRATION PHASE 3 RESULT: PASSED (3 account lifecycles)\n");
    } else {
        printk("VOIP INTEGRATION PHASE 3 RESULT: FAILED (%d checks)\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
