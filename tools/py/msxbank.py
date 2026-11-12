#!/usr/bin/env python3
"""Midway Xbox sound banks (the games' sndsxbox\\<bank>.mbx + .msx): list the streams of a .msx, extract one as a WAV,
or write a bank with streams replaced by a re-encoded track (ffmpeg on PATH decodes the track), the jukebox's
substitute.

  python tools/py/msxbank.py list <bank.msx>
  python tools/py/msxbank.py extract <bank.msx> <id> <out.wav>          # 16-bit PCM WAV, decoded as the games play it
  python tools/py/msxbank.py replace <bank.msx> <id>[,<id>...] <track> <out.msx> [--seconds N]

Format (research 2026-11-11): every stream is Xbox ADPCM (WAVE_FORMAT_XBOX_ADPCM 0x69): IMA ADPCM in blocks of
36 bytes per channel, a 4-byte header per channel (the block's first sample as int16, the step index, 0) then 8 groups
of 4 bytes per channel holding 8 nibbles each; a block plays its header sample and the first 63 nibbles (the 64th
nibble is the next block's header sample, encoded again). The runtime's decoder (xdk/dsound.cpp) does the same.
  version 1 (Deadly Alliance): [0x04] stream count, [0x10] file size, 12-byte entries from 0x18 (id, offset, size);
    each stream is a complete RIFF WAVE (a 20-byte fmt chunk and a data chunk) at its offset
  version 2 (Deception, Armageddon): [0x04] stream count plus one, [0x14] the resident block's size (the sounds the
    game loads whole at boot), [0x20] file size, 24-byte entries from 0x34 (id, data offset, data size, offset of the
    stream's 40-byte RIFF/fmt header inside the file header, 40, resident flag: its data offset counts from the
    resident block's start); the file header (the table and every sound's RIFF header) ends on a 0x800 boundary, the
    resident block follows it (0x800 in small banks, 0x1800 in a fighter's), the streamed data after; the data holds
    the blocks alone. A resident sound cannot grow (the game does not load a bigger one at all): the launcher makes a
    replaced one a streamed sound (out of the resident block, its flag cleared, and marked streamed in the .mbx: 36-byte
    sound records from [0x10], +8 the id, +32 1 when streamed); this tool only replaces streamed ones in place
The .mbx (the bank's descriptors) is not changed: a replaced stream keeps its format (rate, channels) and gets its
new length in the table. The launcher does the same in C++ (launcher/src/Jukebox.cpp); this is the reference and the
quick way to try a bank by hand.
"""
import os
import struct
import subprocess
import sys
import tempfile

K_INDEX = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8]
K_STEP = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
          107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
          876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428,
          4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385,
          24623, 27086, 29794, 32767]


def read_table(d):
    ver = struct.unpack_from('<I', d, 0)[0]
    ents = []
    if ver == 1:
        n = struct.unpack_from('<I', d, 4)[0]
        for i in range(n):
            id_, off, size = struct.unpack_from('<III', d, 0x18 + 12 * i)
            if size == 0:
                break
            ents.append({'id': id_ & 0xFFFF, 'off': off, 'size': size, 'resident': False, 'index': i, 'hdr': off,
                         'data': off + 0x30, 'data_size': size - 0x30})
        return ver, ents
    if ver == 2:
        n = struct.unpack_from('<I', d, 4)[0] - 1
        resident_at = header_end(d) if struct.unpack_from('<I', d, 0x14)[0] else 0
        for i in range(n):
            id_, off, size, hdr, hdr_len, res = struct.unpack_from('<IIIIII', d, 0x34 + 24 * i)
            data = off + resident_at if res else off
            ents.append({'id': id_ & 0xFFFF, 'off': off, 'size': size, 'resident': bool(res), 'index': i, 'hdr': hdr,
                         'data': data, 'data_size': size})
        return ver, ents
    raise SystemExit(f'unknown msx version {ver}')


def header_end(d):
    """version 2: where the file header ends (the table and every sound's RIFF header), on its 0x800 boundary"""
    n = struct.unpack_from('<I', d, 4)[0] - 1
    end = 0x34 + 24 * n
    for i in range(n):
        end = max(end, struct.unpack_from('<I', d, 0x34 + 24 * i + 12)[0] + 40)
    return (end + 0x7FF) // 0x800 * 0x800


def wave_format(d, off):
    """(tag, channels, rate) of the RIFF/fmt header at off, else None."""
    if d[off:off + 4] != b'RIFF' or d[off + 8:off + 16] != b'WAVEfmt ':
        return None
    tag, ch, rate = struct.unpack_from('<HHI', d, off + 20)
    return tag, ch, rate


def seconds_of(e, fmt):
    tag, ch, rate = fmt
    if tag == 0x69:
        return e['data_size'] // (36 * ch) * 64 / rate
    return 0.0


def describe(d, e):
    f = wave_format(d, e['hdr'])
    if not f:
        return 'no RIFF header'
    return f'{f[0]:#x} {f[1]}ch {f[2]} Hz {seconds_of(e, f):7.1f} s'


def cmd_list(path):
    d = open(path, 'rb').read()
    ver, ents = read_table(d)
    print(f'{os.path.basename(path)}: version {ver}, {len(ents)} streams, {len(d)} bytes')
    for e in ents:
        print(f"  id {e['id']:3d}  data {e['data']:#9x}  size {e['data_size']:#9x}  "
              f"{'resident ' if e['resident'] else ''}{describe(d, e)}")


def decode(data, ch):
    """Xbox ADPCM blocks to interleaved int16 samples, 64 frames a block as the games play them."""
    out = []
    blocks = len(data) // (36 * ch)
    for b in range(blocks):
        base = b * 36 * ch
        frames = [[0] * ch for _ in range(64)]
        for c in range(ch):
            pred = struct.unpack_from('<h', data, base + c * 4)[0]
            idx = min(data[base + c * 4 + 2], 88)
            frames[0][c] = pred
            dpos = base + ch * 4
            n_out = 1
            for k in range(8):
                for j in range(8):
                    n = (data[dpos + (k * ch + c) * 4 + j // 2] >> ((j & 1) * 4)) & 15
                    step = K_STEP[idx]
                    diff = step >> 3
                    if n & 1:
                        diff += step >> 2
                    if n & 2:
                        diff += step >> 1
                    if n & 4:
                        diff += step
                    pred = max(-32768, min(32767, pred - diff if n & 8 else pred + diff))
                    idx = max(0, min(88, idx + K_INDEX[n]))
                    if n_out < 64:
                        frames[n_out][c] = pred
                    n_out += 1
        for fr in frames:
            out.extend(fr)
    return out


def encode(samples, ch):
    """Interleaved int16 samples to Xbox ADPCM blocks (whole blocks of 64 frames; the rest is dropped)."""
    frames = len(samples) // ch
    blocks = frames // 64
    out = bytearray()
    pred = [0] * ch
    idx = [0] * ch
    for b in range(blocks):
        base = b * 64 * ch
        # the header: the block's first sample, played as is, and the step index to go on with
        for c in range(ch):
            pred[c] = samples[base + c]
            out += struct.pack('<hBB', pred[c], idx[c], 0)
        nibbles = [[0] * 64 for _ in range(ch)]
        for c in range(ch):
            p = pred[c]
            i = idx[c]
            for s in range(64):
                at = base + (s + 1) * ch + c  # the 64th is the next block's first sample
                sample = samples[at] if at < len(samples) else p
                step = K_STEP[i]
                diff = sample - p
                n = 0
                if diff < 0:
                    n = 8
                    diff = -diff
                if diff >= step:
                    n |= 4
                    diff -= step
                if diff >= step >> 1:
                    n |= 2
                    diff -= step >> 1
                if diff >= step >> 2:
                    n |= 1
                d = step >> 3
                if n & 1:
                    d += step >> 2
                if n & 2:
                    d += step >> 1
                if n & 4:
                    d += step
                p = max(-32768, min(32767, p - d if n & 8 else p + d))
                i = max(0, min(88, i + K_INDEX[n]))
                nibbles[c][s] = n
            idx[c] = i
        for k in range(8):
            for c in range(ch):
                for j in range(0, 8, 2):
                    out.append(nibbles[c][k * 8 + j] | nibbles[c][k * 8 + j + 1] << 4)
    return bytes(out)


def cmd_extract(path, id_, out):
    d = open(path, 'rb').read()
    ver, ents = read_table(d)
    e = next((x for x in ents if x['id'] == int(id_)), None)
    if not e:
        raise SystemExit(f'no stream {id_}')
    f = wave_format(d, e['hdr'])
    if not f or f[0] != 0x69:
        raise SystemExit('not an Xbox ADPCM stream')
    tag, ch, rate = f
    pcm = decode(d[e['data']:e['data'] + e['data_size']], ch)
    body = struct.pack('<%dh' % len(pcm), *pcm)
    fmt = struct.pack('<HHIIHH', 1, ch, rate, rate * ch * 2, ch * 2, 16)
    wav = b'RIFF' + struct.pack('<I', 36 + len(body)) + b'WAVEfmt ' + struct.pack('<I', 16) + fmt + b'data' + \
        struct.pack('<I', len(body)) + body
    open(out, 'wb').write(wav)
    print(f'wrote {out}: {describe(d, e)}')


def decode_track(track, ch, rate, seconds):
    """The track as interleaved int16 PCM at the stream's rate and channels (ffmpeg)."""
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, 'pcm.raw')
        args = ['ffmpeg', '-y', '-loglevel', 'error', '-i', track]
        if seconds:
            args += ['-t', str(seconds)]
        args += ['-ar', str(rate), '-ac', str(ch), '-f', 's16le', '-c:a', 'pcm_s16le', out]
        subprocess.run(args, check=True)
        raw = open(out, 'rb').read()
    return list(struct.unpack('<%dh' % (len(raw) // 2), raw[:len(raw) // 2 * 2]))


def xbox_header(ch, rate, data_size, whole_riff):
    """The 40-byte RIFF/WAVE/fmt header the banks keep: RIFF size counts the header and data (version 1 adds a data
    chunk header: the games' tool put the whole file size in the RIFF size there)."""
    fmt = struct.pack('<HHIIHHHH', 0x69, ch, rate, rate * 36 * ch // 64, 36 * ch, 4, 2, 64)
    riff_size = 0x30 + data_size if whole_riff else 0x28 + data_size
    h = b'RIFF' + struct.pack('<I', riff_size) + b'WAVEfmt ' + struct.pack('<I', 20) + fmt
    if whole_riff:
        h += b'data' + struct.pack('<I', data_size)
    return h


def cmd_replace(path, ids, track, out, seconds=None):
    d = open(path, 'rb').read()
    ver, ents = read_table(d)
    ids = [int(x) for x in ids.split(',')]
    targets = [e for e in ents if e['id'] in ids]
    if len(targets) != len(ids):
        raise SystemExit(f'streams {ids} not all in the bank')
    encoded = {}  # (channels, rate) -> blocks
    for e in targets:
        f = wave_format(d, e['hdr'])
        if not f or f[0] != 0x69:
            raise SystemExit(f"stream {e['id']} is not Xbox ADPCM")
        key = (f[1], f[2])
        if key not in encoded:
            encoded[key] = encode(decode_track(track, f[1], f[2], seconds), f[1])
    align = 0x800
    if ver == 1:
        header_end = min(e['off'] for e in ents)
        outd = bytearray(d[:header_end])
    else:
        # the header stays; the resident block (the sounds kept in memory, offsets from its start) is laid out again
        # in case one of them is replaced, and its size at [0x14] follows
        hdr_end = header_end(d)
        outd = bytearray(d[:hdr_end])
        rpos = 0
        for e in ents:
            if not e['resident']:
                continue
            rpos = (rpos + 3) // 4 * 4
            if e in targets:
                f = wave_format(d, e['hdr'])
                blob = encoded[(f[1], f[2])]
                outd[e['hdr']:e['hdr'] + 40] = xbox_header(f[1], f[2], len(blob), False)
                e['size'] = len(blob)
            else:
                blob = d[e['data']:e['data'] + e['data_size']]
            outd += b'\0' * (hdr_end + rpos - len(outd))
            outd += blob
            e['off'] = rpos
            rpos += len(blob)
        if any(e['resident'] for e in ents):
            struct.pack_into('<I', outd, 0x14, len(outd) - hdr_end)
    pos = len(outd)
    for e in ents:
        if e['resident']:
            continue
        pos = (pos + align - 1) // align * align
        outd += b'\0' * (pos - len(outd))
        if e in targets:
            f = wave_format(d, e['hdr'])
            blocks = encoded[(f[1], f[2])]
            if ver == 1:
                blob = xbox_header(f[1], f[2], len(blocks), True) + blocks
                e['off'], e['size'] = pos, len(blob)
            else:
                blob = blocks
                e['off'], e['size'] = pos, len(blob)
                outd[e['hdr']:e['hdr'] + 40] = xbox_header(f[1], f[2], len(blocks), False)
        else:
            blob = d[e['off']:e['off'] + e['size']]
            e['off'] = pos
        outd += blob
        pos = len(outd)
    for e in ents:
        if ver == 1:
            struct.pack_into('<II', outd, 0x18 + 12 * e['index'] + 4, e['off'], e['size'])
        else:
            struct.pack_into('<II', outd, 0x34 + 24 * e['index'] + 4, e['off'], e['size'])
    struct.pack_into('<I', outd, 0x10 if ver == 1 else 0x20, len(outd))
    open(out, 'wb').write(outd)
    print(f'wrote {out}: {len(outd)} bytes, streams {ids} replaced by {os.path.basename(track)}')


if __name__ == '__main__':
    a = sys.argv[1:]
    if not a:
        print(__doc__)
        sys.exit(1)
    if a[0] == 'list':
        cmd_list(a[1])
    elif a[0] == 'extract':
        cmd_extract(a[1], a[2], a[3])
    elif a[0] == 'replace':
        secs = None
        if '--seconds' in a:
            i = a.index('--seconds')
            secs = float(a[i + 1])
            del a[i:i + 2]
        cmd_replace(a[1], a[2], a[3], a[4], secs)
    else:
        print(__doc__)
        sys.exit(1)
