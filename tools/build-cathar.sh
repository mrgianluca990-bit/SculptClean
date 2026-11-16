#!/usr/bin/env bash
set -euo pipefail

# Pinned, unchanged upstream source. This builds a separate offline helper;
# no Cathar code or worker thread is linked into the VST3 audio callback.
sculpt_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
sculpt_work="${SCULPT_CATHAR_BUILD_DIR:-${sculpt_root}/build-cathar}"
sculpt_destination="${1:-${sculpt_work}/bin/cathar}"
sculpt_toolchain="${SCULPT_CATHAR_RUST_TOOLCHAIN:-1.90.0}"
sculpt_jobs="${SCULPT_CATHAR_BUILD_JOBS:-2}"
sculpt_commit="f2c2842f89084589d069e5a8a0b61311aa70d928"
sculpt_sha256="2c6cfa7e876d452d81b32ff8c1515363ec6437d4c72a235bc03af382fd014939"
sculpt_archive="${sculpt_root}/ThirdParty/Cathar/cathar-0.8.0-source.tar.gz"
sculpt_source="${sculpt_work}/source"

if ! command -v cargo >/dev/null 2>&1 || ! command -v rustup >/dev/null 2>&1; then
  echo "Cathar richiede Rust con cargo e rustup durante la compilazione. Il plug-in compilato non richiede Rust sul Mac dell'utente."
  exit 1
fi
mkdir -p "$sculpt_work" "$(dirname -- "$sculpt_destination")"
if [[ ! -f "$sculpt_archive" ]]; then
  sculpt_archive="${sculpt_work}/cathar-source.tar.gz"
  curl --fail --location --retry 2 --output "$sculpt_archive" "https://codeload.github.com/vbasky/cathar/tar.gz/${sculpt_commit}"
fi
python3 - "$sculpt_archive" "$sculpt_sha256" <<'PY'
import hashlib,pathlib,sys
actual=hashlib.sha256(pathlib.Path(sys.argv[1]).read_bytes()).hexdigest()
if actual != sys.argv[2]:
    raise SystemExit('Cathar: checksum sorgente non valido; compilazione interrotta.')
PY
if [[ ! -f "${sculpt_source}/.sculpt-commit" ]] || [[ "$(cat "${sculpt_source}/.sculpt-commit")" != "$sculpt_commit" ]]; then
  # Only the tool-owned staging directory is replaced, never upstream or user audio.
  rm -rf -- "$sculpt_source"
  mkdir -p "$sculpt_source"
  tar -xzf "$sculpt_archive" --strip-components=1 -C "$sculpt_source"
  printf '%s\n' "$sculpt_commit" > "${sculpt_source}/.sculpt-commit"
fi

rustup toolchain install "$sculpt_toolchain" --profile minimal --no-self-update
sculpt_flags=(--manifest-path "${sculpt_source}/Cargo.toml" --locked --release --package cathar-cli --bin cathar --no-default-features --jobs "$sculpt_jobs")
if [[ "$(uname -s)" == "Darwin" ]]; then
  rustup target add --toolchain "$sculpt_toolchain" aarch64-apple-darwin x86_64-apple-darwin
  for sculpt_arch in aarch64-apple-darwin x86_64-apple-darwin; do
    env MACOSX_DEPLOYMENT_TARGET=11.0 CARGO_TARGET_DIR="${sculpt_work}/target" cargo "+${sculpt_toolchain}" build "${sculpt_flags[@]}" --target "$sculpt_arch"
  done
  lipo -create "${sculpt_work}/target/aarch64-apple-darwin/release/cathar" "${sculpt_work}/target/x86_64-apple-darwin/release/cathar" -output "$sculpt_destination"
  lipo "$sculpt_destination" -verify_arch arm64 x86_64
  codesign --force --sign - "$sculpt_destination"
else
  env CARGO_TARGET_DIR="${sculpt_work}/target" cargo "+${sculpt_toolchain}" build "${sculpt_flags[@]}"
  cp "${sculpt_work}/target/release/cathar" "$sculpt_destination"
fi
chmod 755 "$sculpt_destination"
"$sculpt_destination" --version
echo "Offline helper: $sculpt_destination"
