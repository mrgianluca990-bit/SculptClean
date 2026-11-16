#!/bin/bash
# Sculpt Clean's isolated, optional, offline separation runtime.
# No Homebrew, administrator rights or system Python modifications are required.
set -euo pipefail
script_dir="$(cd -- "$(dirname -- "$0")" && pwd)"
if [ "$(uname -s)" = "Darwin" ]; then
    default_root="$HOME/Library/Application Support/SculptClean/stems"
else
    default_root="$HOME/.local/share/SculptClean/stems"
fi
install_root="${1:-$default_root}"
cancel_file="${2:-}"
active_pid=''
creating_venv=0
mkdir -p "$install_root"
install_root="$(cd -- "$install_root" && pwd)"
if ! mkdir "$install_root/.setup-lock" 2>/dev/null; then
    echo 'Setup is already running, or a previous setup was interrupted (.setup-lock).' >&2
    exit 1
fi
stage_dir=''
cleanup() {
    rmdir "$install_root/.setup-lock" 2>/dev/null || true
    if [ -n "$stage_dir" ] && [ -d "$stage_dir" ]; then rm -rf -- "$stage_dir"; fi
    # This path did not exist before this invocation; remove only an unfinished
    # venv created by this invocation so cancellation can be retried cleanly.
    if [ "$creating_venv" -eq 1 ]; then rm -rf -- "$install_root/venv"; fi
}
interrupted() {
    if [ -n "$active_pid" ]; then kill "$active_pid" 2>/dev/null || true; wait "$active_pid" 2>/dev/null || true; fi
    exit 130
}
trap cleanup EXIT
trap interrupted INT TERM
run_checked() {
    if [ -n "$cancel_file" ] && [ -e "$cancel_file" ]; then echo 'Cancelled' >&2; return 130; fi
    "$@" &
    active_pid=$!
    while kill -0 "$active_pid" 2>/dev/null; do
        if [ -n "$cancel_file" ] && [ -e "$cancel_file" ]; then
            kill "$active_pid" 2>/dev/null || true
            wait "$active_pid" 2>/dev/null || true
            active_pid=''
            echo 'Cancelled' >&2
            return 130
        fi
        sleep 0.25
    done
    local result=0
    wait "$active_pid" || result=$?
    active_pid=''
    return "$result"
}

valid_python() {
    "$1" -c 'import sys,venv,ssl; assert (3,10) <= sys.version_info[:2] <= (3,12)' >/dev/null 2>&1
}
python_bin=''
for candidate in "${SCULPT_PYTHON:-}" "$install_root/python/bin/python3" python3.11 python3.12 python3.10 python3 /opt/homebrew/bin/python3 /usr/local/bin/python3; do
    if [ -n "$candidate" ] && command -v "$candidate" >/dev/null 2>&1 && valid_python "$candidate"; then
        python_bin="$(command -v "$candidate")"
        break
    fi
done

if [ -z "$python_bin" ]; then
    if [ "$(uname -s)" != 'Darwin' ]; then
        echo 'Python 3.10–3.12 is required on Linux. macOS includes an automatic private Python bootstrap.' >&2
        exit 1
    fi
    case "$(uname -m)" in
        arm64) python_arch='aarch64'; python_sha='566c5e266f2c933d0c0b213a75496bc6a090e493097802f809dbe21c75cd5d13' ;;
        x86_64) python_arch='x86_64'; python_sha='8ecd267281fb5b2464ddcd2de79622cfa7aff42e929b17989da2721ba39d4a5e' ;;
        *) echo 'Unsupported macOS processor' >&2; exit 1 ;;
    esac
    if [ -e "$install_root/python" ]; then
        echo 'Existing private Python is incompatible; choose another installation directory.' >&2
        exit 1
    fi
    stage_dir="$(mktemp -d "$install_root/.python-setup-XXXXXX")"
    python_url="https://github.com/astral-sh/python-build-standalone/releases/download/20241206/cpython-3.11.11+20241206-${python_arch}-apple-darwin-install_only.tar.gz"
    echo 'Downloading a private Python 3.11 runtime…' >&2
    run_checked /usr/bin/curl --fail --location --retry 3 --proto '=https' --tlsv1.2 "$python_url" -o "$stage_dir/python.tar.gz"
    actual_sha="$(/usr/bin/shasum -a 256 "$stage_dir/python.tar.gz" | /usr/bin/awk '{print $1}')"
    if [ "$actual_sha" != "$python_sha" ]; then
        echo 'Private Python checksum does not match; installation stopped.' >&2
        exit 1
    fi
    run_checked /usr/bin/tar -xzf "$stage_dir/python.tar.gz" -C "$stage_dir"
    mv "$stage_dir/python" "$install_root/python"
    python_bin="$install_root/python/bin/python3"
    valid_python "$python_bin"
fi

if [ ! -e "$install_root/venv" ]; then
    creating_venv=1
    run_checked "$python_bin" -m venv "$install_root/venv"
    creating_venv=0
fi
runtime_python="$install_root/venv/bin/python3"
if ! valid_python "$runtime_python"; then
    echo 'Existing private virtual environment is incompatible.' >&2
    exit 1
fi
echo 'Installing verified, pinned CPU separation dependencies…' >&2
if [ "$(uname -s)" = 'Linux' ]; then
    # CPU wheels are separate from PyPI's CUDA-linked Linux distribution.
    "$runtime_python" - "$script_dir/stems-linux-wheels.json" "$install_root/cpu-wheels.txt" <<'PY'
import json,sys,pathlib
manifest=json.loads(pathlib.Path(sys.argv[1]).read_text())
tag='cp'+str(sys.version_info.major)+str(sys.version_info.minor)
lines=[]
for name in ('torch','torchaudio'):
    entry=manifest[name][tag]
    lines.append(entry['url']+' --hash=sha256:'+entry['sha256'])
pathlib.Path(sys.argv[2]).write_text('\n'.join(lines)+'\n')
PY
    run_checked "$runtime_python" -m pip install --disable-pip-version-check --no-input --no-deps --require-hashes -r "$install_root/cpu-wheels.txt" >&2
fi
run_checked "$runtime_python" -m pip install --disable-pip-version-check --no-input --only-binary=:all: --require-hashes --no-deps -r "$script_dir/requirements-stems.txt" >&2
run_checked "$runtime_python" -m pip check >&2
run_checked "$runtime_python" "$script_dir/sculpt-stems.py" download-models --model-dir "$install_root/models" >&2
run_checked "$runtime_python" "$script_dir/sculpt-stems.py" status --model-dir "$install_root/models" --require-ready
