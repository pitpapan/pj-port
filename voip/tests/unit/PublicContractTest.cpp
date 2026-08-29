#include <voip/VoipService.hpp>

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace {

static_assert(std::is_trivially_copyable<voip::AgentHandle>::value,
              "agent handles must be safe to copy");
static_assert(std::is_trivially_copyable<voip::CallHandle>::value,
              "call handles must be safe to copy");
static_assert(std::is_trivially_copyable<voip::OperationId>::value,
              "operation ids must be safe to copy");

static_assert(sizeof(((voip::SipAccountConfig *)nullptr)->identity_uri) ==
                  sizeof(const char *),
              "configuration strings are borrowed during Initialize");
static_assert(sizeof(((voip::AgentSnapshot *)nullptr)->identity_uri) ==
                  voip::max_uri_length + 1,
              "agent URI snapshots own the bounded URI");
static_assert(sizeof(((voip::AgentSnapshot *)nullptr)->username) ==
                  voip::max_username_length + 1,
              "agent snapshots own the bounded username");
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
static_assert(static_cast<unsigned>(voip::CallState::terminated) == 4,
              "call state exposes only business states");
static_assert(static_cast<unsigned>(voip::HoldReason::none) == 0,
              "hold reason ordering is part of the contract");
static_assert(static_cast<unsigned>(voip::HoldReason::media) == 2,
              "hold reason exposes waiting and media only");

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

    (void)service.TryGetEvent(&event);
    (void)service.WaitForEvent(&event, 0);
    (void)service.GetAgentSnapshot(agent_handle, &agent_snapshot);
    (void)service.GetCallSnapshot(call_handle, &call_snapshot);
    (void)service.Dial(agent_handle, request, &call_handle, &operation);
    return 0;
}
