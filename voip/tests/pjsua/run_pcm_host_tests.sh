#!/bin/sh
# Compile the actual checked-in PJMEDIA PCM helpers, not a header mock.
set -eu
out=${1:-/tmp/voip-pcm-host}
mkdir -p "$out"
common='-fsanitize=address,undefined -fno-omit-frame-pointer -ffunction-sections -fdata-sections -DPJ_HAS_LIMITS_H=1 -DPJ_IS_LITTLE_ENDIAN=1 -DPJ_IS_BIG_ENDIAN=0 -DPJMEDIA_HAS_VIDEO=0 -Ipjproject/pjlib/include -Ipjproject/pjmedia/include'
cc $common -c pjproject/pjmedia/src/pjmedia/port.c -o "$out/port.o"
cc $common -c pjproject/pjmedia/src/pjmedia/format.c -o "$out/format.o"
c++ $common -std=c++17 -Wall -Wextra -Werror -pthread -DVOIP_PCM_HOST_TEST -Ivoip/include \
  voip/tests/pjsua/PjsuaAudioPortTest.cpp voip/src/pjsua/PjsuaAudioPort.cpp \
  "$out/port.o" "$out/format.o" -Wl,--gc-sections -o "$out/pcm-test"
# LeakSanitizer cannot run under this sandbox's ptrace supervision. ASan
# buffer/lifetime checks and UBSan remain enabled; leak evidence is separate.
ASAN_OPTIONS=detect_leaks=0 "$out/pcm-test"
