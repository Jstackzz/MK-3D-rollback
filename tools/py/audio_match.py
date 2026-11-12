#!/usr/bin/env python3
"""Where a piece of music plays in a game's audio dump (XBE_AUDIO_DUMP: 48 kHz float stereo WAV): for each candidate
(a WAV, or a stream of a sound bank as <bank.msx>:<id>, or any track ffmpeg reads) the times in the dump where it is
heard, by normalised cross-correlation of 8 kHz mono envelopes over a sliding window.

  python tools/py/audio_match.py <dump.wav> <candidate> [<candidate> ...] [--window 6] [--step 2] [--min 0.5]

Prints, per candidate, the dump times (seconds) where a window of it matches and at what offset into the candidate.
"""
import os
import struct
import subprocess
import sys

import numpy as np

RATE = 8000


def read_wav(path):
    d = open(path, 'rb').read()
    if d[:4] != b'RIFF':
        raise SystemExit(f'{path}: not a WAV')
    p = 12
    fmt = None
    while p + 8 <= len(d):
        cid = d[p:p + 4]
        csz = struct.unpack_from('<I', d, p + 4)[0]
        if cid == b'fmt ':
            tag, ch, rate, _, align, bits = struct.unpack_from('<HHIIHH', d, p + 8)
            fmt = (tag, ch, rate, bits)
        elif cid == b'data':
            body = d[p + 8:p + 8 + csz] if csz else d[p + 8:]
            break
        p += 8 + csz + (csz & 1)
    tag, ch, rate, bits = fmt
    if tag == 3 and bits == 32:
        a = np.frombuffer(body[:len(body) // 4 * 4], dtype='<f4').astype(np.float32)
    elif tag == 1 and bits == 16:
        a = np.frombuffer(body[:len(body) // 2 * 2], dtype='<i2').astype(np.float32) / 32768.0
    else:
        raise SystemExit(f'{path}: format {fmt} not handled')
    a = a[:len(a) // ch * ch].reshape(-1, ch).mean(axis=1)
    return a, rate


def resample(a, rate, to=RATE):
    if rate == to:
        return a
    n = int(len(a) * to / rate)
    x = np.linspace(0, len(a) - 1, n)
    return np.interp(x, np.arange(len(a)), a).astype(np.float32)


def load_candidate(spec, tmpdir):
    """mono float at RATE"""
    if ':' in spec and spec.lower().rsplit(':', 1)[0].endswith('.msx'):
        bank, id_ = spec.rsplit(':', 1)
        out = os.path.join(tmpdir, f'{os.path.basename(bank)}-{id_}.wav')
        here = os.path.dirname(os.path.abspath(__file__))
        subprocess.run([sys.executable, os.path.join(here, 'msxbank.py'), 'extract', bank, id_, out], check=True,
                       stdout=subprocess.DEVNULL)
        a, rate = read_wav(out)
        return resample(a, rate)
    if spec.lower().endswith('.wav'):
        a, rate = read_wav(spec)
        return resample(a, rate)
    raw = subprocess.run(['ffmpeg', '-y', '-loglevel', 'error', '-i', spec, '-ac', '1', '-ar', str(RATE), '-f', 'f32le',
                          '-'], capture_output=True, check=True).stdout
    return np.frombuffer(raw, dtype='<f4').astype(np.float32)


def envelope(a):
    # a band-passed, rectified signal: music structure without the exact phase
    a = a - np.convolve(a, np.ones(64) / 64, mode='same')
    return np.abs(a)


def best_offsets(dump, cand, window, step, threshold):
    """for each window of the dump: the offset into cand with the highest normalised correlation"""
    w = int(min(window * RATE, len(cand)))  # a candidate shorter than the window is matched whole
    s = int(step * RATE)
    env_c = envelope(cand)
    env_c = env_c - env_c.mean()
    out = []
    from scipy.signal import correlate
    for start in range(0, max(1, len(dump) - w), s):
        seg = envelope(dump[start:start + w])
        seg = seg - seg.mean()
        if np.sqrt(np.mean(seg * seg)) < 1e-4 or len(seg) < w // 2:
            continue
        # correlate seg against cand (full), normalised by both energies
        corr = correlate(env_c, seg, mode='valid', method='fft') if len(env_c) >= len(seg) else np.array([0.0])
        norm_seg = np.sqrt(np.sum(seg * seg))
        # sliding energy of cand windows
        sq = np.concatenate([[0.0], np.cumsum(env_c * env_c)])
        energies = np.sqrt(np.maximum(sq[len(seg):] - sq[:-len(seg)], 1e-9))[:len(corr)]
        ncc = corr / (norm_seg * energies + 1e-9)
        k = int(np.argmax(ncc))
        out.append((start / RATE, float(ncc[k]), k / RATE))
    return [o for o in out if o[1] >= threshold], out


def main():
    a = sys.argv[1:]
    window, step, threshold = 6.0, 2.0, 0.5
    for opt, attr in (('--window', 'window'), ('--step', 'step'), ('--min', 'threshold')):
        if opt in a:
            i = a.index(opt)
            v = float(a[i + 1])
            del a[i:i + 2]
            if attr == 'window':
                window = v
            elif attr == 'step':
                step = v
            else:
                threshold = v
    dump_path, cands = a[0], a[1:]
    dump, rate = read_wav(dump_path)
    dump = resample(dump, rate)
    print(f'dump: {len(dump) / RATE:.1f} s')
    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        for spec in cands:
            cand = load_candidate(spec, tmp)
            hits, every = best_offsets(dump, cand, window, step, threshold)
            print(f'== {spec} ({len(cand) / RATE:.1f} s): {len(hits)} windows match')
            for t, c, off in hits:
                print(f'   dump {t:6.1f} s  ~ candidate {off:6.1f} s  (ncc {c:.2f})')
            if not hits:
                best = max(every, key=lambda x: x[1]) if every else None
                if best:
                    print(f'   best was dump {best[0]:.1f} s ~ candidate {best[2]:.1f} s (ncc {best[1]:.2f})')


if __name__ == '__main__':
    main()
