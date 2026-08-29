#ifndef VOIP_VOIP_EVENTS_HPP
#define VOIP_VOIP_EVENTS_HPP

#include <voip/PcmAudio.hpp>

#include <cstdint>

namespace voip {

enum class EventType : std::uint8_t {
    agent_snapshot,
    incoming_call,
    call_state,
    operation_terminal,
    media_snapshot,
    resource_snapshot,
    service_stopped,
};

struct MediaSnapshot {
    bool active;
    bool send_enabled;
    bool receive_enabled;
    PcmFormat format;
};

struct Event {
    std::uint64_t sequence;
    EventType type;
    AgentHandle agent;
    CallHandle call;
    OperationId operation;
    CallState source_state;
    CallState destination_state;
    CallTransition transition;
    AgentSnapshot agent_snapshot;
    CallSnapshot call_snapshot;
    MediaSnapshot media_snapshot;
    ResourceSnapshot resource_snapshot;
    Status status;
};

} // namespace voip

#endif
