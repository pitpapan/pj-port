#include <voip/PjVoipBackend.hpp>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

namespace {

int failures;

#define CHECK(condition) do {                                                   \
    if (!(condition)) {                                                         \
        printk("[Phase 2] FAIL line %d: %s\n", __LINE__, #condition);          \
        ++failures;                                                             \
    }                                                                           \
} while (false)

void TestInjectedFailures() {
    const voip::RuntimeFailurePoint points[] = {
        voip::RuntimeFailurePoint::after_pjlib,
        voip::RuntimeFailurePoint::after_pjlib_util,
        voip::RuntimeFailurePoint::after_pool_factory,
        voip::RuntimeFailurePoint::after_sip_endpoint,
        voip::RuntimeFailurePoint::after_media_endpoint,
        voip::RuntimeFailurePoint::after_tcp_factory,
        voip::RuntimeFailurePoint::after_event_thread,
    };
    for (unsigned index = 0; index < sizeof(points) / sizeof(points[0]); ++index) {
        voip::PjVoipBackend backend(points[index]);
        CHECK(backend.Initialize(nullptr) == voip::Error::internal_failure);
        CHECK(!backend.HasLiveResources());
        CHECK(backend.Shutdown() == voip::Error::ok);
        printk("[Phase 2] initialization stop %u cleanup: %s\n", index + 1,
               failures == 0 ? "PASSED" : "FAILED");
    }
}

void TestLifecycle(unsigned lifecycle) {
    voip::PjVoipBackend backend;
    voip::VoipManager manager(backend);

    CHECK(manager.Initialize(nullptr) == voip::Error::ok);
    CHECK(backend.IsRunning());
    CHECK(backend.TcpPort() != 0);

    backend.SetProbeProcessingPaused(true);
    k_sleep(K_MSEC(5));
    for (std::uint32_t value = 1; value <= 8; ++value) {
        CHECK(backend.SubmitProbe(value) == voip::Error::ok);
    }
    CHECK(backend.SubmitProbe(9) == voip::Error::queue_full);
    backend.SetProbeProcessingPaused(false);
    for (unsigned wait = 0; wait < 500 && backend.ProcessedProbeCount() != 8; ++wait) {
        k_sleep(K_MSEC(1));
    }
    CHECK(backend.ProcessedProbeCount() == 8);
    CHECK(backend.LastProbeValue() == 8);

    CHECK(backend.BeginShutdown() == voip::Error::ok);
    CHECK(backend.SubmitProbe(10) == voip::Error::shutting_down);
    CHECK(manager.Shutdown() == voip::Error::ok);
    CHECK(!backend.HasLiveResources());
    CHECK(backend.SubmitProbe(11) == voip::Error::not_initialized);

    printk("[Phase 2] lifecycle %u shared runtime teardown: %s\n", lifecycle,
           failures == 0 ? "PASSED" : "FAILED");
}

} // namespace

int main() {
    printk("VoIP integration Phase 2 shared runtime validation\n");
    TestInjectedFailures();
    for (unsigned lifecycle = 1; lifecycle <= 5; ++lifecycle) {
        TestLifecycle(lifecycle);
    }
    if (failures == 0) {
        printk("VOIP INTEGRATION PHASE 2 RESULT: PASSED (5 runtime lifecycles)\n");
    } else {
        printk("VOIP INTEGRATION PHASE 2 RESULT: FAILED (%d checks)\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
