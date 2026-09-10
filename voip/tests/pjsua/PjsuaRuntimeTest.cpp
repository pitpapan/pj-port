#include "FakePjsuaApi.hpp"
#include "../../src/pjsua/PjsuaRuntime.hpp"
#include "../../src/pjsua/PjsuaTransportManager.hpp"

#include <cassert>
#include <initializer_list>
#include <zephyr/sys/printk.h>

namespace voip::test {

void RunPjsuaRuntimeTests() {
    pjsua_callback callbacks{};
    const PcmFormat format{8000, 160, 1, SampleFormat::signed_16};
    // The fake carries full PJSUA account/config storage.  Reuse this static
    // fixture only after each lifecycle has destroyed its runtime/transport;
    // allocating several lifetime-disjoint fakes in the Zephyr main frame
    // exceeded the fixed main-stack budget.
    static FakePjsuaApi fake;
    fake.Reset();
    PjsuaRuntime runtime(fake.Api());
    PjsuaTransportManager transport(fake.Api());
    assert(runtime.CreateAndInitialize(callbacks, format) == Error::ok);
    assert(fake.UaConfig().max_calls == 7 && fake.UaConfig().thread_cnt == 0 &&
           fake.UaConfig().stun_srv_cnt == 0 && fake.UaConfig().enable_upnp == PJ_FALSE &&
           fake.UaConfig().use_srtp == PJMEDIA_SRTP_DISABLED);
    assert(fake.MediaConfig().thread_cnt == 0 && fake.MediaConfig().has_ioqueue == PJ_FALSE &&
           fake.MediaConfig().max_media_ports == 12 && fake.MediaConfig().clock_rate == 8000 &&
           fake.MediaConfig().channel_count == 1 && fake.MediaConfig().audio_frame_ptime == 20);
    assert(fake.LoggingConfig().msg_logging == PJ_FALSE);
    assert(transport.Initialize(SignalingTransportPolicy::tcp_plain) == Error::ok);
    assert(runtime.Start() == Error::ok);
    assert(runtime.Pump(17) == Error::ok);
    assert(transport.Id() == 4);
    assert(transport.Shutdown() == Error::ok);
    assert(transport.Shutdown() == Error::ok);
    assert(runtime.Destroy() == Error::ok);
    assert(runtime.Destroy() == Error::ok);
    assert(fake.SequenceEquals("arena,create,defaults,init,nosnd,tcp,start,pump,close,destroy,reset"));

    // A Zephyr CoreActor is a native thread.  PJLIB may lose its TLS entry
    // after pjsua_create(), so every later PJSUA call must restore it before
    // entering the library.
    fake.Reset();
    PjsuaRuntime registration_runtime(fake.Api());
    assert(registration_runtime.CreateAndInitialize(callbacks, format) == Error::ok);
    assert(registration_runtime.Start() == Error::ok);
    fake.SetThreadRegistered(false);
    assert(registration_runtime.Pump(17) == Error::ok);
    assert(fake.ThreadRegisterCount() == 1);
    assert(fake.PumpCount() == 1);
    assert(registration_runtime.Pump(17) == Error::ok);
    assert(fake.ThreadRegisterCount() == 1);
    fake.SetThreadRegistered(false);
    fake.FailThreadRegister(PJ_EUNKNOWN);
    assert(registration_runtime.Pump(17) == Error::internal_failure);
    assert(fake.PumpCount() == 2);

    // A failed external-thread registration makes native destruction unsafe.
    // The runtime must retain its native/arena state so the actor can retry
    // destruction once PJLIB registration is available again.
    fake.SetThreadRegistered(false);
    assert(registration_runtime.Destroy() == Error::internal_failure);
    assert(fake.SequenceEquals(
        "arena,create,defaults,init,nosnd,start,pump,pump"));
    fake.FailThreadRegister(PJ_SUCCESS);
    assert(registration_runtime.Destroy() == Error::ok);
    assert(fake.SequenceEquals(
        "arena,create,defaults,init,nosnd,start,pump,pump,destroy,reset"));

    // A native destroy failure leaves PJSUA and its arena intact.  Resetting
    // either before a later successful destroy would invalidate callbacks.
    fake.Reset();
    assert(registration_runtime.CreateAndInitialize(callbacks, format) == Error::ok);
    assert(registration_runtime.Start() == Error::ok);
    fake.Fail(FakePjsuaApi::Stage::destroy);
    assert(registration_runtime.Destroy() == Error::internal_failure);
    assert(registration_runtime.Created());
    assert(fake.SequenceEquals(
        "arena,create,defaults,init,nosnd,start,destroy"));
    fake.ClearFailure();
    assert(registration_runtime.Destroy() == Error::ok);
    assert(fake.SequenceEquals(
        "arena,create,defaults,init,nosnd,start,destroy,destroy,reset"));

    // Native destruction succeeds once, but failed arena cleanup must be
    // retried before declaring the private lifecycle clean.
    fake.Reset();
    assert(registration_runtime.CreateAndInitialize(callbacks, format) == Error::ok);
    fake.FailArenaReset(PJ_EBUSY);
    const Error reset_failure = registration_runtime.Destroy();
    printk("PjsuaRuntimeTest arena reset failure retained: %s\n",
           reset_failure != Error::ok ? "yes" : "NO");
    assert(reset_failure != Error::ok);
    assert(!registration_runtime.Created());
    assert(registration_runtime.CreateAndInitialize(callbacks, format) == Error::invalid_state);
    fake.FailArenaReset(PJ_SUCCESS);
    assert(registration_runtime.Destroy() == Error::ok);
    assert(fake.TeardownSequenceEquals("destroy,reset,reset"));

    // Destroy clears the runtime-owned descriptor/handle state.  A later
    // lifecycle obtains a fresh PJLIB registration rather than retaining a
    // stale external-thread record.
    fake.Reset();
    assert(registration_runtime.CreateAndInitialize(callbacks, format) == Error::ok);
    assert(registration_runtime.Start() == Error::ok);
    fake.SetThreadRegistered(false);
    assert(registration_runtime.Pump(17) == Error::ok);
    assert(fake.ThreadRegisterCount() == 1);
    assert(registration_runtime.Destroy() == Error::ok);

    for (FakePjsuaApi::Stage stage : {FakePjsuaApi::Stage::arena,
         FakePjsuaApi::Stage::create, FakePjsuaApi::Stage::init,
         FakePjsuaApi::Stage::no_sound, FakePjsuaApi::Stage::transport,
         FakePjsuaApi::Stage::start}) {
        fake.Reset();
        fake.Fail(stage);
        PjsuaRuntime failing_runtime(fake.Api());
        PjsuaTransportManager failing_transport(fake.Api());
        if (stage == FakePjsuaApi::Stage::arena || stage == FakePjsuaApi::Stage::create ||
            stage == FakePjsuaApi::Stage::init || stage == FakePjsuaApi::Stage::no_sound) {
            assert(failing_runtime.CreateAndInitialize(callbacks, format) != Error::ok);
        } else {
            assert(failing_runtime.CreateAndInitialize(callbacks, format) == Error::ok);
        }
        if (stage == FakePjsuaApi::Stage::transport) {
            assert(failing_transport.Initialize(SignalingTransportPolicy::tcp_plain) != Error::ok);
        }
        if (stage == FakePjsuaApi::Stage::start) {
            assert(failing_transport.Initialize(SignalingTransportPolicy::tcp_plain) == Error::ok);
            assert(failing_runtime.Start() != Error::ok);
        }
        assert(failing_transport.Shutdown() == Error::ok);
        assert(failing_runtime.Destroy() == Error::ok);
        assert(failing_runtime.Destroy() == Error::ok);
        switch (stage) {
        case FakePjsuaApi::Stage::arena:
            assert(fake.SequenceEquals("arena")); break;
        case FakePjsuaApi::Stage::create:
            assert(fake.SequenceEquals("arena,create,reset")); break;
        case FakePjsuaApi::Stage::init:
            assert(fake.SequenceEquals("arena,create,defaults,init,destroy,reset")); break;
        case FakePjsuaApi::Stage::no_sound:
            assert(fake.SequenceEquals("arena,create,defaults,init,nosnd,destroy,reset")); break;
        case FakePjsuaApi::Stage::transport:
            assert(fake.SequenceEquals("arena,create,defaults,init,nosnd,tcp,destroy,reset")); break;
        case FakePjsuaApi::Stage::start:
            assert(fake.SequenceEquals("arena,create,defaults,init,nosnd,tcp,start,close,destroy,reset")); break;
        case FakePjsuaApi::Stage::destroy:
            assert(false); break;
        }
    }
    fake.Reset();
    PjsuaTransportManager tls_transport(fake.Api());
    assert(tls_transport.Initialize(SignalingTransportPolicy::tls) ==
           Error::unsupported_configuration);
    fake.Deactivate();
}

} // namespace voip::test
