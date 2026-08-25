#include <voip/PjVoipBackend.hpp>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

namespace {

int failures;

#define CHECK(condition) do {                                                   \
    if (!(condition)) {                                                         \
        printk("[SRTP keys] FAIL line %d: %s\n", __LINE__, #condition);       \
        ++failures;                                                             \
    }                                                                           \
} while (false)

bool WaitFrames(voip::VoipManager &manager) {
    for (unsigned elapsed = 0; elapsed < 3000; elapsed += 20) {
        if (manager.GetMediaStats().received_frames >= 8) return true;
        k_msleep(20);
    }
    return false;
}

void Lifecycle(unsigned number) {
    voip::PjVoipBackend backend;
    voip::VoipManager manager(backend);

    CHECK(manager.Initialize(nullptr) == voip::Error::ok);
    CHECK(backend.SrtpKeysClearedForValidation());
    CHECK(!backend.SrtpKeysActiveForValidation());

    CHECK(manager.StartHeadlessMedia(voip::Codec::pcmu) == voip::Error::ok);
    CHECK(backend.SrtpKeysActiveForValidation());
    CHECK(backend.SrtpTransportActiveForValidation());
    CHECK(!backend.SrtpKeysClearedForValidation());
    CHECK(WaitFrames(manager));
    CHECK(manager.StopMedia() == voip::Error::ok);
    CHECK(!backend.SrtpKeysActiveForValidation());
    CHECK(backend.SrtpKeysClearedForValidation());
    CHECK(!backend.SrtpTransportActiveForValidation());

    CHECK(manager.StartHeadlessMedia(voip::Codec::pcma) == voip::Error::ok);
    CHECK(backend.SrtpKeysActiveForValidation());
    CHECK(backend.SrtpTransportActiveForValidation());
    CHECK(WaitFrames(manager));
    CHECK(backend.InjectMediaTransportFailureForValidation() == voip::Error::ok);
    CHECK(manager.StopMedia() == voip::Error::ok);
    CHECK(backend.SrtpKeysClearedForValidation());

    CHECK(manager.Shutdown() == voip::Error::ok);
    CHECK(!backend.HasLiveResources());
    printk("[SRTP keys] lifecycle %u generation/zeroization: %s\n", number,
           failures == 0 ? "PASSED" : "FAILED");
}

} // namespace

int main() {
    printk("VoIP SRTP per-call key lifecycle validation\n");
    for (unsigned lifecycle = 1; lifecycle <= 3; ++lifecycle)
        Lifecycle(lifecycle);
    printk("SRTP FACADE MEDIA RESULT: %s (PCMU/PCMA; encrypted UDP)\n",
           failures == 0 ? "PASSED" : "FAILED");
    return failures == 0 ? 0 : 1;
}
