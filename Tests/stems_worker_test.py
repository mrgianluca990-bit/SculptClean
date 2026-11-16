#!/usr/bin/env python3
"""Exercise real file publication, rebalance, and optional downloaded-model inference.

Use the private stems Python runtime:
  python Tests/stems_worker_test.py
  python Tests/stems_worker_test.py --model-dir /path/to/models
The latter runs genuine UMX-HQ inference, not a mock backend.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

WORKER = Path(__file__).resolve().parents[1] / "tools/sculpt-stems.py"
MODELS = None


class WorkerTests(unittest.TestCase):
    def setUp(self):
        try:
            import numpy as np
            import soundfile as sf
        except ImportError:
            self.skipTest("Run with the private stems runtime (NumPy/SoundFile required)")
        self.np, self.sf = np, sf
        self.temp = tempfile.TemporaryDirectory(prefix="sculpt-stems-test-")
        self.root = Path(self.temp.name)

    def tearDown(self):
        if hasattr(self, "temp"):
            self.temp.cleanup()

    def run_worker(self, *arguments, success=True):
        proc = subprocess.run([sys.executable, str(WORKER), *map(str, arguments)],
                              capture_output=True, text=True, timeout=180)
        self.assertEqual(proc.returncode == 0, success, proc.stdout + proc.stderr)
        result = json.loads(proc.stdout.splitlines()[-1])
        self.assertEqual(result["ok"], success)
        return result

    def fake_stems(self):
        directory = self.root / "stems"
        directory.mkdir()
        rate, length = 48000, 3000
        stems = {}
        source = self.root / "source.wav"
        self.sf.write(source, self.np.zeros((length, 2)), rate, subtype="FLOAT")
        for i, name in enumerate(("vocals", "drums", "bass", "other")):
            data = self.np.full((length, 2), .01 * (i + 1), dtype=self.np.float32)
            path = directory / (name + ".wav")
            self.sf.write(path, data, rate, subtype="FLOAT")
            stems[name] = {"file": path.name, "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
        metadata = {"schema": "sculpt-stems-1", "source": str(source), "frames": length,
                    "channels": 2, "sample_rate": rate, "stems": stems}
        (directory / "stems.json").write_text(json.dumps(metadata))
        return directory, source

    def test_rebalance_and_isolate(self):
        directory, source = self.fake_stems()
        result = self.run_worker("rebalance", directory, self.root / "mix.wav")
        audio, rate = self.sf.read(result["output"])
        self.assertEqual(audio.shape, (3000, 2))
        self.assertEqual(rate, 48000)
        self.np.testing.assert_allclose(audio, .1, atol=1e-7)
        result = self.run_worker("rebalance", directory, self.root / "vocal.wav",
                                 "--drums-db", -60, "--bass-db", -60, "--other-db", -60)
        audio, _ = self.sf.read(result["output"])
        self.np.testing.assert_allclose(audio, .01, atol=1e-7)

    def test_existing_outputs_and_source_are_preserved(self):
        directory, source = self.fake_stems()
        before = source.read_bytes()
        self.run_worker("rebalance", directory, source, success=False)
        self.assertEqual(before, source.read_bytes())
        out = self.root / "result.wav"
        out.write_bytes(b"user data")
        self.run_worker("rebalance", directory, out, success=False)
        self.assertEqual(out.read_bytes(), b"user data")
        self.run_worker("separate", source, directory, success=False)

    def test_changed_stem_and_invalid_gain_are_rejected(self):
        directory, _ = self.fake_stems()
        self.run_worker("rebalance", directory, self.root / "invalid.wav",
                        "--vocals-db", "nan", success=False)
        with (directory / "bass.wav").open("ab") as handle:
            handle.write(b"changed")
        self.run_worker("rebalance", directory, self.root / "changed.wav", success=False)
        self.assertFalse((self.root / "changed.wav").exists())

    def test_cancellation_before_work(self):
        directory, source = self.fake_stems()
        cancel = self.root / "cancel"
        cancel.touch()
        result = self.run_worker("separate", source, self.root / "cancelled",
                                 "--cancel-file", cancel, success=False)
        self.assertEqual(result["error"], "Cancelled")
        self.assertFalse((self.root / "cancelled").exists())
        result = self.run_worker("rebalance", directory, self.root / "cancelled.wav",
                                 "--cancel-file", cancel, success=False)
        self.assertFalse((self.root / "cancelled.wav").exists())
        setup_root = self.root / "install"
        proc = subprocess.run(["bash", str(WORKER.with_name("setup-stems.sh")),
                               str(setup_root), str(cancel)], capture_output=True,
                              text=True, timeout=30)
        self.assertEqual(proc.returncode, 130, proc.stdout + proc.stderr)
        self.assertFalse((setup_root / ".setup-lock").exists())

    def test_real_inference_and_reconstruction(self):
        if MODELS is None:
            self.skipTest("Pass --model-dir for real trained-model inference")
        rate, frames = 48000, 96000
        t = self.np.arange(frames) / rate
        rng = self.np.random.default_rng(182)
        left = .07 * self.np.sin(2 * self.np.pi * 110 * t) + .02 * rng.standard_normal(frames)
        right = left * .8 + .03 * self.np.sin(2 * self.np.pi * 663 * t)
        audio = self.np.column_stack((left, right)).astype(self.np.float32)
        source = self.root / "synthetic.wav"
        self.sf.write(source, audio, rate, subtype="FLOAT")
        result = self.run_worker("separate", source, self.root / "separated",
                                 "--model-dir", MODELS, "--threads", 2)
        self.assertEqual(result["frames"], frames)
        total = self.np.zeros_like(audio, dtype=self.np.float64)
        energies = []
        for name in ("vocals", "drums", "bass", "other"):
            stem, stem_rate = self.sf.read(self.root / "separated" / (name + ".wav"))
            self.assertEqual(stem.shape, audio.shape)
            self.assertEqual(stem_rate, rate)
            self.assertTrue(self.np.isfinite(stem).all())
            energies.append(float(self.np.mean(stem ** 2)))
            total += stem
        self.np.testing.assert_allclose(total, audio, atol=1e-6)
        self.assertTrue(all(value > 0 for value in energies))
        self.assertGreater(max(energies) - min(energies), 1e-9)
        print(json.dumps({"actual_inference_seconds": result["elapsed_seconds"],
                          "audio_seconds": 2.0, "stem_energies": energies,
                          "sum_max_error": float(self.np.max(self.np.abs(total - audio)))}))

    def test_real_chunked_mono(self):
        if MODELS is None:
            self.skipTest("Pass --model-dir for real trained-model inference")
        rate, frames = 44100, 5 * 44100
        t = self.np.arange(frames) / rate
        audio = (.04 * self.np.sin(2 * self.np.pi * 220 * t)).astype(self.np.float32)
        source = self.root / "mono.wav"
        self.sf.write(source, audio, rate, subtype="FLOAT")
        result = self.run_worker("separate", source, self.root / "mono-stems",
                                 "--model-dir", MODELS, "--threads", 2)
        total = self.np.zeros(frames)
        for name in ("vocals", "drums", "bass", "other"):
            stem, stem_rate = self.sf.read(self.root / "mono-stems" / (name + ".wav"))
            self.assertEqual(stem.shape, (frames,))
            self.assertEqual(stem_rate, rate)
            self.assertTrue(self.np.isfinite(stem).all())
            total += stem
        self.np.testing.assert_allclose(total, audio, atol=1e-6)
        print(json.dumps({"chunked_mono_inference_seconds": result["elapsed_seconds"],
                          "audio_seconds": 5.0}))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--model-dir", type=Path)
    args, remaining = parser.parse_known_args()
    MODELS = args.model_dir.resolve() if args.model_dir else None
    unittest.main(argv=[sys.argv[0], *remaining], verbosity=2)
