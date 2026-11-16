#!/usr/bin/env bash
set -euo pipefail
sculpt_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
sculpt_build="${sculpt_root}/build-voice-tests"
mkdir -p "$sculpt_build"
sculpt_cc="${CC:-cc}"
sculpt_cxx="${CXX:-c++}"
sculpt_objects=()
for sculpt_unit in denoise rnn rnn_data pitch kiss_fft celt_lpc; do
  "$sculpt_cc" -std=gnu99 -O2 -DTRAINING=0 -DRNNOISE_BUILD=1 -I"${sculpt_root}/ThirdParty/RNNoise/include" -c "${sculpt_root}/ThirdParty/RNNoise/src/${sculpt_unit}.c" -o "${sculpt_build}/${sculpt_unit}.o"
  sculpt_objects+=("${sculpt_build}/${sculpt_unit}.o")
done
sculpt_link_args=(-std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror "${sculpt_root}/Tests/voice_test.cpp" "${sculpt_objects[@]}" -pthread -lm -o "${sculpt_build}/voice_test")
if [[ "$(uname -s)" == "Linux" ]]; then sculpt_link_args+=(-DSCULPT_TEST_C_ALLOCATION=1 -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc); fi
"$sculpt_cxx" "${sculpt_link_args[@]}"
"${sculpt_build}/voice_test"
