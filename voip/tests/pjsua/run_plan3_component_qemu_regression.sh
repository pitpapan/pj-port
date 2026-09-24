#!/bin/sh
# Usage: run from the Zephyr workspace root with a configured component build
# directory.  A successful component test deliberately idles after its verdict,
# so timeout(1) returning 124 is the expected harness result.
set -eu

build_dir=${1:?configured component build directory required}
log_file=${2:?output log file required}

set +e
timeout 18s west build -d "$build_dir" -t run >"$log_file" 2>&1
run_status=$?
set -e

test "$run_status" -eq 124
rg -qF 'PJSUA PLAN 3 COMPONENT RESULT: PASSED' "$log_file"
! rg -iq '(MPU|USAGE|BUS|HARD|MEMMANAGE)[[:space:]]+FAULT|(Instruction|Data)[[:space:]]+Access[[:space:]]+Violation|Illegal load of EXC_RETURN into PC|ZEPHYR FATAL ERROR|ASSERTION FAIL|Kernel[[:space:]]+(panic|oops)|Halting system|abort\(\)' "$log_file"
