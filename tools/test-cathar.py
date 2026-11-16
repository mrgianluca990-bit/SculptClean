#!/usr/bin/env python3
"""Deterministic, dependency-free CLI fixtures for the pinned Cathar helper.

This validates actual rendered audio, not menu labels. It also records the
known WPE limitation on a wanted sustained tone rather than treating it as a
speech-separation result. No original or pre-existing output is overwritten.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import platform
import random
import re
import struct
import subprocess
import tempfile
import time


def write_wav(path, rate, channels):
    frames = len(channels[0])
    assert all(len(channel) == frames for channel in channels)
    payload = bytearray()
    for frame in range(frames):
        payload.extend(struct.pack('<' + 'f' * len(channels), *(c[frame] for c in channels)))
    fmt = struct.pack('<HHIIHH', 3, len(channels), rate, rate * 4 * len(channels), 4 * len(channels), 32)
    data = b'WAVEfmt ' + struct.pack('<I', len(fmt)) + fmt + b'data' + struct.pack('<I', len(payload)) + payload
    with path.open('xb') as out:
        out.write(b'RIFF' + struct.pack('<I', len(data)) + data)


def read_wav(path):
    data = path.read_bytes()
    if data[:4] != b'RIFF' or data[8:12] != b'WAVE':
        raise AssertionError('Output is not a RIFF WAV')
    offset = 12
    fmt = payload = None
    while offset + 8 <= len(data):
        tag, length = struct.unpack_from('<4sI', data, offset)
        body = data[offset + 8:offset + 8 + length]
        if len(body) != length:
            raise AssertionError('Truncated WAV chunk')
        if tag == b'fmt ':
            fmt = body
        elif tag == b'data':
            payload = body
        offset += 8 + length + (length & 1)
    if fmt is None or payload is None:
        raise AssertionError('Missing WAV format or audio')
    code, channels, rate, _, align, bits = struct.unpack_from('<HHIIHH', fmt)
    if code == 65534 and len(fmt) >= 40:
        code = struct.unpack_from('<H', fmt, 24)[0]
    if code != 3 or bits != 32 or align != channels * 4:
        raise AssertionError('Expected float32 WAV output')
    values = struct.unpack('<' + 'f' * (len(payload) // 4), payload)
    if any(not math.isfinite(v) for v in values):
        raise AssertionError('Rendered non-finite audio')
    return rate, [list(values[c::channels]) for c in range(channels)]


def rms(samples):
    return math.sqrt(sum(v * v for v in samples) / max(1, len(samples)))


def db_ratio(after, before):
    return 20 * math.log10(max(1e-15, after) / max(1e-15, before))


def tone_amplitude(samples, rate, hz, begin=0, end=None):
    end = len(samples) if end is None else end
    re = im = 0.0
    for n in range(begin, end):
        phase = 2 * math.pi * hz * n / rate
        re += samples[n] * math.cos(phase)
        im += samples[n] * math.sin(phase)
    return 2 * math.hypot(re, im) / max(1, end - begin)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--binary', required=True, type=Path)
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    binary = args.binary.resolve()
    if not binary.is_file():
        raise SystemExit('Cathar binary is missing')
    version = subprocess.run([str(binary), '--version'], check=True, capture_output=True, text=True, timeout=10).stdout.strip()
    if '0.8.0' not in version:
        raise SystemExit('Unexpected Cathar version: ' + version)
    report = {'engine': version, 'source_commit': 'f2c2842f89084589d069e5a8a0b61311aa70d928',
              'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
              'platform': platform.platform(),
              'fixture_sample_rate_hz': 16000, 'fixture_duration_s': 3,
              'scope': 'Synthetic fixtures; not a real-recording listening comparison or RX parity claim',
              'tests': [], 'limitations': []}
    failures = []
    with tempfile.TemporaryDirectory(prefix='sculpt-cathar-test-') as directory:
        work = Path(directory)
        counter = 0

        def render(command, samples, rate=16000, options=(), reference=None):
            nonlocal counter
            counter += 1
            source = work / ('input-' + str(counter) + '.wav')
            target = work / ('output-' + str(counter) + '.wav')
            channels = samples if isinstance(samples[0], list) else [samples]
            write_wav(source, rate, channels)
            digest = hashlib.sha256(source.read_bytes()).hexdigest()
            argv = [str(binary), command, str(source), '--out', str(target), *map(str, options)]
            if reference is not None:
                ref = work / ('reference-' + str(counter) + '.wav')
                write_wav(ref, rate, [reference])
                argv += ['--reference', str(ref)]
            start = time.monotonic()
            completed = subprocess.run(argv, check=False, capture_output=True, text=True, timeout=120)
            if completed.returncode != 0:
                raise AssertionError(completed.stderr[-2500:])
            if hashlib.sha256(source.read_bytes()).hexdigest() != digest:
                raise AssertionError('Helper modified the original')
            out_rate, output = read_wav(target)
            if out_rate != rate or len(output) != len(channels):
                raise AssertionError('Sample rate or channel count changed')
            if command != 'dewow' and any(len(c) != len(channels[0]) for c in output):
                raise AssertionError('Length changed in a non-time operation')
            return output, time.monotonic() - start

        def check(name, function):
            try:
                metrics = function()
                report['tests'].append({'name': name, 'passed': True, **metrics})
                print('PASS:', name, json.dumps(metrics), flush=True)
            except (AssertionError, OSError, subprocess.SubprocessError, ValueError) as error:
                failures.append(name)
                report['tests'].append({'name': name, 'passed': False, 'error': str(error)})
                print('FAIL:', name, str(error), flush=True)

        rate = 16000
        length = rate * 3
        sine = lambda hz, amp=.1: [amp * math.sin(2 * math.pi * hz * n / rate) for n in range(length)]

        def dehum():
            wanted = sine(997, .15)
            hum = [a + b for a, b in zip(sine(50, .15), sine(100, .08))]
            damaged = [a + b for a, b in zip(wanted, hum)]
            output, seconds = render('dehum', damaged, options=['--freq', 0, '--harmonics', 8, '--adaptive'])
            after = output[0]
            reduction = db_ratio(tone_amplitude(after, rate, 50, rate), tone_amplitude(damaged, rate, 50, rate))
            wanted_change = db_ratio(tone_amplitude(after, rate, 997, rate), tone_amplitude(wanted, rate, 997, rate))
            assert reduction < -3, 'Hum reduction below 3 dB'
            assert abs(wanted_change) < 1, 'Distant wanted tone changed by 1 dB or more'
            return {'hum_change_db': reduction, 'wanted_997hz_change_db': wanted_change, 'seconds': seconds}
        check('adaptive de-hum: 50 Hz plus harmonic and wanted tone', dehum)

        def wind():
            wanted = sine(997, .1)
            damaged = [a + b for a, b in zip(wanted, sine(30, .2))]
            output, seconds = render('dewind', damaged, options=['--cutoff', 80])
            reduction = db_ratio(tone_amplitude(output[0], rate, 30, rate), tone_amplitude(damaged, rate, 30, rate))
            wanted_change = db_ratio(tone_amplitude(output[0], rate, 997, rate), tone_amplitude(wanted, rate, 997, rate))
            assert reduction < -20, 'Low-frequency reduction below 20 dB'
            assert abs(wanted_change) < .1, 'Wanted tone was changed'
            return {'30hz_change_db': reduction, '997hz_change_db': wanted_change, 'seconds': seconds}
        check('wind high-pass frequency discrimination', wind)

        def clicks():
            wanted = sine(311, .06)
            damaged = wanted.copy()
            for pos in range(3000, length - 3000, 6001):
                damaged[pos] = .9
                damaged[pos + 1] = -.65
            output, seconds = render('declick', damaged, options=['--threshold', 5, '--method', 'ar'])
            before = rms([a - b for a, b in zip(damaged, wanted)])
            after = rms([a - b for a, b in zip(output[0], wanted)])
            change = db_ratio(after, before)
            assert change < -6, 'Click repair did not improve known-clean error by 6 dB'
            return {'error_to_clean_change_db': change, 'seconds': seconds}
        check('autoregressive de-click against known clean waveform', clicks)

        def crackle():
            wanted = sine(311, .06)
            damaged = wanted.copy()
            for pos in range(1000, length - 1000, 211):
                damaged[pos] += .09 if pos % 2 else -.09
            output, seconds = render('decrackle', damaged, options=['--sensitivity', 7])
            change = db_ratio(rms([a - b for a, b in zip(output[0], wanted)]), rms([a - b for a, b in zip(damaged, wanted)]))
            assert change < -3, 'Crackle repair did not improve known-clean error by 3 dB'
            return {'error_to_clean_change_db': change, 'seconds': seconds}
        check('de-crackle against known clean waveform', crackle)

        def wpe():
            rng = random.Random(251)
            dry = [(.3 * (2 * rng.random() - 1) if n % (rate // 2) < rate // 20 else 0) for n in range(length)]
            wet = dry.copy()
            for delay, gain in ((.03,.6),(.06,.4),(.10,.28),(.16,.18),(.24,.1)):
                shift = round(delay * rate)
                for n in range(shift, length):
                    wet[n] += gain * dry[n - shift]
            output, seconds = render('dereverb', wet, options=['--wpe', '--taps', 12, '--delay', 2])
            tail_indices = [n for n in range(rate // 2, length) if n % (rate // 2) > rate // 5]
            change = db_ratio(rms([output[0][n] for n in tail_indices]), rms([wet[n] for n in tail_indices]))
            assert change < -1.5, 'Known room tail reduction below 1.5 dB'
            steady = sine(440, .15)
            tone_out, _ = render('dereverb', steady, options=['--wpe', '--taps', 12, '--delay', 2])
            tone_change = db_ratio(rms(tone_out[0][rate:]), rms(steady[rate:]))
            report['limitations'].append({'module':'WPE', 'wanted_sustained_440hz_change_db':tone_change,
                'meaning':'A predictable wanted note can be removed; use conservative wet/dry and audition.'})
            return {'late_tail_change_db': change, 'wanted_tone_change_db': tone_change, 'seconds': seconds}
        check('WPE: convolved sparse room and explicit tonal limitation', wpe)

        def deess():
            low = sine(311, .1)
            high = sine(6000, .003)
            for n in range(round(.8 * rate), round(1.0 * rate)):
                high[n] = .3 * math.sin(2 * math.pi * 6000 * n / rate)
            damaged = [a + b for a, b in zip(low, high)]
            output, seconds = render('deesser', damaged, options=['--freq', 4000, '--threshold=6', '--bands', 3])
            start, end = round(.8 * rate), round(1.0 * rate)
            reduction = db_ratio(tone_amplitude(output[0],rate,6000,start,end), tone_amplitude(damaged,rate,6000,start,end))
            low_change = db_ratio(tone_amplitude(output[0],rate,311,rate,2*rate), tone_amplitude(damaged,rate,311,rate,2*rate))
            assert reduction < -.1, 'Sibilant burst was not reduced'
            assert abs(low_change) < .2, 'Low-band tone changed by more than 0.2 dB'
            return {'6000hz_burst_change_db': reduction, '311hz_change_db': low_change, 'seconds': seconds}
        check('adaptive multiband de-ess', deess)

        def unity_match():
            signal = [a + b for a,b in zip(sine(311,.1),sine(997,.03))]
            output, seconds = render('rebalance', signal, options=['--strength',1], reference=signal)
            error = db_ratio(rms([a-b for a,b in zip(output[0][1000:-1000],signal[1000:-1000])]),rms(signal[1000:-1000]))
            assert error < -80, 'Same-reference EQ Match is not sufficiently transparent'
            # A passthrough would pass the identity check, so also require a real
            # spectral correction towards an independently specified reference.
            flat = [a + b for a,b in zip(sine(311,.1),sine(997,.1))]
            reference = [a + b for a,b in zip(sine(311,.03),sine(997,.1))]
            tilted, tilt_seconds = render('rebalance', flat, options=['--strength',1], reference=reference)
            expected = db_ratio(.1,.03)
            measured = db_ratio(tone_amplitude(tilted[0],rate,997,rate,2*rate), tone_amplitude(tilted[0],rate,311,rate,2*rate))
            assert abs(measured-expected)<1, 'Reference spectral balance was not matched within 1 dB'
            return {'same_reference_error_db':error, 'reference_high_low_ratio_db':expected,
                    'output_high_low_ratio_db':measured, 'seconds':seconds+tilt_seconds}
        check('EQ Match: identity and actual reference correction', unity_match)

        def format_and_options():
            signal = sine(311,.03)
            values = {}
            for command, options in [('deplosive',['--strength',4,'--method','events']),('derustle',['--strength',4]),('normalize',['--target=-23','--true-peak=-1']),('dewow',[])]:
                output, seconds = render(command,[signal,[-.5*x for x in signal]],options=options)
                assert len(output[0]) >= len(signal)-16 and len(output[0]) <= len(signal)+16, 'Unexpected duration change'
                values[command+'_seconds'] = seconds
                if command == 'normalize':
                    normalized_file = work/'normalized-measurement.wav'
                    write_wav(normalized_file,rate,output)
                    measured_stats = subprocess.run([str(binary),'stats',str(normalized_file)],check=True,capture_output=True,text=True,timeout=60).stdout
                    lufs_match = re.search(r'Integrated\s+(-?[\d.]+)\s+LUFS',measured_stats)
                    tp_match = re.search(r'True peak\s+(-?[\d.]+)\s+dBTP',measured_stats)
                    assert lufs_match and tp_match, 'Statistics omit actual LUFS or true-peak values'
                    measured_lufs = float(lufs_match.group(1))
                    measured_tp = float(tp_match.group(1))
                    assert abs(measured_lufs+23)<.2, 'Normalization missed the -23 LUFS target'
                    assert measured_tp<=-1, 'Normalization exceeded its true-peak ceiling'
                    values['normalized_lufs']=measured_lufs
                    values['normalized_true_peak_dbtp']=measured_tp
            # Resampling is checked separately because its format should change.
            source=work/'resample-in.wav';target=work/'resample-out.wav'
            write_wav(source,rate,[signal,[-.5*x for x in signal]])
            subprocess.run([str(binary),'resample',str(source),'--rate','48000','--out',str(target)],check=True,capture_output=True,timeout=60)
            out_rate, output = read_wav(target)
            assert out_rate==48000 and len(output)==2 and abs(len(output[0])-3*len(signal))<=3
            stats=subprocess.run([str(binary),'stats',str(source)],check=True,capture_output=True,text=True,timeout=60).stdout
            assert 'LUFS' in stats and 'True peak' in stats
            values['stats_has_lufs_and_true_peak']=True
            return values
        check('stereo formats, additional CLI options, resampling and stats',format_and_options)

    report['passed'] = not failures
    if args.report:
        args.report.parent.mkdir(parents=True,exist_ok=True)
        args.report.write_text(json.dumps(report,indent=2)+'\n')
    if failures:
        raise SystemExit('Cathar fixture failures: '+', '.join(failures))
    print('All Cathar CLI integration fixtures passed.')


if __name__ == '__main__':
    main()
