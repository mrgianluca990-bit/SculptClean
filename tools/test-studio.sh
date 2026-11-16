#!/usr/bin/env bash
set -euo pipefail

# Native tests for the offline editor's processing code, without a GUI or JUCE
# dependency. Build the six genuine RNNoise translation units once, then link
# them into both the voice and the complete offline-tools tests.
sculpt_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
sculpt_build="${SCULPT_STUDIO_TEST_BUILD_DIR:-${sculpt_root}/build-studio-tests}"
sculpt_cc="${CC:-cc}"
sculpt_cxx="${CXX:-c++}"
mkdir -p "$sculpt_build"

sculpt_objects=()
for sculpt_unit in denoise rnn rnn_data pitch kiss_fft celt_lpc; do
  "$sculpt_cc" -std=gnu99 -O2 -DTRAINING=0 -DRNNOISE_BUILD=1 \
    -I"${sculpt_root}/ThirdParty/RNNoise/include" \
    -c "${sculpt_root}/ThirdParty/RNNoise/src/${sculpt_unit}.c" \
    -o "${sculpt_build}/${sculpt_unit}.o"
  sculpt_objects+=("${sculpt_build}/${sculpt_unit}.o")
done

sculpt_flags=(-std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror -pthread)
sculpt_links=(-lm)
if [[ "$(uname -s)" == "Darwin" ]]; then
  sculpt_links+=(-framework Accelerate)
fi
for sculpt_test in voice offline_repair offline_tools; do
  sculpt_command=("$sculpt_cxx" "${sculpt_flags[@]}" \
    "${sculpt_root}/Tests/${sculpt_test}_test.cpp")
  if [[ "$sculpt_test" != "offline_repair" ]]; then
    sculpt_command+=("${sculpt_objects[@]}")
  fi
  if [[ "$sculpt_test" == "voice" && "$(uname -s)" == "Linux" ]]; then
    sculpt_command+=(-DSCULPT_TEST_C_ALLOCATION=1 \
      -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc)
  fi
  echo "Studio test: ${sculpt_test}"
  # Keep one nonempty argv array: macOS Bash 3.2 with nounset rejects some
  # expansions of empty arrays that newer Bash versions accept.
  sculpt_command+=("${sculpt_links[@]}" -o "${sculpt_build}/${sculpt_test}_test")
  "${sculpt_command[@]}"
  "${sculpt_build}/${sculpt_test}_test"
done
