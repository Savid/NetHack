#!/usr/bin/env python3
"""Layout dumps (nethack --layouts) and the keyframes written against them.

  repeat      --layouts twice for a seed gives the same bytes
  hashes      --layout-hashes gives each level's layout and the end hash
              as --layouts does
  ignored     an options file, NETHACKOPTIONS, ROGUEOPTS, and a window type
              (-w, or in sysconf) leave the dump as it was
  sysconf     sysconf's SEED is the dump's seed, whatever standard input
              says; an invalid one is refused
  refusals    -D, an invalid seed on standard input, no seed at all, and
              standard output closed by its reader: exit 1, no output, no
              scratch directory left
  files       --layouts FILE: a file it makes, and an existing plain file,
              end up mode 0600 holding the dump; a directory is refused
  sandbox     in a copy of the playground, writable then read-only, with
              a private TMPDIR: standard output is exactly the dump, the
              playground is unchanged, TMPDIR is left empty, and it takes
              well under 10 s (a fraction of a second, unloaded)
  busy        with a game waiting for a key in the same playground, the
              dump doesn't block and leaves the game's files alone; the
              game then plays on and ends, and its xlogfile entry has the
              dump's datahash
  rebuild     seeded games started as a race server starts them (the seed
              in NETHACKOPTIONS), with the feed and NH_FEEDCHECK, taken
              down two levels, saved, restored and back up one, so that a
              level is revisited from a save; the seed dumped from
              standard input.  Folding each feed: every keyframe's layout
              is the dump's for its level, a keyframe on the level the
              game was already on equals the state folded before it, and
              the folded terrain, map, screen and view hash to every "chk"
              line
  nullbase    a seeded game in wizard mode, level-teleporting: every
              keyframe's layout is null, and the feed folds the same way

Usage: layouttest.py [--seeds N] PLAYGROUND

PLAYGROUND is an installed playground whose sysconf has WIZARDS=* and
EXPLORERS=* and no SEED or RECORDFILE; run it as the playground's owner.
The ignored and sysconf checks add a line to that sysconf for a moment,
and always put the file back.  Nothing a dump or a game writes is printed.
"""
import argparse
import collections
import json
import os
import random
import re
import shutil
import signal
import stat
import subprocess
import sys
import tempfile
import time
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import feedgame  # noqa: E402

COLNO, ROWNO = 80, 21
CELLS = COLNO * ROWNO
CODES = ("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
         "0123456789+/")
# terrain types and door states (include/rm.h), and the moves
SDOOR, SCORR, DOOR = 14, 15, 23
D_BROKEN, D_LOCKED = 0x01, 0x08
STEPS = {(-1, 0): "h", (0, 1): "j", (0, -1): "k", (1, 0): "l",
         (-1, -1): "y", (1, -1): "u", (-1, 1): "b", (1, 1): "n"}
RACE_OPTIONS = ("color,!legacy,!news,!splash_screen,!tutorial,!autopickup,"
                "!tips,!autodescribe")
SEEDS = ["00042", "layout test three", "layouttest", "7", "layout five"]


def fnv(data, h=0xcbf29ce484222325):
    for b in data:
        h = ((h ^ b) * 0x100000001b3) & 0xffffffffffffffff
    return "%016x" % h


# ---------- dumps ----------

def dump(pg, form, seed, work, extra_env=None, args=(), tmpdir=None,
         closed=False, target="-"):
    """-> (exit status, stdout, stderr, seconds, what TMPDIR was left
    holding); closed: standard output is a pipe nobody reads; target: the
    file to write, from work"""
    tmpdir = tmpdir or tempfile.mkdtemp(prefix="tmp-", dir=work)
    home = os.path.join(work, "home")
    os.makedirs(home, exist_ok=True)
    env = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "HOME": home,
           "NETHACKDIR": pg, "TMPDIR": tmpdir}
    env.update(extra_env or {})
    out = subprocess.PIPE
    if closed:
        r, out = os.pipe()
        os.close(r)
    t = time.time()
    p = subprocess.run([os.path.join(pg, "nethack"), form, target]
                       + list(args),
                       input=seed, stdout=out, stderr=subprocess.PIPE,
                       env=env, cwd=work, timeout=120)
    if closed:
        os.close(out)
    return (p.returncode, p.stdout or b"", p.stderr, time.time() - t,
            os.listdir(tmpdir))


def layouts(pg, seed, work):
    """the full dump of seed, parsed: {(dn, dl): level line}, end line"""
    rc, out, err, _, _ = dump(pg, "--layouts", seed.encode() + b"\n", work)
    assert rc == 0, "--layouts failed: %s" % err.decode()[:200]
    lines = [json.loads(x) for x in out.decode().splitlines()]
    return ({(x["dn"], x["dl"]): x for x in lines if x["k"] == "level"},
            lines[-1])


def check_repeat_hashes(pg, work):
    ok = True
    for seed in SEEDS[:3]:
        a = dump(pg, "--layouts", seed.encode() + b"\n", work)
        b = dump(pg, "--layouts", seed.encode() + b"\n", work)
        h = dump(pg, "--layout-hashes", seed.encode() + b"\n", work)
        good = a[0] == b[0] == h[0] == 0 and a[1] == b[1] and bool(a[1])
        full = [json.loads(x) for x in a[1].decode().splitlines()]
        brief = [json.loads(x) for x in h[1].decode().splitlines()]
        good &= (full[-1] == brief[-1]
                 and fnv(a[1][:a[1].rindex(b'{"k":"end"')])
                 == full[-1]["hash"]
                 and [(x["dn"], x["dl"], x["layout"]) for x in full
                      if x["k"] == "level"]
                 == [(x["dn"], x["dl"], x["layout"]) for x in brief
                     if x["k"] == "level"]
                 and brief[0]["form"] == "hashes"
                 and "character" not in brief[0]
                 and {x["k"] for x in brief} == {"hdr", "level", "end"})
        print("repeat/hash  %-18r %s  %d levels" %
              (seed, "ok  " if good else "FAIL", full[-1].get("levels", 0)))
        ok &= good
    return ok


def check_ignored(pg, work):
    seed = b"layouttest\n"
    base = dump(pg, "--layouts", seed, work)
    home = os.path.join(work, "home")
    with open(os.path.join(home, ".nethackrc"), "w") as f:
        f.write("OPTIONS=seed:elsewhere,role:Wizard,race:elf,!color\n")
    try:
        other = dump(pg, "--layouts", seed, work, extra_env={
            "NETHACKOPTIONS": "seed:other,role:Priest,fruit:durian",
            "ROGUEOPTS": "name=x"})
    finally:
        os.remove(os.path.join(home, ".nethackrc"))
    tty = dump(pg, "--layouts", seed, work, args=["-wtty"])
    with SysconfLine(pg, "OPTIONS=windowtype:tty"):
        tty2 = dump(pg, "--layouts", seed, work)
    good = (base[0] == other[0] == tty[0] == tty2[0] == 0
            and base[1] == other[1] == tty[1] == tty2[1])
    print("ignored      %s  options file, NETHACKOPTIONS, ROGUEOPTS, -wtty,"
          " sysconf windowtype" % ("ok  " if good else "FAIL"))
    return good


class SysconfLine:
    """the playground's sysconf with another line, for a moment"""

    def __init__(self, pg, line):
        self.path = os.path.join(pg, "sysconf")
        self.line = line

    def __enter__(self):
        with open(self.path, "rb") as f:
            self.saved = f.read()
        with open(self.path, "ab") as f:
            f.write(b"\n" + self.line.encode() + b"\n")

    def __exit__(self, *exc):
        with open(self.path, "wb") as f:
            f.write(self.saved)


def check_sysconf(pg, work):
    want = dump(pg, "--layout-hashes", b"server seed\n", work)
    with SysconfLine(pg, "SEED=server seed"):
        got = dump(pg, "--layout-hashes", b"something else\n", work)
    with SysconfLine(pg, "SEED=" + "x" * 100):
        bad = dump(pg, "--layout-hashes", b"layouttest\n", work)
    good = (want[0] == got[0] == 0 and want[1] == got[1]
            and bad[0] == 1 and not bad[1] and not bad[4]
            and b"SEED" in bad[2])
    print("sysconf      %s  SEED used, standard input ignored; invalid"
          " refused" % ("ok  " if good else "FAIL"))
    return good


def check_refusals(pg, work):
    ok = True
    for what, seed, args, closed in (
            ("-D", b"layouttest\n", ["-D"], False),
            ("empty line", b"\n", [], False),
            ("too long", b"y" * 100 + b"\n", [], False),
            ("no input", b"", [], False),
            ("no reader", b"layouttest\n", [], True)):
        rc, out, err, _, left = dump(pg, "--layouts", seed, work, args=args,
                                     closed=closed)
        good = rc == 1 and not out and not left and err.startswith(
            b"nethack: ")
        print("refusal      %-10s %s" % (what, "ok  " if good else "FAIL"))
        ok &= good
    return ok


def check_files(pg, work):
    """--layouts FILE: a file it makes, and an existing plain file, end up
    private and holding the dump; a directory is refused, untouched"""
    want = dump(pg, "--layouts", b"layouttest\n", work)[1]
    made = os.path.join(work, "made.jsonl")
    old = os.path.join(work, "old.jsonl")
    with open(old, "w") as f:
        f.write("old")
    os.chmod(old, 0o644)
    folder = os.path.join(work, "folder")
    os.mkdir(folder)
    good = True
    for path in (made, old):
        rc, _, _, _, left = dump(pg, "--layouts", b"layouttest\n", work,
                                 target=os.path.basename(path))
        with open(path, "rb") as f:
            good &= (rc == 0 and not left and f.read() == want
                     and stat.S_IMODE(os.stat(path).st_mode) == 0o600)
    rc, _, err, _, left = dump(pg, "--layouts", b"layouttest\n", work,
                               target="folder")
    good &= (rc == 1 and not left and not os.listdir(folder)
             and err.startswith(b"nethack: "))
    print("files        %s  made and existing files private, holding the"
          " dump; a directory refused" % ("ok  " if good else "FAIL"))
    return good


def snapshot(d):
    """every file under d: (size, mtime, contents)"""
    files = {}
    for root, _, names in os.walk(d):
        for n in names:
            p = os.path.join(root, n)
            st = os.lstat(p)
            with open(p, "rb") as f:
                files[os.path.relpath(p, d)] = (st.st_size, st.st_mtime_ns,
                                                f.read())
    return files


def check_sandbox(pg, work):
    ro = os.path.join(work, "ro")
    shutil.copytree(pg, ro, symlinks=False)
    before = snapshot(ro)
    want = dump(ro, "--layout-hashes", b"layouttest\n", work)
    writable = want[0] == 0 and not want[4] and snapshot(ro) == before
    for root, _, names in os.walk(ro):
        for n in names:
            os.chmod(os.path.join(root, n), 0o444 | (
                os.stat(os.path.join(root, n)).st_mode & 0o111))
        os.chmod(root, 0o555)
    before = snapshot(ro)
    try:
        got = dump(ro, "--layout-hashes", b"layouttest\n", work)
        after = snapshot(ro)
    finally:
        for root, _, _ in os.walk(ro):
            os.chmod(root, 0o755)
    good = (writable and got[0] == 0 and got[1] == want[1] and not got[4]
            and before == after and got[3] <= 10.0)
    print("sandbox      %s  writable and read-only playgrounds unchanged,"
          " %.2f s" % ("ok  " if good else "FAIL", got[3]))
    return good


def check_busy(pg, work):
    busy = os.path.join(work, "busy")
    feedgame.copy_playground(busy, pg)
    g = feedgame.Game(busy, "busy", "layouttest", mode="explore", feed=False)
    try:
        g.drain(3.0)
        g.send("\033", settle=1.0)
        game_files = {n: v for n, v in snapshot(busy).items()
                      if n[0].isdigit() or "lock" in n}
        t = time.time()
        got = dump(busy, "--layout-hashes", b"layouttest\n", work)
        secs = time.time() - t
        after = {n: v for n, v in snapshot(busy).items()
                 if n[0].isdigit() or "lock" in n}
        rng = random.Random(1)
        feedgame.play(g, rng, 30)
        g.finish()
        ended = (g.status is not None and os.WIFEXITED(g.status)
                 and os.WEXITSTATUS(g.status) == 0)
    finally:
        g.close()
    with open(os.path.join(busy, "xlogfile")) as f:
        xlog = f.read()
    m = re.findall(r"datahash=([0-9a-f]+)", xlog)
    hdr = json.loads(got[1].decode().splitlines()[0]) if got[1] else {}
    good = (got[0] == 0 and secs < 5 and bool(game_files)
            and game_files == after and ended and bool(m)
            and m[-1] == hdr.get("datahash"))
    print("busy         %s  %d game files untouched, game ended %s,"
          " datahash %s" % ("ok  " if good else "FAIL", len(game_files),
                            "normally" if ended else "ABNORMALLY",
                            "matches" if m and m[-1] == hdr.get("datahash")
                            else "DIFFERS"))
    return good


# ---------- games and their feeds ----------

class Feed:
    """a game's feed (feedgame.Game.read_feed()), as dicts"""

    def __init__(self, g):
        self.g = g
        self.parsed = 0
        self.dicts = []
        g.read_feed()

    @property
    def lines(self):
        """the feed's complete lines so far"""
        upto = self.g.feed.rfind(b"\n") + 1
        if upto > self.parsed:
            self.dicts += [json.loads(x) for x in
                           bytes(self.g.feed[self.parsed:upto]).split(b"\n")
                           if x]
            self.parsed = upto
        return self.dicts

    def hero(self):
        """(x, y, dn, dl) as last written"""
        for x in reversed(self.lines):
            if x["k"] in ("hero", "kf"):
                h = x if x["k"] == "hero" else x["hero"]
                return h["x"], h["y"], h["dn"], h["dl"]
        return None

    def terrain(self, levels):
        """the hero's level's terrain now, [typ], [flags]: its last
        keyframe's, against its layout in levels, and the changes since"""
        lines = self.lines[:]
        for n in range(len(lines) - 1, -1, -1):
            if lines[n]["k"] == "kf":
                break
        else:
            return None
        terr = decode(lines[n], levels)[0]
        typ = [ord(c) - 65 for c in terr[0]]
        flags = [ord(c) - 65 for c in terr[1]]
        for x in lines[n + 1:]:
            if x["k"] == "lvl" and "cells" in x:
                for i, t, f, _, _ in x["cells"]:
                    typ[i], flags[i] = t, f
        return typ, flags

    def stairs(self):
        """the hero's level's stairs, as last written"""
        for x in reversed(self.lines):
            if x["k"] == "kf":
                return x["level"]["stairs"]
            if x["k"] == "lvl" and "stairs" in x:
                return x["stairs"]
        return []


def path_step(typ, flags, src, dst, blocked=()):
    """the first step, (dx, dy), of a shortest walk from src to dst over
    the true terrain, through hidden doors and corridors (to be searched
    for) and around the cells in blocked; None if there is none"""
    def door(i):
        return typ[i] == SDOOR or (typ[i] == DOOR and flags[i] > D_BROKEN)
    start, goal = src[1] * COLNO + src[0], dst[1] * COLNO + dst[0]
    first: dict = {start: None}
    todo = collections.deque([start])
    while todo:
        i = todo.popleft()
        if i == goal:
            return first[i]
        x, y = i % COLNO, i // COLNO
        for dx, dy in STEPS:
            nx, ny = x + dx, y + dy
            j = ny * COLNO + nx
            if (not (1 <= nx < COLNO and 0 <= ny < ROWNO) or j in first
                    or j in blocked
                    or not (typ[j] >= DOOR or typ[j] in (SDOOR, SCORR))
                    or (dx and dy and (door(i) or door(j)))):
                continue
            first[j] = first[i] or (dx, dy)
            todo.append(j)
    return None


def settle(g, feed, mark, keys, secs=10):
    """wait until the game has read the keys sent since feed.lines[mark]
    (keys of them) and come back for a command; Escape answers what asks
    for more (--More--, a prompt)"""
    end = time.time() + secs
    poke = time.time() + 1.0
    while g.alive and time.time() < end:
        kinds = [x["k"] for x in feed.lines[mark:]
                 if x["k"] in ("key", "hero")]
        if kinds.count("key") >= keys:
            if kinds[-1] == "hero":
                return True
            if time.time() > poke:
                g.send("\033", settle=0.0)
                keys += 1
                poke = time.time() + 1.0
        g.drain(0.02)
    return False


def take_stairs(g, feed, levels, up, tries=500):
    """walk to the stairs (the shortest way, as the feed's terrain has it,
    searching where a hidden door or corridor is in the way, kicking a
    locked door, going around a square a step didn't get onto) and take
    them; TRUE once on another level"""
    key = "<" if up else ">"
    wander = random.Random(0)  # (not the caller's: its play stays the same)
    start = feed.hero()
    blocked = set()
    for _ in range(tries):
        here = feed.hero()
        if not g.alive or not here or not start:
            return False
        if here[2:] != start[2:]:
            return True
        cands = [s for s in feed.stairs() if s[2] == (1 if up else 0)]
        if not cands:
            return False
        cands.sort(key=lambda s: (s[4] != here[2],
                                  abs(s[0] - here[0]) + abs(s[1] - here[1])))
        terr = feed.terrain(levels)
        step = j = None
        if (here[0], here[1]) == tuple(cands[0][:2]):
            keys = key
        else:
            step = (path_step(terr[0], terr[1], here, cands[0], blocked)
                    if terr else None)
            if step is None:
                blocked.clear()
                keys = wander.choice(feedgame.DIRS)
            else:
                j = (here[1] + step[1]) * COLNO + here[0] + step[0]
                keys = STEPS[step]
                if terr[0][j] in (SDOOR, SCORR):
                    keys = "5s"
                elif terr[0][j] == DOOR and terr[1][j] & D_LOCKED:
                    keys = "\004" + keys
        mark = len(feed.lines)
        g.tail = ""
        feedgame.send_keys(g, "\033" + keys, "explore", settle=0.0)
        settle(g, feed, mark, 1 + len(keys))
        if keys == STEPS.get(step) and feed.hero() == here:
            blocked.add(j)
    here = feed.hero()
    return bool(here) and here[2:] != start[2:]


def race_game(pg, name, seed, record, mode="explore"):
    """a game as a race server starts one: the seed in NETHACKOPTIONS"""
    g = feedgame.Game(pg, name, "", mode=mode, record=record, feed=True,
                      extra_env={
                          "NETHACKOPTIONS": "seed:%s,%s" % (seed,
                                                            RACE_OPTIONS),
                          "NH_FEEDCHECK": "1"})
    return g, Feed(g)


def ask_keyframe(g):
    """SIGUSR1: a keyframe on the level the game is already on, to check
    against the folded state (a game that has gone fails its check)"""
    g.signal(signal.SIGUSR1)


def play(g, rng, keys):
    """random keys, after asking for a keyframe"""
    ask_keyframe(g)
    feedgame.play(g, rng, keys)


def end_game(g, command):
    g.finish(command)
    g.close()
    g.feed_thread.join(10)


# ---------- folding a feed ----------

def decode(kf, dump_levels):
    """a keyframe's level as [terrain (4 lists), map, screen, view]"""
    lv = kf["level"]
    if lv["layout"] is None:
        terr = [["A"] * CELLS, ["A"] * CELLS, ["0"] * CELLS, ["0"] * CELLS]
    else:
        d = dump_levels[(lv["dn"], lv["dl"])]
        terr = [list(d[f]) for f in ("typ", "flags", "lit", "horiz")]
    for run in lv["terr"]:
        for f in range(4):
            for k, c in enumerate(run[1 + f]):
                terr[f][run[0] + k] = c
    pal = [s[0] for s in lv["sym"]]
    w, g = lv["gw"], lv["g"]
    gl = []
    for i in range(CELLS):
        n = 0
        for c in g[i * w:(i + 1) * w]:
            n = n * 64 + CODES.index(c)
        gl.append(pal[n])
    scr = list(gl)
    for i, glyph in lv["scr"]:
        scr[i] = glyph
    vis = [0] * CELLS
    for i, n in lv["vis"]:
        for k in range(n):
            vis[i + k] = 1
    return [terr, gl, scr, vis]


def chk_values(state):
    terr, gl, scr, vis = state
    return {"terr": fnv("".join("".join(f) for f in terr).encode()),
            "g": fnv("".join("%d," % x for x in gl).encode()),
            "scr": fnv("".join("%d," % x for x in scr).encode()),
            "vis": fnv("".join("1" if v else "0" for v in vis).encode())}


def fold(lines, dump_levels, null_base):
    """-> (problems, keyframes, checkpoints, chk lines, levels arrived)"""
    problems = []
    state, lev = None, None
    nkf = ncheck = nchk = 0
    arrived = []
    for x in lines:
        k = x["k"]
        here = (x.get("dn"), x.get("dl"))
        if k == "kf":
            nkf += 1
            lv = x["level"]
            klev = (lv["dn"], lv["dl"])
            if null_base:
                if lv["layout"] is not None:
                    problems.append("keyframe with a layout at %s" % (klev,))
                    continue
            elif (klev not in dump_levels or lv["layout"] is None
                  or lv["layout"] != dump_levels[klev]["layout"]):
                problems.append("keyframe layout at %s isn't the dump's" %
                                (klev,))
                continue
            new = decode(x, dump_levels)
            if state is not None and lev == klev:
                ncheck += 1
                if new != state:
                    problems.append("keyframe at %s (%s) differs from the"
                                    " folded state" % (klev, x["why"]))
            elif x["why"] == "arrive":
                arrived.append(klev)
            state, lev = new, klev
        elif state is None:
            continue
        elif k in ("map", "scr", "vis", "lvl") and here != lev:
            problems.append("%s line for %s while on %s" % (k, here, lev))
        elif k == "map":
            for c in x["cells"]:
                state[1][c[0]] = c[1]
        elif k == "scr":
            for c in x["cells"]:
                state[2][c[0]] = c[1]
        elif k == "vis":
            for i in x["on"]:
                state[3][i] = 1
            for i in x["off"]:
                state[3][i] = 0
        elif k == "lvl" and "cells" in x:
            for i, t, f, lit, hz in x["cells"]:
                state[0][0][i] = chr(65 + t)
                state[0][1][i] = chr(65 + f)
                state[0][2][i] = str(lit)
                state[0][3][i] = str(hz)
        elif k == "chk":
            nchk += 1
            if here != lev:
                problems.append("chk for %s while on %s" % (here, lev))
                continue
            want = chk_values(state)
            bad = [f for f in want if x[f] != want[f]]
            if bad:
                problems.append("chk at turn %d on %s: %s differ" %
                                (x["t"], lev, "/".join(bad)))
    return problems, nkf, ncheck, nchk, arrived


def check_rebuild(pg, work, seeds):
    ok = True
    for n, seed in enumerate(seeds):
        gpg = os.path.join(work, "game%d" % n)
        feedgame.copy_playground(gpg, pg)
        record = os.path.join(gpg, "game.nhrec")
        rng = random.Random(seed)
        name = "rebuild%d" % n
        levels, _ = layouts(pg, seed, work)
        g, f1 = race_game(gpg, name, seed, record)
        started = g.first_command()
        play(g, rng, 20)
        down = take_stairs(g, f1, levels, False)
        play(g, rng, 20)
        down = down and take_stairs(g, f1, levels, False)
        play(g, rng, 10)
        end_game(g, "S")
        g, f2 = race_game(gpg, name, seed, record)
        started = g.first_command() and started
        play(g, rng, 5)
        up = take_stairs(g, f2, levels, True)
        play(g, rng, 20)
        end_game(g, "#quit\r")
        p1, k1, c1, h1, a1 = fold(f1.lines, levels, False)
        p2, k2, c2, h2, a2 = fold(f2.lines, levels, False)
        visited = set(a1) | {(x["hero"]["dn"], x["hero"]["dl"])
                             for x in f1.lines if x["k"] == "kf"}
        revisit = any(lev in visited for lev in a2)
        problems = p1 + p2
        if not started:
            problems.append("a game never asked for a command")
        if not (down and up and revisit):
            problems.append("route not completed (down %s, back up %s,"
                            " revisited after the restore %s)"
                            % (down, up, revisit))
        good = not problems
        print("rebuild      %-18r %s  %d keyframes (%d checkpoints),"
              " %d chk lines, levels %s / %s" %
              (seed, "ok  " if good else "FAIL", k1 + k2, c1 + c2, h1 + h2,
               a1, a2))
        for pr in problems[:5]:
            print("               " + pr)
        ok &= good
    return ok


def check_nullbase(pg, work):
    gpg = os.path.join(work, "wizard")
    feedgame.copy_playground(gpg, pg)
    rng = random.Random(3)
    g, feed = race_game(gpg, "nullbase", "layouttest", None, mode="wizard")
    started = g.first_command()
    for lev in (3, 5, 2, 4):
        ask_keyframe(g)
        feedgame.play(g, rng, 10, mode="wizard")
        feedgame.send_keys(g, ["\033", "\026", "%d\r" % lev, "\033"],
                           "wizard")
    ask_keyframe(g)
    feedgame.play(g, rng, 10, mode="wizard")
    end_game(g, "#quit\r")
    problems, nkf, ncheck, nchk, arrived = fold(feed.lines, {}, True)
    if not started:
        problems.append("the game never asked for a command")
    if len(set(arrived)) < 3:
        problems.append("only %d levels arrived on" % len(set(arrived)))
    good = not problems
    print("nullbase     %s  %d keyframes (%d checkpoints), %d chk lines,"
          " levels %s" % ("ok  " if good else "FAIL", nkf, ncheck, nchk,
                          arrived))
    for pr in problems[:5]:
        print("               " + pr)
    return good


def main():
    ap = argparse.ArgumentParser(description=(__doc__ or "").split("\n")[0])
    ap.add_argument("playground")
    ap.add_argument("--seeds", type=int, default=3,
                    help="games for the rebuild check")
    ap.add_argument("--keep", action="store_true",
                    help="keep the scratch files")
    args = ap.parse_args()
    pg = os.path.abspath(args.playground)
    with open(os.path.join(pg, "sysconf")) as f:
        config = f.read()
    if re.search(r"^\s*(SEED|RECORDFILE)\s*=", config, re.M):
        sys.exit("use a test playground's sysconf (no SEED or RECORDFILE)")
    if os.stat(os.path.join(pg, "nethack")).st_mode & (stat.S_ISUID
                                                        | stat.S_ISGID):
        sys.exit("use an unprivileged playground binary")
    work = tempfile.mkdtemp(prefix="layouttest-")
    ok = True
    try:
        ok &= check_repeat_hashes(pg, work)
        ok &= check_ignored(pg, work)
        ok &= check_sysconf(pg, work)
        ok &= check_refusals(pg, work)
        ok &= check_files(pg, work)
        ok &= check_sandbox(pg, work)
        ok &= check_busy(pg, work)
        ok &= check_rebuild(pg, work, SEEDS[:args.seeds])
        ok &= check_nullbase(pg, work)
    except Exception:
        traceback.print_exc()
        ok = False
    finally:
        if args.keep or not ok:
            print("scratch files kept in", work)
        else:
            shutil.rmtree(work, ignore_errors=True)
    print("layouttest:", "ok" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
