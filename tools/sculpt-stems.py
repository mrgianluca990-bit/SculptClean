#!/usr/bin/env python3
"""Local, offline Open-Unmix UMX-HQ separation worker for Sculpt Clean Studio.

Only setup/download-models use the network. Separation loads SHA256-verified
official weights. This worker never runs in the VST3 audio callback.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.metadata
import json
import math
import os
from pathlib import Path
import shutil
import signal
import sys
import tempfile
import time
import urllib.request

STEMS = ("vocals", "drums", "bass", "other")
HERE = Path(__file__).resolve().parent
SCHEMA = "sculpt-stems-1"
EXPECTED = {"openunmix": "1.3.0", "torch": "2.2.2", "torchaudio": "2.2.2",
            "numpy": "1.26.4", "soundfile": "0.12.1", "certifi": "2026.10.23"}


def default_models():
    if sys.platform == "darwin":
        return Path.home() / "Library/Application Support/SculptClean/stems/models"
    return Path.home() / ".local/share/SculptClean/stems/models"


def report(payload, progress=False):
    print(json.dumps(payload, allow_nan=False), file=sys.stderr if progress else sys.stdout, flush=True)


def check_cancel(args):
    if getattr(args, "cancel_file", None) is not None and args.cancel_file.exists():
        raise InterruptedError("Cancelled")


def publish_without_replacement(source, destination):
    """Atomically publish a completed file/directory; reject an existing name."""
    import ctypes
    libc = ctypes.CDLL(None, use_errno=True)
    old, new = os.fsencode(source), os.fsencode(destination)
    if sys.platform == "darwin":
        function = libc.renamex_np
        function.argtypes = (ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint)
        result = function(old, new, 0x00000004)  # XNU RENAME_EXCL
    elif sys.platform.startswith("linux") and hasattr(libc, "renameat2"):
        function = libc.renameat2
        function.argtypes = (ctypes.c_int, ctypes.c_char_p, ctypes.c_int,
                             ctypes.c_char_p, ctypes.c_uint)
        result = function(-100, old, -100, new, 1)  # AT_FDCWD, RENAME_NOREPLACE
    else:
        raise OSError("Atomic exclusive output publication requires macOS or modern Linux")
    if result:
        error = ctypes.get_errno()
        raise OSError(error, os.strerror(error), str(destination))


def digest(path):
    h = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def model_manifest():
    return json.loads((HERE / "stems-models.json").read_text())


def check_models(directory):
    missing, invalid = [], []
    for entry in model_manifest()["files"]:
        path = directory / entry["filename"]
        if not path.is_file():
            missing.append(entry["filename"])
        elif path.stat().st_size != entry["bytes"] or digest(path) != entry["sha256"]:
            invalid.append(entry["filename"])
    return missing, invalid


def status(args):
    versions, missing, incompatible = {}, [], []
    for package, version in EXPECTED.items():
        try:
            actual = importlib.metadata.version(package)
            versions[package] = actual
            if actual.split("+")[0] != version:
                incompatible.append(package + ": " + actual + " (expected " + version + ")")
        except importlib.metadata.PackageNotFoundError:
            missing.append(package)
    missing_models, invalid_models = check_models(args.model_dir)
    runtime_error = None
    if not missing and not incompatible:
        try:
            # Validate native extension imports, not just package metadata.
            import torch, torchaudio, soundfile, openunmix  # noqa: F401
        except Exception as exc:
            runtime_error = str(exc)
    ready = not (missing or incompatible or missing_models or invalid_models or runtime_error)
    result = {"ok": True, "ready": ready, "backend": "Open-Unmix UMX-HQ",
              "model_license": "MIT", "device": "cpu", "versions": versions,
              "model_dir": str(args.model_dir), "missing_packages": missing,
              "incompatible_packages": incompatible, "missing_models": missing_models,
              "invalid_models": invalid_models, "runtime_error": runtime_error,
              "python": sys.executable}
    report(result)
    return 0 if ready or not args.require_ready else 1


def download_models(args):
    import certifi
    import ssl
    # Some macOS Pythons have no usable OpenSSL CA bundle even when pip works.
    # Add pinned Mozilla roots while retaining the system's configured roots.
    tls_context = ssl.create_default_context()
    tls_context.load_verify_locations(cafile=certifi.where())
    args.model_dir.mkdir(parents=True, exist_ok=True)
    for entry in model_manifest()["files"]:
        check_cancel(args)
        target = args.model_dir / entry["filename"]
        if target.exists() and digest(target) == entry["sha256"]:
            continue
        report({"stage": "download", "model": entry["stem"]}, True)
        fd, temporary = tempfile.mkstemp(prefix=".download-", dir=args.model_dir)
        try:
            with os.fdopen(fd, "wb") as out:
                with urllib.request.urlopen(entry["url"], timeout=120, context=tls_context) as response:
                    while True:
                        check_cancel(args)
                        block = response.read(1024 * 1024)
                        if not block:
                            break
                        out.write(block)
                out.flush()
                os.fsync(out.fileno())
            if Path(temporary).stat().st_size != entry["bytes"] or digest(temporary) != entry["sha256"]:
                raise ValueError("Downloaded model hash does not match: " + entry["filename"])
            os.replace(temporary, target)
        finally:
            Path(temporary).unlink(missing_ok=True)
    report({"ok": True, "model_dir": str(args.model_dir), "verified": 4})
    return 0


def configure_threads(count):
    # Set before NumPy/Torch imports; never silently select GPU/MPS.
    if count < 1 or count > 2:
        raise ValueError("threads must be 1 or 2")
    for name in ("OMP_NUM_THREADS", "MKL_NUM_THREADS", "OPENBLAS_NUM_THREADS",
                 "VECLIB_MAXIMUM_THREADS", "NUMEXPR_NUM_THREADS"):
        os.environ[name] = str(count)


def load_separator(model_dir, threads, args):
    missing, invalid = check_models(model_dir)
    if missing or invalid:
        raise ValueError("Models are not ready; run setup. Missing: " + ", ".join(missing)
                         + "; invalid: " + ", ".join(invalid))
    import torch
    import openunmix
    if torch.__version__.split("+")[0] != "2.2.2":
        raise ValueError("This worker requires tested PyTorch 2.2.2; run setup")
    torch.set_num_threads(threads)
    torch.set_num_interop_threads(1)
    separator = openunmix.umxhq(targets=list(STEMS), pretrained=False,
                               device="cpu", niter=1, residual=False, wiener_win_len=100)
    for entry in model_manifest()["files"]:
        check_cancel(args)
        # Official pinned files only; weights_only avoids arbitrary pickle execution.
        state = torch.load(str(model_dir / entry["filename"]), map_location="cpu", weights_only=True)
        # The 2019 checkpoint bundles three deterministic analysis buffers that
        # Open-Unmix 1.3 moved from each target into Separator. Keep strict checks
        # on every learned tensor rather than silently accepting missing weights.
        for legacy_buffer in ("sample_rate", "stft.window", "transform.0.window"):
            state.pop(legacy_buffer, None)
        separator.target_models[entry["stem"]].load_state_dict(state, strict=True)
    return separator.eval()


def resample(audio, original_rate, target_rate, length=None):
    import numpy as np
    if original_rate != target_rate:
        import torch
        from torchaudio.functional import resample as bandlimited_resample
        tensor = torch.from_numpy(np.ascontiguousarray(audio.T))
        tensor = bandlimited_resample(tensor, original_rate, target_rate,
                                      lowpass_filter_width=64, rolloff=0.9475937167,
                                      resampling_method="sinc_interp_kaiser", beta=14.76965646)
        audio = np.ascontiguousarray(tensor.numpy().T, dtype=np.float32)
    if length is not None:
        if len(audio) < length:
            audio = np.pad(audio, ((0, length - len(audio)), (0, 0)))
        audio = audio[:length]
    return audio


def read_audio(path):
    import numpy as np
    import soundfile as sf
    audio, rate = sf.read(str(path), dtype="float32", always_2d=True)
    if audio.shape[1] not in (1, 2) or len(audio) == 0:
        raise ValueError("Input must contain non-empty mono or stereo audio")
    if not 8000 <= rate <= 192000:
        raise ValueError("Sample rate must be between 8 kHz and 192 kHz")
    if not np.isfinite(audio).all():
        raise ValueError("Input contains non-finite samples")
    return audio, rate


def separate(args):
    check_cancel(args)
    configure_threads(args.threads)
    import numpy as np
    import soundfile as sf
    import torch
    source = args.input.resolve(strict=True)
    destination = args.output_dir.absolute()
    if destination.exists() or destination.is_symlink():
        raise ValueError("Output directory already exists; choose a new directory")
    destination.parent.mkdir(parents=True, exist_ok=True)
    audio, rate = read_audio(source)
    source_hash = digest(source)
    channels, frames = audio.shape[1], len(audio)
    started = time.perf_counter()
    report({"stage": "load", "progress": 0.0}, True)
    separator = load_separator(args.model_dir, args.threads, args)
    native = resample(audio, rate, 44100)
    if channels == 1:
        native = np.repeat(native, 2, axis=1)
    # 4 s cores with 1 s context each side bound neural temporary working memory.
    # Adjacent cores share context; short crossfades prevent hard chunk boundaries.
    core, context, fade = 4 * 44100, 44100, 4410
    estimates = np.zeros((4, len(native), 2), dtype=np.float32)
    weights = np.zeros(len(native), dtype=np.float32)
    with torch.inference_mode():
        for start in range(0, len(native), core):
            check_cancel(args)
            end = min(len(native), start + core)
            left, right = max(0, start - context), min(len(native), end + context)
            chunk = native[left:right]
            # Reflection STFT requires at least n_fft/2+1 samples.
            if len(chunk) < 4096:
                chunk = np.pad(chunk, ((0, 4096 - len(chunk)), (0, 0)))
            tensor = torch.from_numpy(np.ascontiguousarray(chunk.T)).unsqueeze(0)
            prediction = separator(tensor)[0].permute(0, 2, 1).cpu().numpy()
            check_cancel(args)
            out_start, out_end = max(0, start - fade), min(len(native), end + fade)
            segment = prediction[:, out_start - left:out_end - left, :]
            window = np.ones(out_end - out_start, dtype=np.float32)
            if out_start < start:
                window[:start - out_start] = np.linspace(0, 1, start - out_start, endpoint=False)
            if out_end > end:
                window[end - out_start:] = np.linspace(1, 0, out_end - end, endpoint=False)
            estimates[:, out_start:out_end, :] += segment * window[None, :, None]
            weights[out_start:out_end] += window
            report({"stage": "separate", "progress": end / len(native)}, True)
    estimates /= np.maximum(weights[None, :, None], 1e-8)
    restored = []
    for stem in estimates:
        if channels == 1:
            stem = stem.mean(axis=1, keepdims=True)
        restored.append(resample(stem, 44100, rate, frames))
    # Preserve the source exactly when all gains equal zero dB. Assign only the
    # reconstruction residual to Other; this is disclosed in stems.json.
    restored[3] += audio - np.sum(np.stack(restored), axis=0)
    if not all(np.isfinite(stem).all() for stem in restored):
        raise ValueError("Model produced invalid audio; no files were written")
    temporary = Path(tempfile.mkdtemp(prefix=".sculpt-stems-", dir=destination.parent))
    try:
        files = {}
        for name, stem in zip(STEMS, restored):
            check_cancel(args)
            path = temporary / (name + ".wav")
            sf.write(str(path), stem, rate, subtype="FLOAT")
            files[name] = {"file": path.name, "sha256": digest(path)}
        manifest = {"schema": SCHEMA, "backend": "Open-Unmix", "model": "umxhq",
                    "model_license": "MIT", "openunmix": "1.3.0", "sample_rate": rate,
                    "channels": channels, "frames": frames, "source": str(source),
                    "source_sha256": source_hash, "stems": files, "threads": args.threads,
                    "device": "cpu", "niter": 1, "core_seconds": 4, "context_seconds": 1,
                    "crossfade_seconds": 0.1, "mixture_consistency": "residual_to_other",
                    "model_files": model_manifest()["files"],
                    "elapsed_seconds": time.perf_counter() - started}
        (temporary / "stems.json").write_text(json.dumps(manifest, indent=2) + "\n")
        check_cancel(args)
        publish_without_replacement(temporary, destination)
    finally:
        shutil.rmtree(temporary, ignore_errors=True)
    report({"ok": True, "output_dir": str(destination.resolve()), "stems": list(STEMS),
            "frames": frames, "sample_rate": rate,
            "elapsed_seconds": manifest["elapsed_seconds"]})
    return 0


def rebalance(args):
    check_cancel(args)
    configure_threads(args.threads)
    import numpy as np
    import soundfile as sf
    directory = args.stem_dir.resolve(strict=True)
    metadata = json.loads((directory / "stems.json").read_text())
    if metadata.get("schema") != SCHEMA:
        raise ValueError("Unrecognised stem manifest")
    destination = args.output.absolute()
    if destination.exists() or destination.is_symlink():
        raise ValueError("Output already exists; choose a new WAV filename")
    if destination.suffix.lower() != ".wav":
        raise ValueError("Rebalance output must be a .wav file")
    if destination.resolve() == Path(metadata["source"]).resolve():
        raise ValueError("The source audio must never be overwritten")
    output, rate, gains = None, None, {}
    for name in STEMS:
        check_cancel(args)
        gain_db = getattr(args, name + "_db")
        if not math.isfinite(gain_db) or not -60 <= gain_db <= 12:
            raise ValueError("Stem gains must be finite and between -60 and +12 dB")
        entry = metadata["stems"][name]
        path = (directory / entry["file"]).resolve(strict=True)
        if path.parent != directory or path.suffix.lower() != ".wav":
            raise ValueError("Stem paths must remain inside the selected stem directory")
        if digest(path) != entry["sha256"]:
            raise ValueError("Stem file changed since separation: " + name)
        audio, stem_rate = read_audio(path)
        if (stem_rate != metadata["sample_rate"] or len(audio) != metadata["frames"]
                or audio.shape[1] != metadata["channels"]):
            raise ValueError("Stem lengths, rates or channel counts do not match")
        if output is None:
            output = np.zeros_like(audio, dtype=np.float64)
            rate = stem_rate
        # -60 dB is a deliberate hard mute, useful for vocals-only/instrumental.
        gain = 0.0 if gain_db <= -60 else 10.0 ** (gain_db / 20.0)
        output += audio * gain
        gains[name] = gain_db
    if not np.isfinite(output).all():
        raise ValueError("Rebalanced audio is invalid")
    destination.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=".sculpt-rebalance-", suffix=".wav", dir=destination.parent)
    os.close(fd)
    try:
        sf.write(temporary, output, rate, subtype="FLOAT")
        check_cancel(args)
        publish_without_replacement(temporary, destination)
    finally:
        Path(temporary).unlink(missing_ok=True)
    report({"ok": True, "output": str(destination.resolve()), "frames": len(output),
            "sample_rate": rate, "gains_db": gains,
            "peak": float(np.max(np.abs(output))), "format": "WAV float32"})
    return 0


def main():
    def cancelled_signal(_number, _frame):
        raise InterruptedError("Cancelled")
    signal.signal(signal.SIGTERM, cancelled_signal)
    signal.signal(signal.SIGINT, cancelled_signal)
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    for name in ("status", "download-models"):
        item = sub.add_parser(name)
        item.add_argument("--model-dir", type=Path, default=default_models())
        if name == "status":
            item.add_argument("--require-ready", action="store_true")
        else:
            item.add_argument("--cancel-file", type=Path)
    item = sub.add_parser("separate")
    item.add_argument("input", type=Path)
    item.add_argument("output_dir", type=Path)
    item.add_argument("--model-dir", type=Path, default=default_models())
    item.add_argument("--threads", type=int, default=2)
    item.add_argument("--cancel-file", type=Path)
    item = sub.add_parser("rebalance")
    item.add_argument("stem_dir", type=Path)
    item.add_argument("output", type=Path)
    item.add_argument("--threads", type=int, default=2)
    item.add_argument("--cancel-file", type=Path)
    for name in STEMS:
        item.add_argument("--" + name + "-db", type=float, default=0)
    args = parser.parse_args()
    try:
        return {"status": status, "download-models": download_models,
                "separate": separate, "rebalance": rebalance}[args.command](args)
    except (Exception, KeyboardInterrupt) as exc:
        report({"ok": False, "error": str(exc) or "Cancelled", "command": args.command})
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
