#!/usr/bin/env python3
"""Measure the engine's peak memory per lifetime class on a set of modules.

Runs `schismplay --arena SIZE --mem-stats` on every module in a directory,
rendering each one to a throwaway WAV, and prints the peak ENGINE, SONG and
SCRATCH bytes for each file, then the max and median of each column.

    mem-stats.py --corpus DIR [--ext it,s3m,mo3] [--arena 1G] [--length 60]
                 path/to/schismplay

The peaks are what the arena handed out, block headers and alignment padding
included, so they're the region sizes a host would need for those files.
"""

import argparse
import glob
import os
import re
import statistics
import subprocess
import sys
import tempfile

CLASSES = ("engine", "song", "scratch")


def measure(schismplay, song, arena, length, timeout, tmp):
    out = os.path.join(tmp, "out.wav")
    cmd = [schismplay, "--arena", arena, "--mem-stats", "--seed", "1", "-o", out]
    if length:
        cmd += ["-l", str(length)]
    cmd.append(song)
    env = dict(os.environ, HOME=tmp, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
    try:
        r = subprocess.run(cmd, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                           universal_newlines=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return None, None, "timeout"
    finally:
        if os.path.exists(out):
            os.remove(out)

    stages = {}
    for line in r.stderr.splitlines():
        m = re.match(r"mem-stats (\w+) (\w+) peak=(\d+)", line)
        if m:
            stages.setdefault(m.group(1), {})[m.group(2)] = int(m.group(3))
    note = "" if r.returncode == 0 else "exit %d" % r.returncode
    # the last stage has the peaks for the whole run
    last = stages.get("render") or stages.get("load")
    return stages.get("load"), last, note


def main():
    ap = argparse.ArgumentParser(description=(__doc__ or "").split("\n\n")[0])
    ap.add_argument("--corpus", required=True, help="directory of modules")
    ap.add_argument("--ext", default="it,s3m,xm,mod,mo3", help="comma-separated extensions")
    ap.add_argument("--arena", default="1G", help="arena size passed to schismplay")
    ap.add_argument("--length", type=int, default=60, help="seconds to render (0 = whole song)")
    ap.add_argument("--timeout", type=int, default=900, help="seconds per file")
    ap.add_argument("schismplay")
    args = ap.parse_args()

    exts = [e.strip().lstrip(".") for e in args.ext.split(",") if e.strip()]
    songs = sorted(set(f for e in exts for f in glob.glob(os.path.join(args.corpus, "*." + e))))
    if not songs:
        sys.exit("no modules found in %s" % args.corpus)

    rows = []
    print("file\t" + "\t".join(c + "_peak" for c in CLASSES) + "\tnote")
    with tempfile.TemporaryDirectory(prefix="mem-stats.") as tmp:
        for song in songs:
            _, last, note = measure(os.path.abspath(args.schismplay), song, args.arena,
                                       args.length, args.timeout, tmp)
            name = os.path.basename(song)
            if not last:
                print("%s\t\t\t\t%s" % (name, note or "no stats"), flush=True)
                continue
            vals = [last.get(c, 0) for c in CLASSES]
            rows.append(vals)
            print("%s\t%s\t%s" % (name, "\t".join(str(v) for v in vals), note), flush=True)

    if rows:
        print("max\t" + "\t".join(str(max(r[i] for r in rows)) for i in range(len(CLASSES))))
        print("median\t" + "\t".join("%d" % statistics.median(r[i] for r in rows)
                                     for i in range(len(CLASSES))))
    print("%d of %d files measured" % (len(rows), len(songs)))
    return 0 if len(rows) == len(songs) else 1


if __name__ == "__main__":
    sys.exit(main())
