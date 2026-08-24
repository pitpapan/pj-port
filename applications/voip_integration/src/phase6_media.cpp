#include <voip/PjVoipBackend.hpp>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>

namespace {

int failures;

#define CHECK(condition) do {                                                   \
    if (!(condition)) {                                                         \
        printk("[Phase 6] FAIL line %d: %s\n", __LINE__, #condition);          \
        ++failures;                                                             \
    }                                                                           \
} while (false)

class RecordingObserver final : public voip::Observer {
public:
    atomic_t media_callbacks{};
    voip::MediaDirection last_direction{voip::MediaDirection::inactive};

    void OnMediaState(const voip::CallInfo &info,
                      const voip::Status &status) override {
        if (status.error != voip::Error::ok) ++failures;
        last_direction = info.direction;
        atomic_inc(&media_callbacks);
    }
};

bool WaitFrames(voip::VoipManager &manager, std::uint32_t minimum) {
    for (unsigned elapsed = 0; elapsed < 5000; elapsed += 20) {
        if (manager.GetMediaStats().received_frames >= minimum) return true;
        k_msleep(20);
    }
    return false;
}

void ExerciseCodec(voip::VoipManager &manager, RecordingObserver &observer,
                   voip::Codec codec, atomic_val_t callback_base) {
    CHECK(manager.StartHeadlessMedia(codec) == voip::Error::ok);
    CHECK(manager.StartHeadlessMedia(codec) == voip::Error::busy);
    CHECK(WaitFrames(manager, 24));
    voip::MediaStats active = manager.GetMediaStats();
    CHECK(active.generated_frames >= 24);
    CHECK(active.received_frames >= 24);
    CHECK(active.rtp_packets_sent >= 24);
    CHECK(active.rtp_packets_received >= 20);
    CHECK(active.sink_hash != 2166136261U);
    CHECK(active.sink_capacity_frames == 8);
    CHECK(active.sink_peak_frames == active.sink_capacity_frames);

    CHECK(manager.SetMediaPaused(true) == voip::Error::ok);
    const std::uint32_t paused_frames = manager.GetMediaStats().generated_frames;
    k_msleep(80);
    CHECK(manager.GetMediaStats().generated_frames <= paused_frames + 1);
    CHECK(observer.last_direction == voip::MediaDirection::inactive);
    CHECK(manager.SetMediaPaused(false) == voip::Error::ok);
    CHECK(observer.last_direction == voip::MediaDirection::send_receive);
    CHECK(WaitFrames(manager, active.received_frames + 12));
    CHECK(manager.StopMedia() == voip::Error::ok);
    CHECK(manager.StopMedia() == voip::Error::invalid_state);
    CHECK(observer.last_direction == voip::MediaDirection::inactive);
    CHECK(atomic_get(&observer.media_callbacks) >= callback_base + 4);
}

void Lifecycle(unsigned number) {
    voip::PjVoipBackend backend;
    voip::VoipManager manager(backend);
    RecordingObserver observer;
    CHECK(manager.Initialize(&observer) == voip::Error::ok);
    ExerciseCodec(manager, observer, voip::Codec::pcmu, 0);
    ExerciseCodec(manager, observer, voip::Codec::pcma, 4);
    CHECK(manager.StartHeadlessMedia(voip::Codec::pcmu) == voip::Error::ok);
    CHECK(WaitFrames(manager, 12));
    CHECK(backend.InjectMediaTransportFailureForValidation() == voip::Error::ok);
    CHECK(manager.StopMedia() == voip::Error::ok);
    CHECK(manager.Shutdown() == voip::Error::ok);
    CHECK(!backend.HasLiveResources());
    printk("[Phase 6] lifecycle %u PCMU/PCMA headless media: %s\n", number,
           failures == 0 ? "PASSED" : "FAILED");
}

} // namespace

int main() {
    printk("VoIP integration Phase 6 headless G.711 RTP validation\n");
    for (unsigned lifecycle = 1; lifecycle <= 3; ++lifecycle) Lifecycle(lifecycle);
    if (failures == 0)
        printk("VOIP INTEGRATION PHASE 6 RESULT: PASSED (3 media lifecycles)\n");
    else
        printk("VOIP INTEGRATION PHASE 6 RESULT: FAILED (%d checks)\n", failures);
    return failures == 0 ? 0 : 1;
}
