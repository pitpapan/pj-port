namespace voip {
class Backend;
class EventHandler;
} // namespace voip
struct k_work_q;

#include <voip/VoipService.hpp>
#include <voip/VoipEvents.hpp>
#include <voip/PcmAudio.hpp>
#include <voip/VoipTypes.hpp>

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace {

template <typename T, typename = void>
struct has_username_member : std::false_type {};

template <typename T>
struct has_username_member<T, std::void_t<decltype(&T::username)>>
    : std::true_type {};

template <typename T, typename = void>
struct has_password_member : std::false_type {};

template <typename T>
struct has_password_member<T, std::void_t<decltype(&T::password)>>
    : std::true_type {};

template <typename T, typename = void>
struct has_legacy_dial : std::false_type {};

template <typename T>
struct has_legacy_dial<T, std::void_t<decltype(static_cast<voip::Error
    (T::*)(voip::AgentHandle, const voip::DialRequest &, voip::CallHandle *,
           voip::OperationId *) noexcept>(&T::Dial))>> : std::true_type {};

static_assert(!has_legacy_dial<voip::VoipService>::value,
              "legacy synchronous CallHandle Dial must remain absent");

template <typename T, typename = void>
struct has_queued_phase : std::false_type {};

template <typename T>
struct has_queued_phase<T, std::void_t<decltype(T::queued)>>
    : std::true_type {};

template <typename T, typename = void>
struct has_sip_phase : std::false_type {};

template <typename T>
struct has_sip_phase<T, std::void_t<decltype(T::sip)>> : std::true_type {};

template <typename T, typename = void>
struct has_media_phase : std::false_type {};

template <typename T>
struct has_media_phase<T, std::void_t<decltype(T::media)>>
    : std::true_type {};

template <typename T, typename = void>
struct has_teardown_phase : std::false_type {};

template <typename T>
struct has_teardown_phase<T, std::void_t<decltype(T::teardown)>>
    : std::true_type {};

#define DEFINE_PHASE_TRAIT(name)                                              \
    template <typename T, typename = void>                                    \
    struct has_##name##_phase : std::false_type {};                            \
    template <typename T>                                                      \
    struct has_##name##_phase<T, std::void_t<decltype(T::name)>>              \
        : std::true_type {}

DEFINE_PHASE_TRAIT(outgoing);
DEFINE_PHASE_TRAIT(incoming);
DEFINE_PHASE_TRAIT(early);
DEFINE_PHASE_TRAIT(held);
DEFINE_PHASE_TRAIT(disconnecting);
DEFINE_PHASE_TRAIT(disconnected);
DEFINE_PHASE_TRAIT(failed);
#undef DEFINE_PHASE_TRAIT

static_assert(std::is_trivially_copyable<voip::AgentHandle>::value,
              "agent handles must be safe to copy");
static_assert(std::is_trivially_copyable<voip::CallHandle>::value,
              "call handles must be safe to copy");
static_assert(std::is_trivially_copyable<voip::OperationId>::value,
              "operation ids must be safe to copy");
static_assert(std::is_trivially_copyable<voip::AgentSnapshot>::value,
              "agent snapshots must be safe to copy");
static_assert(std::is_trivially_copyable<voip::CallSnapshot>::value,
              "call snapshots must be safe to copy");
static_assert(std::is_trivially_copyable<voip::Event>::value,
              "events must be safe to copy");

static_assert(sizeof(((voip::SipAccountConfig *)nullptr)->identity_uri) ==
                  sizeof(const char *),
              "configuration strings are borrowed during Initialize");
static_assert(sizeof(((voip::AgentSnapshot *)nullptr)->identity_uri) ==
                  voip::max_uri_length + 1,
              "agent URI snapshots own the bounded URI");
static_assert(voip::max_password_length == 127,
              "credential copy storage retains the bounded password limit");
static_assert(sizeof(((voip::CallSnapshot *)nullptr)->remote_address) ==
                  voip::max_address_length + 1,
              "call snapshots own the bounded address");
static_assert(sizeof(((voip::Event *)nullptr)->status.reason) ==
                  voip::max_reason_length + 1,
              "events own the bounded diagnostic reason");

static_assert(static_cast<unsigned>(voip::CallState::idle) == 0,
              "call state ordering is part of the contract");
static_assert(static_cast<unsigned>(voip::CallState::initiated) == 1,
              "call state ordering is part of the contract");
static_assert(static_cast<unsigned>(voip::CallState::established) == 2,
              "call state ordering is part of the contract");
static_assert(static_cast<unsigned>(voip::CallState::hold) == 3,
              "call state ordering is part of the contract");
static_assert(static_cast<unsigned>(voip::CallState::terminated) == 4,
              "call state exposes only business states");
static_assert(static_cast<unsigned>(voip::CallState::terminated) + 1 == 5,
              "call state contains exactly five business states");
static_assert(static_cast<unsigned>(voip::HoldReason::none) == 0,
              "hold reason ordering is part of the contract");
static_assert(static_cast<unsigned>(voip::HoldReason::waiting) == 1,
              "hold reason ordering is part of the contract");
static_assert(static_cast<unsigned>(voip::HoldReason::media) == 2,
              "hold reason exposes waiting and media only");
static_assert(static_cast<unsigned>(voip::HoldReason::media) + 1 == 3,
              "hold reason contains exactly three reasons");

#define ASSERT_ENUM_VALUE(type, member, value) \
    static_assert(static_cast<unsigned>(voip::type::member) == value, \
                  "public enum value changed")
ASSERT_ENUM_VALUE(Error, ok, 0);
ASSERT_ENUM_VALUE(Error, invalid_argument, 1);
ASSERT_ENUM_VALUE(Error, invalid_handle, 2);
ASSERT_ENUM_VALUE(Error, invalid_state, 3);
ASSERT_ENUM_VALUE(Error, unsupported_configuration, 4);
ASSERT_ENUM_VALUE(Error, agent_unavailable, 5);
ASSERT_ENUM_VALUE(Error, busy, 6);
ASSERT_ENUM_VALUE(Error, queue_full, 7);
ASSERT_ENUM_VALUE(Error, resource_exhausted, 8);
ASSERT_ENUM_VALUE(Error, authentication_failed, 9);
ASSERT_ENUM_VALUE(Error, signaling_failed, 10);
ASSERT_ENUM_VALUE(Error, remote_rejected, 11);
ASSERT_ENUM_VALUE(Error, negotiation_failed, 12);
ASSERT_ENUM_VALUE(Error, media_failed, 13);
ASSERT_ENUM_VALUE(Error, cancelled, 14);
ASSERT_ENUM_VALUE(Error, timed_out, 15);
ASSERT_ENUM_VALUE(Error, shutting_down, 16);
ASSERT_ENUM_VALUE(Error, shutdown_timeout, 17);
ASSERT_ENUM_VALUE(Error, internal_failure, 18);
static_assert(static_cast<unsigned>(voip::Error::internal_failure) + 1 == 19,
              "error contains exactly nineteen product categories");
ASSERT_ENUM_VALUE(RegistrationState, disabled, 0);
ASSERT_ENUM_VALUE(RegistrationState, registering, 1);
ASSERT_ENUM_VALUE(RegistrationState, registered, 2);
ASSERT_ENUM_VALUE(RegistrationState, refreshing, 3);
ASSERT_ENUM_VALUE(RegistrationState, retry_wait, 4);
ASSERT_ENUM_VALUE(RegistrationState, unregistering, 5);
ASSERT_ENUM_VALUE(RegistrationState, authentication_failed, 6);
ASSERT_ENUM_VALUE(RegistrationState, transport_failed, 7);
static_assert(static_cast<unsigned>(voip::RegistrationState::transport_failed) +
                  1 == 8,
              "registration state contains exactly eight states");
ASSERT_ENUM_VALUE(CallTransition, initiation, 0);
ASSERT_ENUM_VALUE(CallTransition, acceptance, 1);
ASSERT_ENUM_VALUE(CallTransition, rejection, 2);
ASSERT_ENUM_VALUE(CallTransition, wait, 3);
ASSERT_ENUM_VALUE(CallTransition, timeout, 4);
ASSERT_ENUM_VALUE(CallTransition, hold, 5);
ASSERT_ENUM_VALUE(CallTransition, resume, 6);
ASSERT_ENUM_VALUE(CallTransition, finish, 7);
ASSERT_ENUM_VALUE(CallTransition, cleanup, 8);
static_assert(static_cast<unsigned>(voip::CallTransition::cleanup) + 1 == 9,
              "call transition contains exactly nine causes");
ASSERT_ENUM_VALUE(SignalingSecurity, none, 0);
ASSERT_ENUM_VALUE(SignalingSecurity, tls, 1);
ASSERT_ENUM_VALUE(MediaSecurity, none, 0);
ASSERT_ENUM_VALUE(MediaSecurity, srtp_sdes, 1);
static_assert(static_cast<unsigned>(voip::SignalingSecurity::tls) + 1 == 2,
              "signaling security contains exactly two policies");
static_assert(static_cast<unsigned>(voip::MediaSecurity::srtp_sdes) + 1 == 2,
              "media security contains exactly two policies");
ASSERT_ENUM_VALUE(EventType, agent_snapshot, 0);
ASSERT_ENUM_VALUE(EventType, incoming_call, 1);
ASSERT_ENUM_VALUE(EventType, call_state, 2);
ASSERT_ENUM_VALUE(EventType, operation_terminal, 3);
ASSERT_ENUM_VALUE(EventType, media_snapshot, 4);
ASSERT_ENUM_VALUE(EventType, resource_snapshot, 5);
ASSERT_ENUM_VALUE(EventType, service_stopped, 6);
static_assert(static_cast<unsigned>(voip::EventType::service_stopped) + 1 == 7,
              "event type contains exactly seven public event kinds");
#undef ASSERT_ENUM_VALUE

static_assert(!has_username_member<voip::AgentSnapshot>::value,
              "agent snapshots must not expose credentials");
static_assert(!has_password_member<voip::AgentSnapshot>::value,
              "agent snapshots must not expose credentials");
static_assert(!has_queued_phase<voip::CallState>::value,
              "queued is an internal phase");
static_assert(!has_sip_phase<voip::CallState>::value,
              "SIP phases are not public business states");
static_assert(!has_media_phase<voip::CallState>::value,
              "media phases are not public business states");
static_assert(!has_teardown_phase<voip::CallState>::value,
              "teardown phases are not public business states");
static_assert(!has_outgoing_phase<voip::CallState>::value,
              "outgoing is an internal phase");
static_assert(!has_incoming_phase<voip::CallState>::value,
              "incoming is an internal phase");
static_assert(!has_early_phase<voip::CallState>::value,
              "early is an internal phase");
static_assert(!has_held_phase<voip::CallState>::value,
              "held is an internal phase");
static_assert(!has_disconnecting_phase<voip::CallState>::value,
              "disconnecting is an internal phase");
static_assert(!has_disconnected_phase<voip::CallState>::value,
              "disconnected is an internal phase");
static_assert(!has_failed_phase<voip::CallState>::value,
              "failed is an internal phase");
template <typename T, typename = void>
struct has_legacy_initialize : std::false_type {};

template <typename T>
struct has_legacy_initialize<
    T, std::void_t<decltype(static_cast<voip::Error (T::*)(
        const voip::ServiceConfig &, voip::EventHandler *) noexcept>(
        &T::Initialize))>> : std::true_type {};

template <typename T, typename = void>
struct has_event_queue_member : std::false_type {};

template <typename T>
struct has_event_queue_member<T, std::void_t<decltype(&T::event_queue)>>
    : std::true_type {};

static_assert(std::is_default_constructible<voip::VoipService>::value,
              "service uses the fixed-storage default constructor");
static_assert(!std::is_constructible<voip::VoipService, voip::Backend &>::value,
              "the legacy backend constructor must remain removed");
static_assert(!has_legacy_initialize<voip::VoipService>::value,
              "the legacy callback Initialize overload must remain removed");
static_assert(!has_event_queue_member<voip::ServiceConfig>::value,
              "the service config must not expose a workqueue");

class Source final : public voip::PcmSource {
public:
    voip::PcmFormat Format() const noexcept override { return {8000, 160, 1,
                                                               voip::SampleFormat::signed_16}; }
    voip::Error Read(std::int16_t *, std::size_t,
                     std::uint64_t) noexcept override {
        return voip::Error::ok;
    }
};

class Sink final : public voip::PcmSink {
public:
    voip::PcmFormat Format() const noexcept override { return {8000, 160, 1,
                                                               voip::SampleFormat::signed_16}; }
    voip::Error Write(const std::int16_t *, std::size_t,
                      std::uint64_t) noexcept override {
        return voip::Error::ok;
    }
    void Flush() noexcept override {}
};

static_assert(!std::is_abstract<Source>::value && !std::is_abstract<Sink>::value,
              "PCM interfaces must expose the complete non-blocking contract");

} // namespace

int main() {
    voip::AgentConfig agent{
        {"sip:agent@example.test", "sip:example.test", "agent", "secret"},
        {nullptr, nullptr},
        true,
    };
    const voip::ServiceConfig config{
        &agent,
        1,
        1000,
        5000,
        {8000, 160, 1, voip::SampleFormat::signed_16},
        {voip::SignalingSecurity::none, voip::MediaSecurity::none},
    };
    (void)config;

    voip::VoipService service;
    voip::Event event{};
    voip::AgentHandle agent_handle{};
    voip::CallHandle call_handle{};
    voip::OperationId operation = 0;
    voip::DialRequest request{"sip:peer@example.test"};
    voip::AgentSnapshot agent_snapshot{};
    voip::CallSnapshot call_snapshot{};
    const voip::ResourceSnapshot resources = service.GetResourceSnapshot();

    (void)resources.available_commands;
    (void)resources.available_operations;
    (void)resources.available_events;
    (void)resources.available_fifo_entries;
    (void)resources.available_logical_calls;
    (void)resources.available_promoted_calls;
    (void)resources.available_media_bridges;
    (void)resources.active_calls;
    (void)resources.queued_calls;
    (void)resources.audio_callback_failures;
    (void)service.TryGetEvent(&event);
    (void)service.WaitForEvent(&event, 0);
    (void)service.GetAgentSnapshot(agent_handle, &agent_snapshot);
    (void)service.GetCallSnapshot(call_handle, &call_snapshot);
    (void)service.Dial(agent_handle, request, &operation);
    return 0;
}
