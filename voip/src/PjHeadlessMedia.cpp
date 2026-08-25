#include "PjHeadlessMedia.hpp"
#if defined(CONFIG_VOIP_PJ_SRTP_KEY_LIFECYCLE)
#include "PjSrtpKeyMaterial.hpp"
#endif

#include <pjmedia/event.h>
#include <pjmedia/g711.h>
#include <pjmedia/sdp.h>
#include <pjmedia/stream.h>
#if defined(CONFIG_VOIP_PJ_SRTP_MEDIA)
#include <pjmedia/transport_srtp.h>
#endif
#include <pjmedia/transport_udp.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include <new>

namespace voip {
namespace {

constexpr unsigned frame_samples = 160;
constexpr unsigned frame_bytes = frame_samples * sizeof(pj_int16_t);
constexpr unsigned sink_capacity = 8;

Error Translate(pj_status_t status) noexcept {
    return status == PJ_SUCCESS ? Error::ok : Error::media_failure;
}

} // namespace

struct PjHeadlessMedia::State {
    pj_pool_factory *factory;
    pjmedia_endpt *endpoint;
    pj_pool_t *event_pool;
    pjmedia_event_mgr *event_manager;
    pj_pool_t *pool;
    pjmedia_transport *first_transport;
    pjmedia_transport *second_transport;
    pjmedia_stream *first_stream;
    pjmedia_stream *second_stream;
    pjmedia_port *first_port;
    pjmedia_port *second_port;
    pj_thread_t *worker;
    atomic_t stop;
    atomic_t paused;
    atomic_t running;
    atomic_t worker_status;
    Codec codec;
    unsigned first_rtp_port;
    unsigned second_rtp_port;
    char remote_address[64];
    MediaStats stats;
    pj_int16_t sink[sink_capacity][frame_samples];
    unsigned sink_next;
    bool codec_initialized;
    bool srtp_started;
    bool sdes_signaling;
#if defined(CONFIG_VOIP_PJ_SRTP_KEY_LIFECYCLE)
    PjSrtpKeyMaterial local_key;
    PjSrtpKeyMaterial peer_key;
#endif

    static pj_status_t BoundSocket(pj_sock_t *socket, pj_sockaddr *address) noexcept {
        pj_sockaddr_in bind_address;
        pj_str_t loopback = pj_str(const_cast<char *>("127.0.0.1"));
        int length = sizeof(*address);
        *socket = PJ_INVALID_SOCKET;
        pj_status_t status = pj_sock_socket(pj_AF_INET(),
            pj_SOCK_DGRAM() | pj_SOCK_CLOEXEC(), 0, socket);
        if (status == PJ_SUCCESS)
            status = pj_sockaddr_in_init(&bind_address, &loopback, 0);
        if (status == PJ_SUCCESS)
            status = pj_sock_bind(*socket, &bind_address, sizeof(bind_address));
        if (status == PJ_SUCCESS)
            status = pj_sock_getsockname(*socket, address, &length);
        if (status != PJ_SUCCESS && *socket != PJ_INVALID_SOCKET) {
            pj_sock_close(*socket);
            *socket = PJ_INVALID_SOCKET;
        }
        return status;
    }

    pj_status_t CreateTransport(const char *name, pjmedia_transport **transport,
                                unsigned *rtp_port) noexcept {
        pjmedia_sock_info sockets;
        pj_bzero(&sockets, sizeof(sockets));
        sockets.rtp_sock = sockets.rtcp_sock = PJ_INVALID_SOCKET;
        pj_status_t status = BoundSocket(&sockets.rtp_sock,
                                          &sockets.rtp_addr_name);
        if (status == PJ_SUCCESS)
            status = BoundSocket(&sockets.rtcp_sock, &sockets.rtcp_addr_name);
        if (status == PJ_SUCCESS) {
            *rtp_port = pj_sockaddr_get_port(&sockets.rtp_addr_name);
            pjmedia_transport *udp = nullptr;
            status = pjmedia_transport_udp_attach(endpoint, name, &sockets,
                PJMEDIA_UDP_NO_SRC_ADDR_CHECKING, &udp);
            if (status == PJ_SUCCESS) {
                sockets.rtp_sock = sockets.rtcp_sock = PJ_INVALID_SOCKET;
#if defined(CONFIG_VOIP_PJ_SRTP_MEDIA)
                pjmedia_srtp_setting setting;
                pjmedia_srtp_setting_default(&setting);
                setting.use = PJMEDIA_SRTP_MANDATORY;
                setting.close_member_tp = PJ_TRUE;
                setting.crypto_count = 1;
                setting.crypto[0].name =
                    pj_str(const_cast<char *>("AES_CM_128_HMAC_SHA1_80"));
                setting.crypto[0].key.ptr = reinterpret_cast<char *>(
                    const_cast<std::uint8_t *>(local_key.Data()));
                setting.crypto[0].key.slen = PjSrtpKeyMaterial::size;
                setting.keying_count = sdes_signaling ? 1U : 0U;
                if (sdes_signaling)
                    setting.keying[0] = PJMEDIA_SRTP_KEYING_SDES;
                status = pjmedia_transport_srtp_create(endpoint, udp, &setting,
                                                        transport);
                if (status != PJ_SUCCESS) (void)pjmedia_transport_close(udp);
#else
                *transport = udp;
#endif
            }
        }
        if (sockets.rtp_sock != PJ_INVALID_SOCKET) pj_sock_close(sockets.rtp_sock);
        if (sockets.rtcp_sock != PJ_INVALID_SOCKET) pj_sock_close(sockets.rtcp_sock);
        return status;
    }

#if defined(CONFIG_VOIP_PJ_SRTP_MEDIA)
    static void InitCrypto(pjmedia_srtp_crypto *crypto,
                           const PjSrtpKeyMaterial &key) noexcept {
        pj_bzero(crypto, sizeof(*crypto));
        crypto->name = pj_str(const_cast<char *>("AES_CM_128_HMAC_SHA1_80"));
        crypto->key.ptr = reinterpret_cast<char *>(
            const_cast<std::uint8_t *>(key.Data()));
        crypto->key.slen = PjSrtpKeyMaterial::size;
    }

    pj_status_t StartSrtp() noexcept {
        pjmedia_srtp_crypto local;
        pjmedia_srtp_crypto peer;
        InitCrypto(&local, local_key);
        InitCrypto(&peer, peer_key);
        pj_status_t status = pjmedia_transport_srtp_start(first_transport,
                                                           &local, &peer);
        if (status == PJ_SUCCESS)
            status = pjmedia_transport_srtp_start(second_transport,
                                                   &peer, &local);
        srtp_started = status == PJ_SUCCESS;
        return status;
    }
#endif

    pj_status_t ParseSdp(unsigned port, const char *address,
                         pjmedia_sdp_session **session) noexcept {
        char text[320];
        const unsigned payload = codec == Codec::pcmu ? 0U : 8U;
        const char *name = codec == Codec::pcmu ? "PCMU" : "PCMA";
        const int length = pj_ansi_snprintf(text, sizeof(text),
            "v=0\r\no=voip 1 1 IN IP4 127.0.0.1\r\ns=headless\r\n"
            "c=IN IP4 %s\r\nt=0 0\r\n"
            "m=audio %u RTP/AVP %u\r\na=sendrecv\r\n"
            "a=rtpmap:%u %s/8000\r\n", address, port, payload, payload, name);
        if (length <= 0 || static_cast<unsigned>(length) >= sizeof(text))
            return PJ_ETOOBIG;
        char *copy = static_cast<char *>(pj_pool_alloc(pool, length + 1));
        if (copy == nullptr) return PJ_ENOMEM;
        pj_memcpy(copy, text, length + 1);
        return pjmedia_sdp_parse(pool, copy, length, session);
    }

    pj_status_t CreateStream(pjmedia_transport *transport, unsigned local_port,
                             unsigned remote_port, pjmedia_stream **stream,
                             pjmedia_port **port) noexcept {
        pjmedia_sdp_session *local = nullptr;
        pjmedia_sdp_session *remote = nullptr;
        pjmedia_stream_info info;
        pj_status_t status = ParseSdp(local_port, "127.0.0.1", &local);
        if (status == PJ_SUCCESS) status = ParseSdp(remote_port,
                                                    remote_address, &remote);
        if (status == PJ_SUCCESS)
            status = pjmedia_stream_info_from_sdp(&info, pool, endpoint,
                                                   local, remote, 0);
        if (status == PJ_SUCCESS)
#if defined(CONFIG_VOIP_PJ_SRTP_MEDIA)
            status = pjmedia_transport_media_start(
                pjmedia_transport_srtp_get_member(transport), pool, local,
                remote, 0);
#else
            status = pjmedia_transport_media_start(transport, pool, local,
                                                    remote, 0);
#endif
        if (status == PJ_SUCCESS)
            status = pjmedia_stream_create(endpoint, pool, &info, transport,
                                            nullptr, stream);
        if (status == PJ_SUCCESS) status = pjmedia_stream_get_port(*stream, port);
        if (status == PJ_SUCCESS) status = pjmedia_stream_start(*stream);
        return status;
    }

    static std::uint32_t Hash(std::uint32_t hash, const void *data,
                              std::size_t size) noexcept {
        const auto *bytes = static_cast<const pj_uint8_t *>(data);
        for (std::size_t i = 0; i < size; ++i) {
            hash ^= bytes[i];
            hash *= 16777619U;
        }
        return hash;
    }

    static int Worker(void *argument) noexcept {
        auto *self = static_cast<State *>(argument);
        unsigned frame_number = 0;
        while (atomic_get(&self->stop) == 0) {
            if (atomic_get(&self->paused) != 0) {
                pj_thread_sleep(5);
                continue;
            }
            pj_int16_t first_tx[frame_samples];
            pj_int16_t second_tx[frame_samples];
            pj_int16_t received[frame_samples];
            for (unsigned i = 0; i < frame_samples; ++i) {
                first_tx[i] = static_cast<pj_int16_t>(
                    ((frame_number * 97U + i * 31U) & 0x3fffU) - 8192);
                second_tx[i] = static_cast<pj_int16_t>(
                    ((frame_number * 53U + i * 19U) & 0x3fffU) - 8192);
            }
            pjmedia_frame frame;
            pj_bzero(&frame, sizeof(frame));
            frame.type = PJMEDIA_FRAME_TYPE_AUDIO;
            frame.buf = first_tx;
            frame.size = sizeof(first_tx);
            frame.timestamp.u64 = static_cast<pj_uint64_t>(frame_number) *
                                  frame_samples;
            pj_status_t status = pjmedia_port_put_frame(self->first_port, &frame);
            if (status == PJ_SUCCESS) {
                frame.buf = second_tx;
                status = pjmedia_port_put_frame(self->second_port, &frame);
            }
            if (status != PJ_SUCCESS) {
                atomic_set(&self->worker_status, status);
                break;
            }
            ++self->stats.generated_frames;
            pj_thread_sleep(20);
            pj_bzero(&frame, sizeof(frame));
            frame.buf = received;
            frame.size = sizeof(received);
            status = pjmedia_port_get_frame(self->first_port, &frame);
            if (status == PJ_SUCCESS && frame.type == PJMEDIA_FRAME_TYPE_AUDIO) {
                pj_memcpy(self->sink[self->sink_next], received, frame_bytes);
                self->sink_next = (self->sink_next + 1U) % sink_capacity;
                self->stats.sink_hash = Hash(self->stats.sink_hash,
                                              received, frame.size);
                ++self->stats.received_frames;
                if (self->stats.sink_peak_frames < sink_capacity)
                    ++self->stats.sink_peak_frames;
            }
            frame.buf = received;
            frame.size = sizeof(received);
            (void)pjmedia_port_get_frame(self->second_port, &frame);
            ++frame_number;
        }
        atomic_set(&self->running, 0);
        return 0;
    }
};

PjHeadlessMedia::PjHeadlessMedia(pj_pool_factory *factory,
                                 pjmedia_endpt *endpoint) noexcept
    : state_(new (std::nothrow) State{}) {
    if (state_ != nullptr) {
        state_->factory = factory;
        state_->endpoint = endpoint;
    }
}

Error PjHeadlessMedia::Initialize() noexcept {
    if (state_ == nullptr) return Error::internal_failure;
    state_->event_pool = pj_pool_create(state_->factory, "voip-media-events",
                                         4096, 4096, nullptr);
    if (state_->event_pool == nullptr) return Error::media_failure;
    pj_status_t status = pjmedia_event_mgr_create(state_->event_pool,
        PJMEDIA_EVENT_MGR_NO_THREAD, &state_->event_manager);
    if (status == PJ_SUCCESS) status = pjmedia_codec_g711_init(state_->endpoint);
    state_->codec_initialized = status == PJ_SUCCESS;
    if (status == PJ_SUCCESS) {
        state_->pool = pj_pool_create(state_->factory, "voip-media", 65536,
                                      32768, nullptr);
        if (state_->pool == nullptr) status = PJ_ENOMEM;
    }
    return Translate(status);
}

Error PjHeadlessMedia::Start(Codec codec) noexcept {
    Error result = Prepare(false);
    if (result != Error::ok) return result;
    return StartPrepared(codec, "127.0.0.1", state_->second_rtp_port);
}

Error PjHeadlessMedia::Prepare(bool sdes_signaling) noexcept {
    if (state_ == nullptr || state_->codec_initialized == false)
        return Error::not_initialized;
    if (state_->pool == nullptr) return Error::not_initialized;
    if (state_->first_stream != nullptr || state_->worker != nullptr)
        return Error::busy;
    pj_pool_reset(state_->pool);
    state_->stats = {};
    state_->stats.sink_hash = 2166136261U;
    state_->stats.sink_capacity_frames = sink_capacity;
    state_->sink_next = 0;
    atomic_set(&state_->stop, 0);
    atomic_set(&state_->paused, 0);
    atomic_set(&state_->worker_status, PJ_SUCCESS);
    state_->sdes_signaling = sdes_signaling;
#if defined(CONFIG_VOIP_PJ_SRTP_KEY_LIFECYCLE)
    if (!state_->local_key.Generate() || !state_->peer_key.Generate()) {
        state_->local_key.Clear();
        state_->peer_key.Clear();
        return Error::internal_failure;
    }
    if (pj_memcmp(state_->local_key.Data(), state_->peer_key.Data(),
                  PjSrtpKeyMaterial::size) == 0) {
        state_->local_key.Clear();
        state_->peer_key.Clear();
        return Error::internal_failure;
    }
#endif
    pj_status_t status = PJ_SUCCESS;
    if (state_->first_transport == nullptr)
        status = state_->CreateTransport("voip-media-a",
            &state_->first_transport, &state_->first_rtp_port);
    if (status == PJ_SUCCESS && state_->second_transport == nullptr)
        status = state_->CreateTransport("voip-media-b",
            &state_->second_transport, &state_->second_rtp_port);
#if defined(CONFIG_VOIP_PJ_SRTP_MEDIA)
    if (status == PJ_SUCCESS && !state_->sdes_signaling)
        status = state_->StartSrtp();
#endif
    if (status != PJ_SUCCESS) {
#if defined(CONFIG_VOIP_PJ_SRTP_KEY_LIFECYCLE)
        state_->local_key.Clear();
        state_->peer_key.Clear();
#endif
        (void)Stop();
        return Error::media_failure;
    }
    return Error::ok;
}

Error PjHeadlessMedia::EncodeSdesOffer(pj_pool_t *pool,
                                       pjmedia_sdp_session *offer) noexcept {
#if defined(CONFIG_VOIP_PJ_SRTP_SIGNALING)
    if (state_ == nullptr || !state_->sdes_signaling ||
        state_->first_transport == nullptr || pool == nullptr || offer == nullptr)
        return Error::invalid_state;
    pj_status_t status = pjmedia_transport_media_create(
        state_->first_transport, pool, 0, nullptr, 0);
    if (status == PJ_SUCCESS)
        status = pjmedia_transport_encode_sdp(state_->first_transport, pool,
                                               offer, nullptr, 0);
    return status == PJ_SUCCESS ? Error::ok : Error::negotiation_failure;
#else
    (void)pool; (void)offer;
    return Error::invalid_state;
#endif
}

Error PjHeadlessMedia::EncodeSdesAnswer(
    pj_pool_t *pool, pjmedia_sdp_session *answer,
    const pjmedia_sdp_session *offer) noexcept {
#if defined(CONFIG_VOIP_PJ_SRTP_SIGNALING)
    if (state_ == nullptr || !state_->sdes_signaling ||
        state_->first_transport == nullptr || pool == nullptr ||
        answer == nullptr || offer == nullptr)
        return Error::negotiation_failure;
    pj_status_t status = pjmedia_transport_media_create(
        state_->first_transport, pool, 0, offer, 0);
    if (status == PJ_SUCCESS)
        status = pjmedia_transport_encode_sdp(state_->first_transport, pool,
                                               answer, offer, 0);
    return status == PJ_SUCCESS ? Error::ok : Error::negotiation_failure;
#else
    (void)pool; (void)answer; (void)offer;
    return Error::invalid_state;
#endif
}

Error PjHeadlessMedia::ActivateSdes(
    pj_pool_t *pool, const pjmedia_sdp_session *local,
    const pjmedia_sdp_session *remote) noexcept {
#if defined(CONFIG_VOIP_PJ_SRTP_SIGNALING)
    if (state_ == nullptr || !state_->sdes_signaling ||
        state_->first_transport == nullptr || state_->second_transport == nullptr)
        return Error::invalid_state;
    pj_status_t status = pjmedia_transport_media_start(
        state_->first_transport, pool, local, remote, 0);
    pjmedia_transport_info info;
    pjmedia_transport_info_init(&info);
    if (status == PJ_SUCCESS)
        status = pjmedia_transport_get_info(state_->first_transport, &info);
    auto *srtp = status == PJ_SUCCESS
        ? static_cast<pjmedia_srtp_info *>(pjmedia_transport_info_get_spc_info(
              &info, PJMEDIA_TRANSPORT_TYPE_SRTP))
        : nullptr;
    if (status == PJ_SUCCESS && (srtp == nullptr || !srtp->active))
        status = PJMEDIA_SRTP_ESDPINTRANSPORT;
    if (status == PJ_SUCCESS)
        status = pjmedia_transport_srtp_start(state_->second_transport,
                                               &srtp->rx_policy,
                                               &srtp->tx_policy);
    state_->srtp_started = status == PJ_SUCCESS;
    return status == PJ_SUCCESS ? Error::ok : Error::negotiation_failure;
#else
    (void)pool; (void)local; (void)remote;
    return Error::invalid_state;
#endif
}

unsigned PjHeadlessMedia::LocalRtpPort() const noexcept {
    return state_ == nullptr ? 0U : state_->first_rtp_port;
}

unsigned PjHeadlessMedia::PeerRtpPortForValidation() const noexcept {
    return state_ == nullptr ? 0U : state_->second_rtp_port;
}

Error PjHeadlessMedia::StartPrepared(Codec codec, const char *remote_address,
                                     unsigned remote_rtp_port) noexcept {
    if (state_ == nullptr || state_->pool == nullptr ||
        state_->first_transport == nullptr || state_->second_transport == nullptr)
        return Error::invalid_state;
    if (state_->first_stream != nullptr || state_->worker != nullptr)
        return Error::busy;
    if (remote_address == nullptr || remote_address[0] == '\0' ||
        remote_rtp_port == 0) {
        (void)Stop();
        return Error::negotiation_failure;
    }
    state_->codec = codec;
    pj_ansi_strncpy(state_->remote_address, remote_address,
                    sizeof(state_->remote_address) - 1);
    pj_status_t status = PJ_SUCCESS;
    if (status == PJ_SUCCESS)
        status = state_->CreateStream(state_->first_transport,
            state_->first_rtp_port, remote_rtp_port,
            &state_->first_stream, &state_->first_port);
    if (status == PJ_SUCCESS)
        status = state_->CreateStream(state_->second_transport,
            state_->second_rtp_port, state_->first_rtp_port,
            &state_->second_stream, &state_->second_port);
    if (status == PJ_SUCCESS) {
        atomic_set(&state_->running, 1);
        status = pj_thread_create(state_->pool, "voip-media", &State::Worker,
            state_, PJ_THREAD_DEFAULT_STACK_SIZE, 0, &state_->worker);
    }
    if (status != PJ_SUCCESS) {
        (void)Stop();
        return Error::media_failure;
    }
    return Error::ok;
}

Error PjHeadlessMedia::SetPaused(bool paused) noexcept {
    if (state_ == nullptr || state_->first_stream == nullptr)
        return Error::invalid_state;
    if ((atomic_get(&state_->paused) != 0) == paused) return Error::invalid_state;
    const pjmedia_dir direction = PJMEDIA_DIR_ENCODING_DECODING;
    pj_status_t status = paused
        ? pjmedia_stream_pause(state_->first_stream, direction)
        : pjmedia_stream_resume(state_->first_stream, direction);
    if (status == PJ_SUCCESS)
        status = paused
            ? pjmedia_stream_pause(state_->second_stream, direction)
            : pjmedia_stream_resume(state_->second_stream, direction);
    if (status == PJ_SUCCESS) atomic_set(&state_->paused, paused ? 1 : 0);
    return Translate(status);
}

MediaStats PjHeadlessMedia::Stats() const noexcept {
    if (state_ == nullptr) return MediaStats{};
    MediaStats result = state_->stats;
    if (state_->first_stream != nullptr) {
        pjmedia_rtcp_stat stat;
        pjmedia_jb_state jitter;
        if (pjmedia_stream_get_stat(state_->first_stream, &stat) == PJ_SUCCESS) {
            result.rtp_packets_sent = stat.tx.pkt;
            result.rtp_packets_received = stat.rx.pkt;
        }
        if (pjmedia_stream_get_stat_jbuf(state_->first_stream, &jitter) == PJ_SUCCESS)
            result.jitter_buffer_frames = jitter.size;
    }
    return result;
}

bool PjHeadlessMedia::Running() const noexcept {
    return state_ != nullptr && state_->first_stream != nullptr &&
           atomic_get(&state_->running) != 0;
}

bool PjHeadlessMedia::SrtpKeysActiveForValidation() const noexcept {
#if defined(CONFIG_VOIP_PJ_SRTP_KEY_LIFECYCLE)
    return state_ != nullptr && state_->local_key.Active() &&
           state_->peer_key.Active() &&
           pj_memcmp(state_->local_key.Data(), state_->peer_key.Data(),
                     PjSrtpKeyMaterial::size) != 0;
#else
    return false;
#endif
}

bool PjHeadlessMedia::SrtpKeysClearedForValidation() const noexcept {
#if defined(CONFIG_VOIP_PJ_SRTP_KEY_LIFECYCLE)
    return state_ != nullptr && state_->local_key.ClearedForValidation() &&
           state_->peer_key.ClearedForValidation();
#else
    return true;
#endif
}

bool PjHeadlessMedia::SrtpTransportActiveForValidation() const noexcept {
#if defined(CONFIG_VOIP_PJ_SRTP_MEDIA)
    return state_ != nullptr && state_->srtp_started &&
           state_->first_transport != nullptr &&
           state_->second_transport != nullptr;
#else
    return false;
#endif
}

Error PjHeadlessMedia::InjectTransportFailure() noexcept {
    if (state_ == nullptr || state_->first_transport == nullptr)
        return Error::invalid_state;
    atomic_set(&state_->stop, 1);
    if (state_->worker != nullptr) {
        pj_thread_join(state_->worker);
        pj_thread_destroy(state_->worker);
        state_->worker = nullptr;
    }
    pj_status_t status = PJ_SUCCESS;
    if (state_->first_stream != nullptr) {
        status = pjmedia_stream_destroy(state_->first_stream);
        state_->first_stream = nullptr;
        state_->first_port = nullptr;
    }
    if (status == PJ_SUCCESS)
        status = pjmedia_transport_media_stop(state_->first_transport);
    if (status == PJ_SUCCESS)
        status = pjmedia_transport_close(state_->first_transport);
    if (status == PJ_SUCCESS) state_->first_transport = nullptr;
    return Translate(status);
}

Error PjHeadlessMedia::Stop() noexcept {
    if (state_ == nullptr || (state_->first_transport == nullptr &&
        state_->second_transport == nullptr && state_->first_stream == nullptr &&
        state_->worker == nullptr)) return Error::invalid_state;
    Error call_result = StopCall();
    pj_status_t result = call_result == Error::ok ? PJ_SUCCESS : PJ_EUNKNOWN;
    if (state_->first_transport != nullptr) {
        const pj_status_t status = pjmedia_transport_close(state_->first_transport);
        if (result == PJ_SUCCESS) result = status;
        state_->first_transport = nullptr;
    }
    if (state_->second_transport != nullptr) {
        const pj_status_t status = pjmedia_transport_close(state_->second_transport);
        if (result == PJ_SUCCESS) result = status;
        state_->second_transport = nullptr;
    }
    state_->first_rtp_port = 0;
    state_->second_rtp_port = 0;
    return Translate(result);
}

Error PjHeadlessMedia::StopCall() noexcept {
    if (state_ == nullptr || (state_->first_transport == nullptr &&
        state_->second_transport == nullptr && state_->first_stream == nullptr &&
        state_->worker == nullptr)) return Error::invalid_state;
    atomic_set(&state_->stop, 1);
    if (state_->worker != nullptr) {
        pj_thread_join(state_->worker);
        pj_thread_destroy(state_->worker);
        state_->worker = nullptr;
    }
    pj_status_t result = PJ_SUCCESS;
    if (state_->first_stream != nullptr) {
        result = pjmedia_stream_destroy(state_->first_stream);
        state_->first_stream = nullptr;
        state_->first_port = nullptr;
    }
    if (state_->second_stream != nullptr) {
        const pj_status_t status = pjmedia_stream_destroy(state_->second_stream);
        if (result == PJ_SUCCESS) result = status;
        state_->second_stream = nullptr;
        state_->second_port = nullptr;
    }
    if (state_->first_transport != nullptr && state_->first_stream == nullptr) {
        (void)pjmedia_transport_media_stop(state_->first_transport);
    }
    if (state_->second_transport != nullptr && state_->second_stream == nullptr) {
        (void)pjmedia_transport_media_stop(state_->second_transport);
    }
#if defined(CONFIG_VOIP_PJ_SRTP_KEY_LIFECYCLE)
    state_->local_key.Clear();
    state_->peer_key.Clear();
#endif
    state_->srtp_started = false;
    pj_pool_reset(state_->pool);
    atomic_set(&state_->running, 0);
    return Translate(result);
}

void PjHeadlessMedia::Destroy() noexcept {
    if (state_ == nullptr) return;
    if (state_->first_transport != nullptr || state_->second_transport != nullptr ||
        state_->first_stream != nullptr || state_->worker != nullptr)
        (void)Stop();
    if (state_->pool != nullptr) {
        pj_pool_release(state_->pool);
        state_->pool = nullptr;
    }
    if (state_->codec_initialized) {
        (void)pjmedia_codec_g711_deinit();
        state_->codec_initialized = false;
    }
    if (state_->event_manager != nullptr) {
        pjmedia_event_mgr_destroy(state_->event_manager);
        state_->event_manager = nullptr;
    }
    if (state_->event_pool != nullptr) {
        pj_pool_release(state_->event_pool);
        state_->event_pool = nullptr;
    }
    delete state_;
    state_ = nullptr;
}

} // namespace voip
