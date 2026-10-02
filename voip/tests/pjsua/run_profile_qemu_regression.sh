#!/bin/sh
# Full-log verdict gate for profiles which idle after reporting results.
set -eu
build_dir=${1:?build directory}
log_file=${2:?complete output log}
marker=${3:?exact expected verdict}
seconds=${4:-20}
set +e
timeout "${seconds}s" west build -d "$build_dir" -t run >"$log_file" 2>&1
status=$?
set -e
case "$status" in 0|124) ;; *) echo "Unexpected run exit: $status"; exit 1 ;; esac
rg -qF "$marker" "$log_file"
if rg -in '(MPU|USAGE|BUS|HARD|MEMMANAGE)[[:space:]]+FAULT|(Instruction|Data)[[:space:]]+Access[[:space:]]+Violation|EXC_RETURN|FATAL|ASSERTION|Kernel[[:space:]]+(panic|oops)|Halting system|abort\(|(^|[[:space:]])FAIL([[:space:]:]|$)' "$log_file"; then
    exit 1
fi
printf '%s (run exit %s, clean complete log)\n' "$marker" "$status"
