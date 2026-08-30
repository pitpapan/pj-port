#include "FakePjsuaApi.hpp"
#include "../../src/pjsua/PjsuaRuntime.hpp"
#include "../../src/pjsua/PjsuaTransportManager.hpp"

#include <cassert>
#include <initializer_list>

namespace voip::test {

void RunPjsuaRuntimeTests() {
    FakePjsuaApi fake;
    PjsuaRuntime runtime(fake.Api());
    PjsuaTransportManager transport(fake.Api());
    pjsua_callback callbacks{};
    const PcmFormat format{8000, 160, 1, SampleFormat::signed_16};
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
    assert(transport.Shutdown() == Error::ok);
    assert(runtime.Destroy() == Error::ok);
    assert(fake.SequenceEquals("arena,create,defaults,init,nosnd,tcp,start,pump,close,destroy,reset"));

    for (FakePjsuaApi::Stage stage : {FakePjsuaApi::Stage::arena,
         FakePjsuaApi::Stage::create, FakePjsuaApi::Stage::init,
         FakePjsuaApi::Stage::no_sound, FakePjsuaApi::Stage::transport,
         FakePjsuaApi::Stage::start}) {
        FakePjsuaApi failing;
        failing.Fail(stage);
        PjsuaRuntime failing_runtime(failing.Api());
        PjsuaTransportManager failing_transport(failing.Api());
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
    }
    FakePjsuaApi tls_fake;
    PjsuaTransportManager tls_transport(tls_fake.Api());
    assert(tls_transport.Initialize(SignalingTransportPolicy::tls) ==
           Error::unsupported_configuration);
}

} // namespace voip::test
