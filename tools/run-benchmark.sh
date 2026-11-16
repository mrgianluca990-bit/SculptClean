#!/usr/bin/env bash
set -euo pipefail
sculpt_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
sculpt_build="${sculpt_root}/build-tests"
sculpt_output="${1:-${sculpt_root}/docs/cpu-local-benchmark.json}"
mkdir -p "$sculpt_build" "$(dirname -- "$sculpt_output")"
sculpt_cxx="${CXX:-c++}"
sculpt_links=()
if [[ "$(uname -s)" == "Darwin" ]]; then sculpt_links+=(-framework Accelerate); fi
"$sculpt_cxx" -std=c++17 -O3 -DNDEBUG -Wall -Wextra -Wpedantic -Werror "${sculpt_root}/tools/benchmark.cpp" "${sculpt_links[@]}" -o "${sculpt_build}/benchmark"
"${sculpt_build}/benchmark" > "${sculpt_build}/benchmark-raw.json"
python3 - "${sculpt_build}/benchmark-raw.json" "$sculpt_output" <<'PY'
import datetime, json, pathlib, platform, subprocess, sys
data=json.loads(pathlib.Path(sys.argv[1]).read_text())
data['timestamp_utc']=datetime.datetime.now(datetime.timezone.utc).isoformat()
data['platform']=platform.platform()
data['architecture']=platform.machine()
try:
    if platform.system()=='Darwin':
        data['cpu_model']=subprocess.check_output(['sysctl','-n','machdep.cpu.brand_string'],text=True).strip()
    else:
        data['cpu_model']=next(line.split(':',1)[1].strip() for line in pathlib.Path('/proc/cpuinfo').read_text().splitlines() if line.startswith('model name'))
except (OSError,StopIteration,subprocess.SubprocessError):
    data['cpu_model']=platform.processor() or 'not reported'
pathlib.Path(sys.argv[2]).write_text(json.dumps(data,indent=2)+'\n')
print('CPU report: '+str(pathlib.Path(sys.argv[2]).resolve()))
for row in data['results']:
    if row['instances'] in (1,16):
        print(f"{row['mode']:8s} {row['instances']:2d} stereo, {row['block_size']:3d} samples: mean {row['mean_us']:.1f} us, p99 {row['p99_us']:.1f} us, max {row['max_us']:.1f} us, deadline misses {row['over_budget_callbacks']}/{row['callbacks']}")
PY
