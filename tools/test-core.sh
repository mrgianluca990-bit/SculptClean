#!/usr/bin/env bash
set -euo pipefail
sculpt_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
sculpt_test_dir="${sculpt_root}/build-tests"
mkdir -p "$sculpt_test_dir"
sculpt_cxx="${CXX:-c++}"
sculpt_flags=(-std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror)
sculpt_links=()
if [[ "$(uname -s)" == "Darwin" ]]; then sculpt_links+=(-framework Accelerate); fi
for sculpt_test in spectral dereverb clean_engine; do
  "$sculpt_cxx" "${sculpt_flags[@]}" "${sculpt_root}/Tests/${sculpt_test}_test.cpp" "${sculpt_links[@]}" -o "${sculpt_test_dir}/${sculpt_test}_test"
  "${sculpt_test_dir}/${sculpt_test}_test"
done
