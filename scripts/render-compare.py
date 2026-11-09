#!/usr/bin/env python3
"""Render a set of modules with schismplay and/or schismtracker and compare
the PCM data.

Each program renders every module to a temporary WAV file. The script hashes
the sample data (not the header), writes one TSV row per program and module,
and prints how many modules came out identical across all the programs given,
and against a reference TSV if one is passed with --ref.

    render-compare.py --corpus DIR [--ext it,s3m] [--seed N] [--ref REF.tsv]
                      [--out OUT.tsv] [--jobs N] [--timeout SECS]
                      [--env NAME=VALUE ...] PROGRAM [PROGRAM ...]

A PROGRAM is the path to a schismplay or schismtracker binary, optionally
prefixed with a label and '=' (e.g. sdl3=build/schismplay). Binaries whose
name contains "schismplay" are run as `schismplay -o OUT FILE`; anything else
is run as `schismtracker --diskwrite=OUT FILE` with the dummy SDL video and
audio drivers and an empty HOME, so no user config is read.

With --seed, the seed is passed to every program, which makes songs that use
random waveforms or volume/pan swing reproducible. Without it, both programs
seed the mixer from time(), and the results only line up if time() is pinned
(for instance with an interposer passed in through --env).

The reference/output TSV columns are: file, format, frames, seconds, pcm_sha256.
"""

import argparse
import concurrent.futures
import glob
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import tempfile


def pcm_hash(path):
    """Return (format, frames, sha256 of the data chunk) for a finished WAV,
    or raise ValueError if the file is missing or wasn't finalised."""
    with open(path, "rb") as f:
        head = f.read(12)
        if len(head) < 12 or head[:4] != b"RIFF" or head[8:12] != b"WAVE":
            raise ValueError("not a WAV file")
        fmt = None
        while True:
            ch = f.read(8)
            if len(ch) < 8:
                raise ValueError("no data chunk")
            cid, size = ch[:4], struct.unpack("<I", ch[4:])[0]
            if cid == b"fmt ":
                body = f.read(size + (size & 1))
                fmt = struct.unpack("<HHIIHH", body[:16])
            elif cid == b"data":
                if fmt is None:
                    raise ValueError("data before fmt")
                if size == 0:
                    raise ValueError("data chunk not finalised")
                h = hashlib.sha256()
                left = size
                while left:
                    buf = f.read(min(left, 1 << 20))
                    if not buf:
                        raise ValueError("truncated data chunk")
                    h.update(buf)
                    left -= len(buf)
                channels, rate, bits = fmt[1], fmt[2], fmt[5]
                frames = size // (channels * bits // 8)
                return "%d/%d/%d" % (rate, bits, channels), frames, h.hexdigest()
            else:
                f.seek(size + (size & 1), 1)


def render(label, binary, song, seed, timeout, extra_env, workdir):
    out = os.path.join(workdir, "%s.wav" % label)
    home = os.path.join(workdir, "home-%s" % label)
    os.makedirs(home, exist_ok=True)
    env = dict(os.environ, HOME=home, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
    env.update(extra_env)

    if "schismplay" in os.path.basename(binary):
        cmd = [binary, "-o", out]
        if seed is not None:
            cmd += ["--seed", str(seed)]
    else:
        cmd = [binary, "--diskwrite=" + out]
        if seed is not None:
            cmd += ["--seed=%d" % seed]
    cmd.append(song)

    note = ""
    try:
        r = subprocess.run(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                           timeout=timeout)
        if r.returncode != 0:
            note = "exit %d" % r.returncode
    except subprocess.TimeoutExpired:
        note = "timeout"

    try:
        fmt, frames, digest = pcm_hash(out)
    except (OSError, ValueError) as e:
        return None, (note + "; " if note else "") + "no output (%s)" % e
    finally:
        if os.path.exists(out):
            os.remove(out)
    return (fmt, frames, digest), note


def load_tsv(path):
    rows = {}
    with open(path) as f:
        next(f)
        for line in f:
            p = line.rstrip("\n").split("\t")
            if len(p) >= 5:
                rows[p[0]] = (p[1], int(p[2]), p[4])
    return rows


def main():
    ap = argparse.ArgumentParser(description=(__doc__ or "").split("\n\n")[0])
    ap.add_argument("--corpus", required=True, help="directory of modules")
    ap.add_argument("--ext", default="it,s3m,xm,mod", help="comma-separated extensions to pick up")
    ap.add_argument("--seed", type=int, help="pass this mixer seed to every program")
    ap.add_argument("--ref", help="reference TSV to compare against")
    ap.add_argument("--out", help="write results here as TSV")
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) // 2))
    ap.add_argument("--timeout", type=int, default=900, help="seconds per render")
    ap.add_argument("--env", action="append", default=[], metavar="NAME=VALUE",
                    help="extra environment for the programs")
    ap.add_argument("programs", nargs="+", metavar="PROGRAM")
    args = ap.parse_args()

    progs = []
    for p in args.programs:
        label, sep, path = p.partition("=")
        if not sep:
            label, path = os.path.basename(p), p
        progs.append((label, os.path.abspath(path)))
    if len(set(l for l, _ in progs)) != len(progs):
        sys.exit("program labels must be unique")

    extra_env = dict(e.split("=", 1) for e in args.env)
    exts = [e.strip().lstrip(".") for e in args.ext.split(",") if e.strip()]
    songs = sorted(set(f for e in exts for f in glob.glob(os.path.join(args.corpus, "*." + e))))
    if not songs:
        sys.exit("no modules found in %s" % args.corpus)

    ref = load_tsv(args.ref) if args.ref else None
    tmp = tempfile.mkdtemp(prefix="render-compare.")

    def job(song):
        wd = tempfile.mkdtemp(dir=tmp)
        try:
            return song, [(label,) + render(label, binary, song, args.seed, args.timeout,
                                             extra_env, wd) for label, binary in progs]
        finally:
            shutil.rmtree(wd, ignore_errors=True)

    out = open(args.out, "w") if args.out else None
    if out:
        out.write("file\tprogram\tformat\tframes\tseconds\tpcm_sha256\tnote\n")

    agree = 0
    ref_match = {label: 0 for label, _ in progs}
    try:
        with concurrent.futures.ThreadPoolExecutor(args.jobs) as ex:
            for song, results in ex.map(job, songs):
                name = os.path.basename(song)
                hashes = []
                for label, res, note in results:
                    if res:
                        fmt, frames, digest = res
                        rate = int(fmt.split("/")[0])
                        hashes.append(digest)
                        if out:
                            out.write("%s\t%s\t%s\t%d\t%.2f\t%s\t%s\n" %
                                      (name, label, fmt, frames, frames / rate, digest, note))
                        if ref and name in ref and ref[name][2] == digest:
                            ref_match[label] += 1
                    else:
                        hashes.append(None)
                        if out:
                            out.write("%s\t%s\t\t\t\t\t%s\n" % (name, label, note))
                same = None not in hashes and len(set(hashes)) == 1
                agree += same
                status = "same" if same else "DIFFERENT"
                detail = "  ".join("%s:%s%s" % (label, (res[2][:12] if res else "-"),
                                                ("(" + note + ")") if note else "")
                                   for label, res, note in results)
                print("%-9s %s  %s" % (status, name, detail), flush=True)
    finally:
        if out:
            out.close()
        shutil.rmtree(tmp, ignore_errors=True)

    print()
    print("%d/%d modules identical across %s" % (agree, len(songs), ", ".join(l for l, _ in progs)))
    if ref:
        for label, _ in progs:
            print("%s: %d/%d match the reference" % (label, ref_match[label], len(songs)))
    return 0 if agree == len(songs) else 1


if __name__ == "__main__":
    sys.exit(main())
