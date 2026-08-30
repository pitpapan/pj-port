#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
out=$(mktemp -d /tmp/voip-plan2-host-XXXXXX)
cleanup=0
trap 'if [ "$cleanup" -eq 1 ]; then rm -rf "$out"; fi' EXIT

cxx="${CXX:-c++}"
flags="-std=c++17 -Wall -Wextra -Werror -pthread -I$root/include -I$root/src"

$cxx $flags -c "$root/tests/unit/PublicContractTest.cpp" -o "$out/PublicContractTest.o"
echo "PublicContractTest PASSED"

$cxx $flags "$root/tests/unit/HandlePoolTest.cpp" -o "$out/HandlePoolTest"
"$out/HandlePoolTest"

$cxx $flags "$root/tests/unit/AgentRegistryTest.cpp" "$root/src/core/AgentRegistry.cpp" -o "$out/AgentRegistryTest"
"$out/AgentRegistryTest"

$cxx $flags "$root/tests/unit/CallStateMachineTest.cpp" "$root/src/core/CallStateMachine.cpp" -o "$out/CallStateMachineTest"
"$out/CallStateMachineTest"

$cxx $flags "$root/tests/unit/VoipEventQueueTest.cpp" "$root/src/core/VoipEventQueue.cpp" -o "$out/VoipEventQueueTest"
"$out/VoipEventQueueTest"

$cxx $flags "$root/tests/unit/OperationMailboxTest.cpp" \
    "$root/src/core/AgentRegistry.cpp" "$root/src/core/CommandMailbox.cpp" \
    "$root/src/core/OperationTable.cpp" "$root/src/core/VoipEventQueue.cpp" \
    -o "$out/OperationMailboxTest"
"$out/OperationMailboxTest"

$cxx $flags "$root/tests/unit/CallSchedulerTest.cpp" \
    "$root/src/core/AgentRegistry.cpp" "$root/src/core/CallStateMachine.cpp" \
    "$root/src/core/CallScheduler.cpp" -o "$out/CallSchedulerTest"
"$out/CallSchedulerTest"

$cxx $flags "$root/tests/unit/VoipServiceCoreTest.cpp" \
    "$root/src/VoipService.cpp" "$root/src/core/AgentRegistry.cpp" \
    "$root/src/core/CallStateMachine.cpp" "$root/src/core/CallScheduler.cpp" \
    "$root/src/core/CommandMailbox.cpp" "$root/src/core/OperationTable.cpp" \
    "$root/src/core/VoipEventQueue.cpp" "$root/src/core/FakeRuntimeAdapter.cpp" \
    "$root/src/core/CoreActor.cpp" "$root/src/core/VoipRuntime.cpp" \
    "$root/src/core/VoipResourceGuard.cpp" -o "$out/VoipServiceCoreTest"
"$out/VoipServiceCoreTest"

$cxx $flags "$root/tests/unit/NoHeapAfterInitTest.cpp" \
    "$root/src/VoipService.cpp" "$root/src/core/AgentRegistry.cpp" \
    "$root/src/core/CallStateMachine.cpp" "$root/src/core/CallScheduler.cpp" \
    "$root/src/core/CommandMailbox.cpp" "$root/src/core/OperationTable.cpp" \
    "$root/src/core/VoipEventQueue.cpp" "$root/src/core/FakeRuntimeAdapter.cpp" \
    "$root/src/core/CoreActor.cpp" "$root/src/core/VoipRuntime.cpp" \
    "$root/src/core/VoipResourceGuard.cpp" -o "$out/NoHeapAfterInitTest"
"$out/NoHeapAfterInitTest"

cleanup=1
