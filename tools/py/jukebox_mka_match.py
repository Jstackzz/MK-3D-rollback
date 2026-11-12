#!/usr/bin/env python3
"""Armageddon: which shell_jukebox stream (or stream of its own bg bank) each arena's fight plays, from the audio dumps
of build/jukebox/mka-dumps.ps1 (dump-<arena>.wav in the scratch folder). Prints a table and, with --write, puts the
banks and streams into launcher/data/jukebox.json.

  python tools/py/jukebox_mka_match.py <dump folder> [--write]
"""
import json
import os
import re
import struct
import subprocess
import sys
import tempfile

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, '..', '..')
sys.path.insert(0, HERE)
import audio_match  # noqa: E402
import msxbank  # noqa: E402

RATE = audio_match.RATE
GAME = os.path.join(ROOT, 'recomp', 'mka', 'assets', 'sndsxbox')


def ima_wav(d, e):
    """the stream as an IMA ADPCM WAV ffmpeg decodes (65 samples a block: 1.5 % slower than the game, fine for a match)"""
    tag, ch, rate = msxbank.wave_format(d, e['hdr'])
    data = d[e['data']:e['data'] + e['data_size']]
    fmt = struct.pack('<HHIIHHHH', 0x11, ch, rate, rate * 36 * ch // 64, 36 * ch, 4, 2, 65)
    return b'RIFF' + struct.pack('<I', 36 + len(fmt) - 16 + len(data)) + b'WAVEfmt ' + struct.pack('<I', len(fmt)) + fmt + \
        b'data' + struct.pack('<I', len(data)) + data


def decode_fast(d, e, tmp):
    p = os.path.join(tmp, 'c.wav')
    open(p, 'wb').write(ima_wav(d, e))
    raw = subprocess.run(['ffmpeg', '-y', '-loglevel', 'error', '-i', p, '-ac', '1', '-ar', str(RATE), '-f', 'f32le', '-'],
                         capture_output=True, check=True).stdout
    return np.frombuffer(raw, dtype='<f4').astype(np.float32)


def candidates(tmp):
    """(bank, id, seconds, envelope) for every stream of 12 s or more in shell_jukebox and the bg_* banks"""
    out = []
    names = ['shell_jukebox'] + sorted(f[:-4] for f in os.listdir(GAME) if f.startswith('bg_') and f.endswith('.msx'))
    for bank in names:
        d = open(os.path.join(GAME, bank + '.msx'), 'rb').read()
        ver, ents = msxbank.read_table(d)
        for e in ents:
            f = msxbank.wave_format(d, e['hdr'])
            if not f or f[0] != 0x69:
                continue
            secs = msxbank.seconds_of(e, f)
            if secs < 12:
                continue
            out.append((bank, e['id'], secs, decode_fast(d, e, tmp)))
    return out


def main():
    folder = sys.argv[1]
    write = '--write' in sys.argv
    with tempfile.TemporaryDirectory() as tmp:
        cands = candidates(tmp)
        print(f'{len(cands)} candidate streams')
        results = {}
        for f in sorted(os.listdir(folder)):
            m = re.match(r'dump-(\d+)\.wav$', f)
            if not m:
                continue
            arena = int(m.group(1))
            dump, rate = audio_match.read_wav(os.path.join(folder, f))
            dump = audio_match.resample(dump, rate)
            # the fight: from 15 s in (boot and menus before) to the end
            fight = dump[15 * RATE:]
            best = []
            for bank, sid, secs, env in cands:
                hits, every = audio_match.best_offsets(fight, env, 6.0, 4.0, 0.0)
                score = sorted((c for _, c, _ in every), reverse=True)
                top = float(np.mean(score[:3])) if score else 0.0
                best.append((top, bank, sid, secs))
            best.sort(reverse=True)
            results[arena] = best[:3]
            print(f"arena {arena:2d}: " + ', '.join(f"{b}:{sid} ({secs:.0f} s) {top:.2f}" for top, b, sid, secs in best[:3]))
    if write:
        p = os.path.join(ROOT, 'launcher', 'data', 'jukebox.json')
        data = json.load(open(p, encoding='utf-8'))
        for s in data['games']['mka']['stages']:
            if s['id'] in results and results[s['id']] and results[s['id']][0][0] >= 0.6:
                top, bank, sid, secs = results[s['id']][0]
                s['bank'] = bank
                s['streams'] = [sid]
            else:
                s['bank'] = ''
                s['streams'] = []
        json.dump(data, open(p, 'w', encoding='utf-8'), indent=2, ensure_ascii=False)
        print('written', p)


if __name__ == '__main__':
    main()
