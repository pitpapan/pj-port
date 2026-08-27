VoIP Multi-Agent Design Concept
Purpose

The VoIP subsystem shall support a configurable set of Agent Endpoints, each representing a logical communication endpoint with its own SIP identity and associated audio devices.

The system is designed to support many configured Agent Endpoints while limiting the number of simultaneous active calls in order to satisfy platform, resource, and operational constraints.

Agent Endpoint Definition

An Agent Endpoint is a logical entity responsible for handling voice communications.

An Agent Endpoint is associated with:

A SIP account/identity.
Registration state.
Assigned audio input device.
Assigned audio output device.

Example:

Agent Endpoint A
 ├── SIP Account: sip:agentA@domain.com
 ├── Microphone A
 └── Speaker A

Agent Endpoint B
 ├── SIP Account: sip:agentB@domain.com
 ├── Microphone B
 └── Speaker B


The Agent Endpoint is not the audio device itself.

Audio devices are resources assigned to the Agent Endpoint and may be changed without affecting the Agent Endpoint identity.

Configurable Agent Endpoints

The system shall support a configurable number of Agent Endpoints.

Examples:

1 Agent Endpoint
5 Agent Endpoints
10 Agent Endpoints
N Agent Endpoints


The implementation shall not assume a fixed number of configured Agents.

Each Agent Endpoint shall maintain its own SIP registration lifecycle independently.

SIP Identity Model

Each Agent Endpoint shall be associated with a unique SIP identity.

Example:

Agent Endpoint A
    sip:agentA@domain.com

Agent Endpoint B
    sip:agentB@domain.com

Agent Endpoint C
    sip:agentC@domain.com


Incoming calls may target a specific Agent Endpoint through its SIP identity.

Concurrent Call Requirement
Maximum Active Calls

The system shall enforce a global limit of:

Maximum Concurrent Active Calls = 2


An active call is any call occupying media and signaling resources, including:

Established
Connected
Held


or any other state classified as active by product requirements.

Example:

Agent Endpoint A -> Active Call #1
Agent Endpoint B -> Active Call #2


At this point:

Active Call Count = 2


and the active-call capacity is fully utilized.

Call Queue Requirement

When the maximum active-call limit has been reached, additional incoming calls shall not become active immediately.

Instead, they shall be placed into a waiting queue.

Example:

Agent Endpoint A -> Call #1 -> Active
Agent Endpoint B -> Call #2 -> Active

Agent Endpoint C -> Call #3 -> Queued
Agent Endpoint D -> Call #4 -> Queued
Agent Endpoint E -> Call #5 -> Queued


The queue may contain calls for any Agent Endpoint.

Queue Promotion

When an active call ends, capacity becomes available.

The next queued call shall be promoted to active status.

Example:

Before

Call #1 -> Active
Call #2 -> Active
Call #3 -> Queued

After Call #1 Ends

Call #2 -> Active
Call #3 -> Active


The queue-selection policy shall be configurable or explicitly defined (FIFO, priority-based, routing-based, etc.).

Audio Routing Requirement

Each active call shall use the audio devices associated with the Agent Endpoint handling that call.

Example:

Call for Agent Endpoint B

Microphone B
      ↓
 RTP/SRTP Media
      ↓
Speaker B


Media shall not be tied to a single system-wide audio path.

Instead, media shall be routed according to the Agent Endpoint associated with the call.

Functional Requirements

The solution shall support:

Configurable Agent Endpoints.
One SIP account per Agent Endpoint.
Independent SIP registration per Agent Endpoint.
Independent audio device assignment per Agent Endpoint.
Incoming and outgoing calls.
RTP/SRTP media transport.
Call hold and resume.
Maximum of two simultaneous active calls.
Queueing of additional calls.
Automatic promotion of queued calls when capacity becomes available.
Design Summary
Configurable Agent Endpoints
            +
One SIP Account per Agent Endpoint
            +
Assigned Audio Devices per Agent Endpoint
            +
Independent SIP Registration
            +
Global Active Call Limit = 2
            +
Additional Calls Queued
            +
Automatic Queue Promotion

Key Constraint
Number of Agent Endpoints: Configurable (N)

Maximum Simultaneous Active Calls: 2

Additional Calls: Queued Until Capacity Becomes Available


This design separates Agent Endpoint capacity from call-processing capacity, allowing many configured Agent Endpoints while strictly limiting the platform to two concurrent active calls at any given time.