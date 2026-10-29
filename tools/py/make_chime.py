"""Makes the launcher's match-found sound, launcher/data/sounds/match-found.wav: three rising bell-like notes (an A
major chord played upwards), about a second long, 16-bit mono at 44.1 kHz. Made here rather than taken from anywhere,
so the launcher ships no one else's sound; the same script always writes the same file.

    python tools/py/make_chime.py [out.wav]
"""
import math
import struct
import sys
import wave
from pathlib import Path

RATE = 44100
LENGTH = 1.0                       # seconds, the last 60 ms fading out
PEAK = 0.55                        # of full scale (about -5 dB): heard over a game, not startling
# (start s, frequency Hz, decay s): A5, C#6, then E6 ringing longest
NOTES = [(0.00, 880.00, 0.22), (0.09, 1108.73, 0.24), (0.18, 1318.51, 0.42)]
# a bell's partials (multiple of the note, loudness, how much faster than the note they fade)
PARTIALS = [(1.0, 1.0, 1.0), (2.0, 0.45, 1.8), (3.0, 0.2, 2.6), (4.2, 0.08, 3.5)]
ATTACK = 0.004                     # seconds: no click at each note's start
FADE = 0.06


def render() -> list[float]:
    n = int(RATE * LENGTH)
    out = [0.0] * n
    for start, freq, decay in NOTES:
        first = int(start * RATE)
        for i in range(first, n):
            t = (i - first) / RATE
            env = min(1.0, t / ATTACK)
            s = 0.0
            for mult, amp, faster in PARTIALS:
                s += amp * math.exp(-t * faster / decay) * math.sin(2 * math.pi * freq * mult * t)
            out[i] += env * s
    fade = int(FADE * RATE)
    for k in range(fade):
        out[n - 1 - k] *= k / fade
    peak = max(abs(v) for v in out)
    return [v * PEAK / peak for v in out]


def main() -> None:
    root = Path(__file__).resolve().parents[2]
    path = Path(sys.argv[1]) if len(sys.argv) > 1 else root / 'launcher' / 'data' / 'sounds' / 'match-found.wav'
    path.parent.mkdir(parents=True, exist_ok=True)
    samples = render()
    with wave.open(str(path), 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(b''.join(struct.pack('<h', round(v * 32767)) for v in samples))
    print(f'wrote {path} ({len(samples) / RATE:.2f} s, {path.stat().st_size} bytes)')


if __name__ == '__main__':
    main()
