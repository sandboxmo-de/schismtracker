#!/usr/bin/env python3
"""Check that the effects voice pool plays an instrument the same way the
song engine does.

The script writes small IT modules (one instrument, one sample, one note on
channel 1, a note-off some rows later), renders each one with schismplay as
a song, then renders the same instrument and note through the effects voice
pool (--sfx-only --sfx-bank-song --sfx-note) with the pool's tick rate set
to the module's tempo, and compares the two at 32 bits.

    sfx-voices-equiv.py [--keep DIR] SCHISMPLAY

The modules use global volume 128, mixing volume 48 (the pool's default),
channel volume 64 and a centred channel, so the song applies no gain the
pool doesn't. All the data is generated here.
"""

import argparse
import math
import os
import struct
import subprocess
import sys
import tempfile

RATE = 44100


def env_bytes(on, nodes, sus=None, loop=None, filt=False, signed=False):
    """An IT envelope block (82 bytes). nodes are (tick, value)."""
    flags = 1 if on else 0
    lpb = lpe = slb = sle = 0
    if loop:
        flags |= 2
        lpb, lpe = loop
    if sus:
        flags |= 4
        slb, sle = sus
    if filt:
        flags |= 0x80
    out = struct.pack("<BBBBBB", flags, len(nodes), lpb, lpe, slb, sle)
    for i in range(25):
        if i < len(nodes):
            t, v = nodes[i]
            out += struct.pack("<bH" if signed else "<BH", v, t)
        else:
            out += b"\0\0\0"
    return out + b"\0"


def instrument_bytes(p):
    out = b"IMPI" + b"inst.iti".ljust(12, b"\0") + b"\0"
    out += struct.pack("<BBB", p.get("nna", 0), 0, 0)
    out += struct.pack("<H", p["fadeout"])
    out += struct.pack("<bB", p.get("pps", 0), p.get("ppc", 60))
    out += struct.pack("<BBBB", p.get("gbv", 128), 0x80 | 32, p.get("rv", 0), p.get("rp", 0))
    out += struct.pack("<HBB", 0x0214, 1, 0)
    out += p.get("name", b"test").ljust(26, b"\0")
    out += struct.pack("<BBBBH", p.get("ifc", 0), p.get("ifr", 0), 0, 0xFF, 0xFFFF)
    for n in range(120):
        out += struct.pack("<BB", n, 1)
    out += p["volenv"] + p["panenv"] + p["pitchenv"]
    out += b"\0" * 4
    assert len(out) == 554, len(out)
    return out


def sample_bytes(p, data_offset):
    flags = 1 | 2  # sample present, 16-bit
    if p.get("loop"):
        flags |= 0x10
    if p.get("sus"):
        flags |= 0x20
    if p.get("pingpong_sus"):
        flags |= 0x80
    loop = p.get("loop") or (0, 0)
    sus = p.get("sus") or (0, 0)
    out = b"IMPS" + b"smp.its".ljust(12, b"\0") + b"\0"
    out += struct.pack("<BBB", p.get("gvl", 64), flags, p.get("vol", 64))
    out += b"test sample".ljust(26, b"\0")
    out += struct.pack("<BB", 1, 32)  # signed, no default pan
    out += struct.pack("<IIIIIII", p["length"], loop[0], loop[1], p["c5speed"],
                       sus[0], sus[1], data_offset)
    out += struct.pack("<BBBB", p.get("vis", 0), p.get("vid", 0), p.get("vir", 0), p.get("vit", 0))
    assert len(out) == 80
    return out


def make_wave(length):
    """A bright, slightly inharmonic 16-bit wave, so filters and
    interpolation have something to do."""
    out = bytearray()
    for i in range(length):
        t = i / 22050.0
        v = (0.45 * math.sin(2 * math.pi * 220 * t)
             + 0.25 * math.sin(2 * math.pi * 661 * t)
             + 0.15 * math.sin(2 * math.pi * 1543 * t)
             + 0.10 * (((i * 37) % 200) / 100.0 - 1.0))
        out += struct.pack("<h", int(max(-1, min(1, v)) * 30000))
    return bytes(out)


def pattern_bytes(rows, events):
    """events: {row: (note, instrument)} on channel 1."""
    data = bytearray()
    for r in range(rows):
        if r in events:
            note, ins = events[r]
            if ins:
                data += bytes([0x81, 0x03, note, ins])
            else:
                data += bytes([0x81, 0x01, note])
        data += b"\0"
    return struct.pack("<HH4x", len(data), rows) + bytes(data)


def make_it(path, inst, smp, tempo, speed, rows, events):
    nord = 2
    header = b"IMPM" + b"sfx-voices equiv".ljust(26, b"\0")
    header += struct.pack("<BB", 4, 16)
    header += struct.pack("<HHHH", nord, 1, 1, 1)
    header += struct.pack("<HH", 0x0214, 0x0214)
    header += struct.pack("<HH", 1 | 4 | 8, 0)        # stereo, instruments, linear slides
    header += struct.pack("<BBBBBB", 128, 48, speed, tempo, 128, 0)
    header += struct.pack("<HII", 0, 0, 0)
    header += bytes([32] + [32 | 128] * 63)             # pan: ch 1 centre, the rest off
    header += bytes([64] * 64)
    assert len(header) == 192
    orders = bytes([0, 255])
    offs_size = 4 * 3
    base = 192 + nord + offs_size
    ins_off = base
    smp_off = ins_off + 554
    pat_off = smp_off + 80
    pat = pattern_bytes(rows, events)
    data_off = pat_off + len(pat)
    wave = make_wave(smp["length"])
    with open(path, "wb") as f:
        f.write(header + orders + struct.pack("<III", ins_off, smp_off, pat_off))
        f.write(instrument_bytes(inst))
        f.write(sample_bytes(smp, data_off))
        f.write(pat)
        f.write(wave)


def read_wav(path):
    with open(path, "rb") as f:
        d = f.read()
    pos = 12
    fmt = (0, 0, 0, 0, 0, 0)
    while pos < len(d):
        cid, size = d[pos:pos + 4], struct.unpack("<I", d[pos + 4:pos + 8])[0]
        body = d[pos + 8:pos + 8 + size]
        if cid == b"fmt ":
            fmt = struct.unpack("<HHIIHH", body[:16])
        elif cid == b"data":
            tag, ch, rate, _, _, bits = fmt
            if tag == 3:
                vals = struct.unpack("<%df" % (len(body) // 4), body)
            elif bits == 32:
                vals = struct.unpack("<%di" % (len(body) // 4), body)
            elif bits == 16:
                vals = struct.unpack("<%dh" % (len(body) // 2), body)
            else:
                raise ValueError("unhandled WAV format %r" % (fmt,))
            return fmt, vals
        pos += 8 + size + (size & 1)
    raise ValueError("no data chunk in " + path)


def run(cmd):
    env = dict(os.environ, SDL_AUDIODRIVER="dummy", SDL_VIDEODRIVER="dummy",
               DYLD_FALLBACK_LIBRARY_PATH="/opt/homebrew/lib")
    r = subprocess.run(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if r.returncode:
        sys.stderr.write(r.stderr.decode(errors="replace"))
        raise SystemExit("failed: %s" % " ".join(cmd))


CASES = [
    # name, tempo, speed, note-off row, interpolation, instrument changes, sample changes
    ("envelopes-cubic", 125, 6, 16, "cubic", {}, {}),
    ("envelopes-polyphase", 125, 6, 16, "polyphase", {}, {}),
    ("filter-env-resonant", 125, 6, 12, "cubic", {"filter": True, "ifc": 0x80 | 90, "ifr": 0x80 | 100}, {}),
    ("static-filter-linear", 125, 4, 20, "linear", {"ifc": 0x80 | 60, "ifr": 0x80 | 110}, {}),
    ("tempo-150", 150, 3, 30, "cubic", {}, {}),
    ("pingpong-sustain", 125, 6, 10, "cubic", {}, {"pingpong_sus": True}),
]


def build_case(path, tempo, speed, off_row, ichg, schg):
    volenv = env_bytes(True, [(0, 0), (3, 64), (8, 40), (14, 52), (30, 30), (50, 24)], sus=(2, 3))
    panenv = env_bytes(True, [(0, 0), (6, -24), (12, 24), (20, 0)], sus=(1, 2), signed=True)
    if ichg.get("filter"):
        pitchenv = env_bytes(True, [(0, 32), (10, -20), (16, 10), (40, -32)], sus=(1, 2), filt=True, signed=True)
    else:
        pitchenv = env_bytes(True, [(0, 0), (5, 6), (10, -4), (30, 2)], sus=(1, 2), signed=True)
    inst = {"fadeout": 24, "volenv": volenv, "panenv": panenv, "pitchenv": pitchenv,
            "ifc": ichg.get("ifc", 0), "ifr": ichg.get("ifr", 0)}
    smp = {"length": 30000, "c5speed": 22050, "sus": (4000, 9000), "loop": (12000, 30000),
           "vis": 20, "vid": 12, "vir": 40, "vit": 0, "vol": 60, "gvl": 64}
    smp.update(schg)
    rows = 128
    make_it(path, inst, smp, tempo, speed, rows, {0: (60, 1), off_row: (255, 0)})
    return rows


def main():
    ap = argparse.ArgumentParser(description="Compare the effects voice pool with the song engine.")
    ap.add_argument("schismplay")
    ap.add_argument("--keep", help="keep the modules and renders here")
    args = ap.parse_args()

    work = args.keep or tempfile.mkdtemp(prefix="sfx-voices-equiv-")
    os.makedirs(work, exist_ok=True)
    failed = 0

    for name, tempo, speed, off_row, interp, ichg, schg in CASES:
        mod = os.path.join(work, name + ".it")
        build_case(mod, tempo, speed, off_row, ichg, schg)
        song_wav = os.path.join(work, name + "-song.wav")
        pool_wav = os.path.join(work, name + "-pool.wav")
        off_frames = off_row * speed * (RATE * 5 * 128 // (tempo * 256))
        run([args.schismplay, "-q", "-o", song_wav, "-b", "32", "-r", str(RATE), "-i", interp,
             "--seed", "1", mod])
        run([args.schismplay, "-q", "-o", pool_wav, "-b", "32", "-r", str(RATE), "-i", interp,
             "--seed", "1", "--sfx-only", "--sfx-bank-song", "--sfx-tick-rate", str(tempo),
             "--sfx-note", "0:1:C-5@0:::%.10f" % (off_frames / RATE), mod])
        _, song = read_wav(song_wav)
        _, pool = read_wav(pool_wav)
        n = min(len(song), len(pool))
        maxdiff = max((abs(song[i] - pool[i]) for i in range(n)), default=0)
        ndiff = sum(1 for i in range(n) if song[i] != pool[i])
        # whichever render is longer must be silent past the other's end
        tail = song[n:] if len(song) > n else pool[n:]
        tail_peak = max((abs(x) for x in tail), default=0)
        peak = max((abs(x) for x in song), default=0)
        ok = maxdiff == 0 and tail_peak == 0 and peak > 0
        failed += not ok
        print("%-22s %s  frames song %d pool %d  peak %d  max diff %d (%d samples)  tail peak %d"
              % (name, "same" if ok else "DIFF", len(song) // 2, len(pool) // 2, peak,
                 maxdiff, ndiff, tail_peak))

    if not args.keep:
        print("renders in", work)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
