#!/usr/bin/env python3
"""Check snapshot saves (SIGUSR2 with NETHACK_SNAPSHOT, src/save.c).

  no-influence  a game recorded with its live feed, asked for a snapshot
                every so many keys (while it runs and while it waits),
                replays and verifies without any, and its state log
                (NH_STATELOG: after every key, the draws from the core and
                display RNGs and a hash of the game's state) is the same,
                key for key: a snapshot draws no number and changes
                nothing played
  restore       a snapshot taken after visiting a few levels restores in
                another playground, and the same keys, none of which
                changes level, give both games the same draws and state
                hashes, key for key: the save holds the dice
  sfctool       the snapshot, converted to sfctool's export format and
                back, restores and plays on the same, key for key

Usage: snapshottest.py [-k KEYS] [--seeds N] [playground]
"""
import argparse
import gzip
import os
import random
import shutil
import signal
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import feedgame as nhgame  # noqa: E402
from feedtest import capture, replay, same_state  # noqa: E402

NAME = "snapshot"
# keys that play turns on the level the hero is on
STAY_KEYS = ["h", "j", "k", "l", "y", "u", "b", "n", "s", "s", "5s",
             "Fh", "Fj", "Fl", "Fk", "H", "L", "J", "K"]


def settle(g, rounds=20):
    """answer what stands between the game and its next command"""
    for _ in range(rounds):
        t = g.tail
        if "Die?" in t:
            g.tail = ""
            g.send("n")
        elif ("--More--" in t or "[ynq]" in t or "[yn]" in t
              or "(end)" in t):
            g.tail = ""
            g.send("\033")
        else:
            break


def snapshot(g, path, timeout=20):
    """ask for a snapshot and wait for it: the seconds it took"""
    if os.path.exists(path):
        os.unlink(path)
    settle(g)
    t0 = time.time()
    g.signal(signal.SIGUSR2)
    while not os.path.exists(path) and time.time() - t0 < timeout:
        g.drain(0.05)
        settle(g)
    if not os.path.exists(path):
        raise RuntimeError("no snapshot written; screen: "
                           + g.screen()[-400:])
    return time.time() - t0


def statelog(path):
    """per key: (key, turn, core draws, display draws, hash)"""
    rows = []
    with open(path) as f:
        for line in f:
            _, key, moves, core, disp, _, h = line.split()
            rows.append((int(key), int(moves), int(core), int(disp), h))
    return rows


def steps(rows):
    """per key after the first: its turn, its draws, its state hash"""
    return [(r[1], r[2] - p[2], r[3] - p[3], r[4])
            for p, r in zip(rows, rows[1:])]


def no_influence(root, seed, nkeys, n):
    """recorded with snapshots, replayed without: the same state log"""
    w = os.path.join(root, "noinf-%d" % n)
    pg = os.path.join(w, "pg")
    nhgame.copy_playground(pg)
    rec = os.path.join(w, "game.rec")
    snap = os.path.join(w, "snap")
    log_on = os.path.join(w, "state-snap.log")
    g = nhgame.Game(pg, NAME, seed, mode="wizard", record=rec,
                    extra_env=dict(NETHACK_SNAPSHOT=snap,
                                   NH_STATELOG=log_on))
    t = capture(g.feed_fd, os.path.join(w, "feed.ndjson"))
    g.drain(1.0)
    rng = random.Random(n)
    taken = [0]

    def on_key(k):
        if k % 41 == 7 and g.alive:
            # mid-command, and then while the game waits for a key
            g.send("20s", settle=0.0)
            g.signal(signal.SIGUSR2)
            g.drain(1.5)
            if os.path.exists(snap):
                taken[0] += 1
                os.unlink(snap)
    nhgame.play(g, rng, nkeys, mode="wizard", on_key=on_key)
    g.finish()
    g.close()
    t.join(10)
    log_off = os.path.join(w, "state-plain.log")
    code, tail = replay(pg, rec, statelog=log_off)
    same, why = same_state(log_on, log_off, prefix=True)
    good = code == 0 and same and taken[0] >= 2
    print("no-influence %-14s %s  %d snapshots, replay exit %d, state log"
          " %s %s; %s" % (seed, "ok  " if good else "FAIL", taken[0], code,
                          "same over" if same else "DIFFERS:", why,
                          " / ".join(tail)))
    return good


def new_game(pg, seed, env):
    nhgame.copy_playground(pg)
    g = nhgame.Game(pg, NAME, seed, mode="wizard", extra_env=env)
    g.read_feed()
    if not g.first_command():
        raise RuntimeError("no first command: " + g.screen()[-400:])
    return g


def restore(root, seed, nkeys, n):
    """the same keys after a snapshot and after its restore: the same
    draws and states"""
    w = os.path.join(root, "restore-%d" % n)
    os.makedirs(w)
    snap = os.path.join(w, "snap")
    log_a = os.path.join(w, "state-a.log")
    g = new_game(os.path.join(w, "pga"), seed,
                 dict(NETHACK_SNAPSHOT=snap, NH_STATELOG=log_a))
    for level in ("3", "5", "4"):
        g.send("\026")  # ^V: level teleport (wizard mode)
        g.send(level + "\r")
        settle(g)
        g.send("10s")
        settle(g)
    secs = snapshot(g, snap)
    g.drain(0.3)
    base = len(statelog(log_a))
    rng = random.Random(1000 + n)
    keys = [rng.choice(STAY_KEYS) for _ in range(nkeys)]
    for k in keys:
        g.send(k, settle=0.5)
        settle(g)
    g.drain(0.5)
    after_a = statelog(log_a)[base:]
    g.signal(signal.SIGKILL)
    nhgame.close_game(g)

    restored, b = play_restored(os.path.join(w, "pgb"), seed, snap, keys)
    a = steps(after_a)
    first = next((i for i, (x, y) in enumerate(zip(a, b)) if x != y), None)
    draws = sum(x[1] for x in a)
    good = (restored and a == b and len(a) >= nkeys // 2 and draws > 0)
    print("restore      %-14s %s  snapshot of %d bytes in %.2f s, %d keys"
          " after it with %d core draws %s"
          % (seed, "ok  " if good else "FAIL", os.path.getsize(snap), secs,
             len(a), draws,
             "the same in the restored game" if a == b and restored
             else "DIFFER at key %s: %s vs %s" % (
                 first, a[first] if first is not None and first < len(a)
                 else None,
                 b[first] if first is not None and first < len(b) else None)
             if restored else "(the snapshot did not restore)"))
    return good, snap, keys, a


def play_restored(pg, seed, save, keys):
    """restore save in a fresh playground pg and send keys: whether it
    restored, and its steps (see steps()) after it"""
    log = pg + "-state.log"
    nhgame.copy_playground(pg)
    shutil.copy(save, os.path.join(pg, "save", "%dwizard" % os.getuid()))
    g = nhgame.Game(pg, NAME, seed, mode="wizard",
                    extra_env=dict(NH_STATELOG=log))
    g.read_feed()
    restored = g.first_command()
    g.drain(0.3)
    base = len(statelog(log)) if os.path.exists(log) else 0
    for k in keys:
        g.send(k, settle=0.5)
        settle(g)
    g.drain(0.5)
    after = statelog(log)[base:] if os.path.exists(log) else []
    g.signal(signal.SIGKILL)
    nhgame.close_game(g)
    return restored, steps(after)


def sfctool_round_trip(root, seed, snap, keys, want):
    """exported and converted back, the snapshot restores and plays on as
    it does (the export carries the dice too)"""
    sfctool = os.path.join(nhgame.NH, "util", "sfctool")
    if not os.path.exists(sfctool):
        print("sfctool      skipped (util/sfctool not built)")
        return True
    w = os.path.join(root, "sfctool")
    os.makedirs(w)
    src = os.path.join(w, "game")
    shutil.copy(snap, src)
    original = open(src, "rb").read()

    def run(*args):
        r = subprocess.run([sfctool] + list(args), capture_output=True,
                           text=True)
        return r.returncode, (r.stdout + r.stderr).strip()

    def plain(path):
        # (sfctool compresses what it writes when the build compresses
        # saves)
        if os.path.exists(path + ".gz"):
            with gzip.open(path + ".gz", "rb") as f:
                data = f.read()
            os.unlink(path + ".gz")
            with open(path, "wb") as f:
                f.write(data)
        return path
    c1, out1 = run("-c", src)
    plain(src + ".exportascii")
    plain(src)
    os.unlink(src)
    c2, out2 = run("-u", src)
    back = plain(src)
    restored, got = (False, None)
    if c1 == 0 and c2 == 0 and os.path.exists(back):
        restored, got = play_restored(os.path.join(w, "pg"), seed, back,
                                      keys)
    good = restored and got == want
    print("sfctool      %s  exported and converted back (%d bytes, was %d):"
          " %s" % ("ok  " if good else "FAIL",
                   os.path.getsize(back) if os.path.exists(back) else 0,
                   len(original),
                   "restores and plays on the same" if good
                   else "restored %s, same steps %s (%s / %s)"
                   % (restored, got == want, out1[-200:], out2[-200:])))
    return good


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-k", type=int, default=400, help="keys per game")
    ap.add_argument("--seeds", type=int, default=2)
    ap.add_argument("playground", nargs="?", default=nhgame.PLAYGROUND)
    args = ap.parse_args()
    nhgame.PLAYGROUND = os.path.abspath(args.playground)
    binary = os.stat(os.path.join(nhgame.PLAYGROUND, "nethack"))
    if binary.st_mode & 0o6000:
        ap.error("run with an unprivileged playground binary")
    root = tempfile.mkdtemp(prefix="snapshottest-")
    ok = True
    for n in range(args.seeds):
        seed = "snapshot-%d" % n
        ok &= no_influence(root, seed, args.k, n)
        good, snap, keys, steps_a = restore(root, seed,
                                            max(60, args.k // 3), n)
        ok &= good
    ok &= sfctool_round_trip(root, seed, snap, keys, steps_a)
    if ok:
        shutil.rmtree(root, ignore_errors=True)
    else:
        print("kept", root)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
