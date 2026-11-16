#!/usr/bin/env bash
set -euo pipefail

if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "Questo script compila Sculpt Clean Studio e VST3 per macOS. Eseguilo su un Mac."
  exit 1
fi
for sculpt_tool in cmake python3 cargo rustup; do
  if ! command -v "$sculpt_tool" >/dev/null 2>&1; then
    echo "Strumento di compilazione mancante: ${sculpt_tool}. Sono richiesti CMake 3.22+, Python 3 e Rust con rustup."
    exit 1
  fi
done
if ! xcode-select -p >/dev/null 2>&1; then
  echo "Sono richiesti Xcode Command Line Tools: xcode-select --install"
  exit 1
fi

sculpt_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
sculpt_build="${SCULPT_CLEAN_BUILD_DIR:-${sculpt_root}/build-macos}"
sculpt_dist="${SCULPT_CLEAN_DIST_DIR:-${sculpt_root}/dist}"
sculpt_jobs="${SCULPT_CLEAN_BUILD_JOBS:-4}"
configure_args=(
  -S "$sculpt_root"
  -B "$sculpt_build"
  -G "Unix Makefiles"
  -DCMAKE_BUILD_TYPE=Release
  -DSCULPT_BUILD_STUDIO=ON
  -DSCULPT_BUILD_STATE_TESTS=ON
  '-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64'
  -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0
)
if [[ -n "${SCULPT_JUCE_SOURCE_DIR:-}" ]]; then
  configure_args+=("-DSCULPT_JUCE_SOURCE_DIR=${SCULPT_JUCE_SOURCE_DIR}")
fi

cmake "${configure_args[@]}"
cmake --build "$sculpt_build" --config Release \
  --target SculptCleanStudio SculptClean_VST3 SculptCleanStateTests \
  --parallel "$sculpt_jobs"
mkdir -p "${sculpt_root}/docs"
ctest --test-dir "$sculpt_build" --build-config Release --output-on-failure --no-tests=error \
  --output-log "${sculpt_root}/docs/plugin-state-macos.log" -R '^plugin-state$'

sculpt_bundle="${sculpt_build}/SculptClean_artefacts/Release/VST3/Sculpt Clean.vst3"
sculpt_binary="${sculpt_bundle}/Contents/MacOS/Sculpt Clean"
sculpt_studio_bundle="${sculpt_build}/SculptCleanStudio_artefacts/Release/Sculpt Clean Studio.app"
sculpt_studio_binary="${sculpt_studio_bundle}/Contents/MacOS/Sculpt Clean Studio"
sculpt_helper="${sculpt_studio_bundle}/Contents/Resources/cathar"
for sculpt_executable in "$sculpt_binary" "$sculpt_studio_binary" "$sculpt_helper"; do
  if [[ ! -x "$sculpt_executable" ]]; then
    echo "La build non ha prodotto l'eseguibile atteso: $sculpt_executable"
    exit 1
  fi
  lipo "$sculpt_executable" -verify_arch arm64 x86_64
done
for sculpt_product in "$sculpt_bundle" "$sculpt_studio_bundle"; do
  plutil -lint "${sculpt_product}/Contents/Info.plist"
done

sculpt_notices="${sculpt_bundle}/Contents/Resources/ThirdParty"
mkdir -p "$sculpt_notices"
cp -R "${sculpt_root}/ThirdParty/." "$sculpt_notices/"

# CMake has copied Cathar, its licenses and the optional stems setup resources
# into Studio. Sign only after every resource has reached its final location.
codesign --force --sign - "$sculpt_helper"
codesign --force --deep --sign - "$sculpt_studio_bundle"
codesign --force --deep --sign - "$sculpt_bundle"
for sculpt_product in "$sculpt_helper" "$sculpt_studio_bundle" "$sculpt_bundle"; do
  codesign --verify --deep --strict --verbose=2 "$sculpt_product"
done

# Test the executable actually shipped inside the signed application bundle.
python3 "${sculpt_root}/tools/test-cathar.py" --binary "$sculpt_helper" \
  --report "${sculpt_root}/docs/cathar-macos-verification.json"
python3 - "$sculpt_studio_binary" "${sculpt_root}/docs/studio-macos-smoke.png" <<'PY' \
  2>&1 | tee "${sculpt_root}/docs/studio-macos-smoke.log"
import pathlib, subprocess, sys
executable, screenshot = sys.argv[1:]
subprocess.run([executable, '--self-test', screenshot], check=True, timeout=90)
if not pathlib.Path(screenshot).is_file():
    raise SystemExit('Studio self-test did not create its UI screenshot')
PY

mkdir -p "$sculpt_dist"
sculpt_package_work="$(mktemp -d "${sculpt_dist}/.sculpt-clean-package.XXXXXX")"
trap 'rm -rf -- "$sculpt_package_work"' EXIT
sculpt_package_root="${sculpt_package_work}/Sculpt-Clean"
mkdir -p "$sculpt_package_root"
ditto "$sculpt_studio_bundle" "${sculpt_package_root}/Sculpt Clean Studio.app"
ditto "$sculpt_bundle" "${sculpt_package_root}/Sculpt Clean.vst3"
cat > "${sculpt_package_root}/LEGGIMI.txt" <<'TXT'
SCULPT CLEAN 0.2.0 — macOS Apple Silicon e Intel

Sculpt Clean Studio.app: copia in Applicazioni per il restauro offline dei file.
Sculpt Clean.vst3: copia in ~/Library/Audio/Plug-Ins/VST3 per usarlo nella DAW.

Il motore Cathar e le licenze sono già contenuti nell'app Studio.
La separazione musicale UMX-HQ richiede il setup Stems dall'app: il primo
download installa il runtime e i modelli, poi l'elaborazione avviene localmente.

I bundle hanno firma locale ad hoc e non sono notarizzati Apple.
Questo archivio non esegue un'installazione automatica.
TXT
sculpt_archive="${sculpt_dist}/Sculpt-Clean-macOS.zip"
sculpt_temporary_archive="${sculpt_package_work}/Sculpt-Clean-macOS.zip"
ditto -c -k --sequesterRsrc --keepParent "$sculpt_package_root" "$sculpt_temporary_archive"
python3 - "$sculpt_temporary_archive" <<'PY'
import sys, zipfile
with zipfile.ZipFile(sys.argv[1]) as package:
    required = [
        'Sculpt-Clean/Sculpt Clean Studio.app/Contents/MacOS/Sculpt Clean Studio',
        'Sculpt-Clean/Sculpt Clean Studio.app/Contents/Resources/cathar',
        'Sculpt-Clean/Sculpt Clean.vst3/Contents/MacOS/Sculpt Clean',
        'Sculpt-Clean/LEGGIMI.txt',
    ]
    for path in required:
        if package.getinfo(path).file_size == 0:
            raise SystemExit('Empty packaged file: ' + path)
    damaged = package.testzip()
    if damaged:
        raise SystemExit('Invalid ZIP entry: ' + damaged)
PY
mv -f "$sculpt_temporary_archive" "$sculpt_archive"
python3 - "$sculpt_archive" <<'PY'
import hashlib, pathlib, sys
archive = pathlib.Path(sys.argv[1])
digest = hashlib.sha256()
with archive.open('rb') as source:
    for block in iter(lambda: source.read(1024 * 1024), b''):
        digest.update(block)
archive.with_name(archive.name + '.sha256').write_text(digest.hexdigest() + '  ' + archive.name + '\n')
PY
echo "Studio e VST3 universali Apple Silicon e Intel: $sculpt_archive"
echo "Architetture, firma locale, test audio, smoke test Studio e contenuto ZIP verificati."
