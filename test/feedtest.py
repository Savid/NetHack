#!/usr/bin/env python3
"""Check that the live feed (src/feed.c in the fork) leaves the game alone.

  no-influence  a game recorded without the feed replays and verifies
                with it, and its state log (NH_STATELOG: after every key,
                the draws from the core and display RNGs and a hash of
                objects, their knowledge bits, monsters, discoveries and
                the game log) is the same, key for key, as a replay's
                without the feed.  The record's own digests are checked
                only now and then and cover less; the state log pins down
                the first key after which the feed made a difference
  (every run with the feed also writes its "chk" lines, NH_FEEDCHECK, so
  the no-influence and signal checks cover them too)
  determinism   a game recorded with the feed, replayed with the feed,
                writes the same feed, byte for byte, up to where the replay
                stops (the end of the session)
  signal        SIGUSR1 (a collector asking for a keyframe) while the game
                runs and while it waits for a key: the record shows no
                hangup or interrupt, keyframes appear, and the recorded
                game (feed on, signalled) has the same state log as its
                replay with no feed at all

Usage: feedtest.py [-k KEYS] [--seeds N] [--mode wizard|explore|normal]
"""
import argparse
import json
import os
import random
import re
import select
import shutil
import signal
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import feedgame as nhgame  # noqa: E402


def capture(fd, path):
    """copy a feed pipe to a file until it closes"""
    def run():
        with open(path, "wb") as out:
            while True:
                d = os.read(fd, 65536)
                if not d:
                    break
                out.write(d)
        os.close(fd)
    t = threading.Thread(target=run, daemon=True)
    t.start()
    return t


def record_game(work, seed, keys, mode, feed, rand, signals=False,
                statelog=None):
    pg = os.path.join(work, "pg")
    nhgame.copy_playground(pg)
    rec = os.path.join(work, "game.rec")
    extra = {"NH_STATELOG": statelog} if statelog else {}
    if feed:
        extra["NH_FEEDCHECK"] = "1"
    g = nhgame.Game(pg, "feedtest", seed, mode=mode, record=rec, feed=feed,
                    extra_env=extra)
    t = capture(g.feed_fd, os.path.join(work, "feed.ndjson")) if feed \
        else None
    g.drain(1.0)
    rng = random.Random(rand)
    sent = [0]

    def on_key(n):
        if signals and n % 37 == 5 and g.alive:
            # mid-command, and then while the game is waiting for a key
            g.send("20s", settle=0.0)
            for pause in (0.2, 1.3):
                if not g.signal(signal.SIGUSR1):
                    return
                sent[0] += 1
                g.drain(pause)
    nhgame.play(g, rng, keys, mode=mode, on_key=on_key)
    g.finish()
    g.close()
    if t:
        t.join(10)
    return pg, rec, sent[0]


def replay(pg, rec, feed_path=None, timeout=600, statelog=None):
    """nethack --replay REC --verify, with the feed to feed_path"""
    settings = dict(NETHACKDIR=pg, TERM="xterm", HOME=pg)
    if statelog:
        settings["NH_STATELOG"] = statelog
    env = nhgame.game_env(**settings)
    rfd = wfd = None
    if feed_path:
        rfd, wfd = os.pipe()
        os.set_inheritable(wfd, True)
        env["NETHACK_FEED_FD"] = str(wfd)
        env["NH_FEEDCHECK"] = "1"
    import pty
    pid, fd = pty.fork()
    if pid == 0:
        try:
            if rfd is not None:
                os.close(rfd)
            os.chdir(pg)
            os.execve("./nethack", ["./nethack", "--replay", rec,
                                    "--verify"], env)
        finally:
            os._exit(127)
    t = None
    if wfd is not None:
        os.close(wfd)
        t = capture(rfd, feed_path)
    out = b""
    last = time.time()
    status = None
    while status is None:
        r, _, _ = select.select([fd], [], [], 0.5)
        if r:
            try:
                d = os.read(fd, 65536)
            except OSError:
                d = b""
            if d:
                out += d
                last = time.time()
        wpid, st = os.waitpid(pid, os.WNOHANG)
        if wpid:
            status = st
        elif time.time() - last > timeout:
            os.kill(pid, signal.SIGKILL)
            os.waitpid(pid, 0)
            status = -1
    os.close(fd)
    if t:
        t.join(10)
    text = re.sub(rb"\x1b\[[0-9;?]*[A-Za-z]", b"", out).decode("latin-1")
    tail = [l.strip() for l in text.replace("\r", "").split("\n")
            if l.startswith("session ") or l.startswith("replay ")]
    code = os.WEXITSTATUS(status) if status >= 0 and os.WIFEXITED(status) \
        else -1
    return code, tail


def record_events(rec):
    with open(rec, "rb") as f:
        data = f.read()
    tags = re.findall(rb"(?:^|\n)([a-z]{1,15}) [0-9]+:", data)
    return [t.decode() for t in tags]


WALLCLOCK = re.compile(rb"elapsed playing time is [^.\"]*")


def same_state(a, b, prefix=False):
    """compare two state logs, key by key -> (same, what); with prefix, b
    may stop early (a replay stops at the session's end, where the recorded
    game goes on answering the end-of-game questions)"""
    x, y = lines(a), lines(b)
    x, y = [l for l in x if l], [l for l in y if l]
    for i, (p, q) in enumerate(zip(x, y)):
        if p != q:
            return False, "first differs after key %d: %r vs %r" % (
                i + 1, p.decode(), q.decode())
    if len(x) != len(y) and not (prefix and len(y) < len(x)):
        return False, "%d vs %d keys" % (len(x), len(y))
    return True, "%d keys" % min(len(x), len(y))


def lines(path):
    with open(path, "rb") as f:
        return f.read().split(b"\n")


def session_test(root, mode):
    """Save and restore: both replay execs must write complete sessions."""
    work = os.path.join(root, "sessions")
    pg = os.path.join(work, "pg")
    nhgame.copy_playground(pg)
    rec = os.path.join(work, "game.rec")
    for command in ("S", "#quit\r"):
        g = nhgame.Game(pg, "feedtest", "feedtest-sessions", mode=mode,
                        record=rec, feed=False)
        g.drain(0.5)
        # Explore/wizard restores ask whether to keep the save file.
        if "keep the save" in g.tail:
            g.send("n")
        g.finish(command)
        g.close()
    path = os.path.join(work, "replay.ndjson")
    code, tail = replay(pg, rec, path)
    data = [json.loads(x) for x in lines(path) if x]
    headers = [x for x in data if x["k"] == "hdr"]
    good = (code == 0 and len(headers) == 2
            and [x["restored"] for x in headers] == [0, 1]
            and sum(x["k"] == "end" for x in data) == 2
            and sum(x["k"] == "kf" for x in data) >= 2)
    print("sessions     %s  replay exit %d, %d headers; %s"
          % ("ok" if good else "FAIL", code, len(headers), " / ".join(tail)))
    return good


# a seed whose hero is a dwarf (a seeded game's character comes from its
# seed, not the options); with color on and off, the two games have the
# same level, the same object descriptions and the same date
GLYPH_SEED = "feedtest-glyphs-20"


def glyph_run(work, mode, color):
    """GLYPH_SEED's dwarf with showrace, color on or off: whether it reached
    its first command, and the feed's lines as dicts."""
    pg = os.path.join(work, "pg")
    nhgame.copy_playground(pg)
    opts = ("seed:%s,showrace,%scolor,!legacy,!news,!splash_screen,"
            "!tutorial,!autopickup" % (GLYPH_SEED, "" if color else "!"))
    g = nhgame.Game(pg, "feedtest", "", mode=mode,
                    extra_env={"NETHACKOPTIONS": opts})
    g.read_feed()
    try:
        ready = g.first_command()
        # a text window with glyph escapes in it: "/", "nearby monsters"
        # (the pet, at least). Do this before moving: combat messages or
        # movement prompts can otherwise swallow the lookup commands.
        for key in "/m":
            g.send(key)
            g.drain(0.3)
        for _ in range(3):
            g.send("\033")
            g.drain(0.2)
        for key in "hhjjkkll":
            g.tail = ""
            g.send(key)
            g.drain(0.1)
            # Cancel movement prompts and dismiss combat's --More--.
            g.send("\033\033")
        g.signal(signal.SIGUSR1)
        g.drain(1.3)
    finally:
        g.finish()
        g.close()
        g.feed_thread.join(10)
    with open(os.path.join(work, "feed.ndjson"), "wb") as out:
        out.write(g.feed)
    return ready, feed_events(g.feed)


def glyph_test(root, mode):
    """The dwarf's showrace color belongs to its cell, not its glyph; the
    glyph metadata is the same with the color option off; text windows
    carry symbols, not \\G escapes."""
    ready, data = glyph_run(os.path.join(root, "glyphs"), mode, True)
    races = [x["character"]["race"] for x in data if x["k"] == "hdr"]
    frames = [x for x in data if x["k"] == "kf" and x["why"] == "signal"]
    good, checked = False, 0
    symbols, screen = {}, []
    if frames:
        frame = frames[-1]
        symbols = {x[0]: x[1:] for x in frame["level"]["sym"]}
        screen = frame["hero"]["screen"]
        good = (screen[1:] == ["h", 15]
                and symbols.get(screen[0], [])[:2] == ["h", 1])
        for event in data[:data.index(frame)]:
            if event["k"] == "scr":
                for cell in event["cells"]:
                    if cell[1] in symbols:
                        good &= cell[2:] == symbols[cell[1]]
                        checked += 1
    good &= ready and races == ["dwarf"] and checked > 0
    texts = [line for x in data if x["k"] == "ui" and x["ev"] == "text"
             for line in x["lines"]]
    escapes = [line for line in texts if "\\G" in line]
    good &= len(texts) > 0 and not escapes
    # the same game with color off: the same glyph metadata
    ready2, data2 = glyph_run(os.path.join(root, "glyphs-nocolor"), mode,
                              False)
    frames2 = [x for x in data2 if x["k"] == "kf" and x["why"] == "signal"]
    same = 0
    if ready2 and frames2:
        symbols2 = {x[0]: x[1:] for x in frames2[-1]["level"]["sym"]}
        common = set(symbols) & set(symbols2)
        same = sum(1 for k in common if symbols[k] == symbols2[k])
        good &= (len(common) > 0 and same == len(common)
                 and frames2[-1]["hero"]["screen"] == screen)
    else:
        good = False
    notes = []
    if not (ready and ready2):
        notes.append("a game never asked for a command")
    if races != ["dwarf"]:
        notes.append("%s's hero is %s, not a dwarf" % (GLYPH_SEED, races))
    print("glyphs       %s  %d incremental cells checked, %d text lines,"
          " %d glyphs same without color%s"
          % ("ok" if good else "FAIL", checked, len(texts), same,
             "".join("; " + n for n in notes)))
    return good


def menu_text_test(root):
    """Show pending menus, preserve UTF-8 and tty's assigned letters."""
    work = os.path.join(root, "menu-text")
    pg = os.path.join(work, "pg")
    nhgame.copy_playground(pg)
    path = os.path.join(work, "feed.ndjson")
    g = nhgame.Game(pg, "Zoë", "", mode="explore", extra_env={
        "NETHACKOPTIONS": "role:Valkyrie,race:human,gender:female,"
        "align:lawful,!legacy,!news,!splash_screen,!tutorial,!autopickup",
    })
    g.read_feed()
    try:
        ready = g.first_command()
        g.send("O")
        g.drain(0.3)
        g.signal(signal.SIGUSR1)
        g.drain(1.3)
        g.send("\033")
        g.signal(signal.SIGUSR1)
        g.drain(1.3)
    finally:
        g.finish()
        g.close()
        g.feed_thread.join(10)
    with open(path, "wb") as out:
        out.write(g.feed)
    data = feed_events(g.feed)
    frames = [e for e in data if e["k"] == "kf"]
    menus = [e for e in data if e.get("ev") == "menu"
             and e.get("prompt") == "Options"]
    good = ready and bool(frames and menus)
    good &= all(e["hero_x"]["name"] == "Zoë" for e in frames)
    items = [i for e in menus for i in e["items"] if not i[2] & 2]
    good &= bool(items) and all(len(i[0]) == 1 for i in items)
    opened = [i for i, e in enumerate(data) if e.get("ev") == "menu_open"
              and e.get("prompt") == "Options"]
    answered = [i for i, e in enumerate(data) if e in menus]
    good &= len(opened) == len(answered) == 1
    if opened and answered:
        first, last = opened[0], answered[0]
        fields = {k: data[first][k] for k in ("win", "prompt", "how", "items")}
        pending = [e for e in data[first + 1:last] if e["k"] == "kf"]
        closed = [e for e in data[last + 1:] if e["k"] == "kf"]
        good &= first < last and data[last]["win"] == fields["win"]
        good &= bool(pending) and all(e["menus"] == [fields] for e in pending)
        good &= bool(closed) and all(not e["menus"] for e in closed)
    print("menu/text    %s  pending menu snapshots, UTF-8, %d selectable items"
          % ("ok" if good else "FAIL", len(items)))
    return good


def feed_events(raw):
    """the feed's lines, less its "chk" lines (NH_FEEDCHECK)"""
    return [json.loads(x) for x in bytes(raw).split(b"\n")[:-1]
            if x and b'"k":"chk"' not in x]


def ending_game(pg, rec):
    """A wizard-mode game at its first command, its feed read as it comes:
    (game, feed bytes, reader thread)."""
    nhgame.copy_playground(pg)
    g = nhgame.Game(pg, "feedtest", "feedtest-ending", mode="wizard",
                    record=rec, options="pettype:none,!tips",
                    extra_env={"NH_FEEDCHECK": "1"})
    g.read_feed()
    g.first_command()
    return g, g.feed, g.feed_thread


def ending_key(g, k, text, secs=10):
    """send a key and wait for text on the screen"""
    g.tail = ""
    g.send(k, settle=0.0)
    end = time.time() + secs
    while text not in g.screen() and g.alive and time.time() < end:
        g.drain(0.05)
    return text in g.screen()


def paniclog_size(pg):
    p = os.path.join(pg, "paniclog")
    return os.path.getsize(p) if os.path.exists(p) else 0


def ending_exit(g, t, secs=10):
    """wait for the game to exit; then let the feed's reader finish"""
    end = time.time() + secs
    while g.status is None and time.time() < end:
        g.drain(0.1)
        g.reap()
        time.sleep(0.05)
    exited = g.status is not None
    if not exited:
        os.kill(g.pid, signal.SIGKILL)
        os.waitpid(g.pid, 0)
    g.close()
    t.join(10)
    return exited


def ending_test(root, mode):
    """A session that ends without the game ending ends with a keyframe of
    the state it ended in ("end"): hung up while a level change waits at
    its --More-- (where a keyframe asked for waits too), recorded and
    replayed, and not recorded (a hangup must end the wait, and log
    nothing to the paniclog); and saved, the state written before the save
    frees it."""
    if mode != "wizard":
        print("ending       skipped (needs --mode wizard)")
        return True
    ok = True
    for record in (True, False):
        name = "hangup-rec" if record else "hangup"
        work = os.path.join(root, "ending-" + name)
        pg = os.path.join(work, "pg")
        rec = os.path.join(work, "game.rec") if record else None
        g, raw, t = ending_game(pg, rec)
        logged = paniclog_size(pg)
        # map the level, teleport to the down stairs, take them
        g.send("\006", settle=0.0)
        waiting = (ending_key(g, "\024", "teleported?")
                   and ending_key(g, ">", "staircase down")
                   and ending_key(g, ".", "materialize")
                   and ending_key(g, ":", "staircase down here")
                   and ending_key(g, ">", "--More--"))
        d = feed_events(raw)
        down = max([i for i, e in enumerate(d)
                    if e["k"] == "key" and e["key"] == ord(">")] or [0])
        g.signal(signal.SIGUSR1)
        g.drain(1.5)
        early = [e for e in feed_events(raw)[down:] if e["k"] == "kf"]
        g.signal(signal.SIGHUP)
        exited = ending_exit(g, t)
        logged = paniclog_size(pg) - logged
        d = feed_events(raw)
        tail = d[down:]
        lev = [e for e in tail if e.get("ev") == "level"]
        kfs = [e for e in tail if e["k"] == "kf"]
        good = (waiting and not early and exited and not logged
                and len(lev) == 1 and len(kfs) == 1
                and kfs[0]["why"] == "end"
                and kfs[0]["level"]["dl"] == lev[0]["to"]["dl"]
                and tail[-2] is kfs[0] and tail[-1]["k"] == "end"
                and tail[-1]["how"] == "exit")
        replayed = ""
        if rec:
            path = os.path.join(work, "replay.ndjson")
            code, _ = replay(pg, rec, path)
            same = lines(path) == bytes(raw).split(b"\n")
            good &= code == 0 and same
            replayed = "; replay exit %d, %s feed" % (
                code, "same" if same else "DIFFERENT")
        ok &= good
        print("ending       %-12s %s  waiting at the level change: %s,"
              " keyframes before the hangup %d, exited %s, paniclog +%d,"
              " after the descent %s%s"
              % (name, "ok  " if good else "FAIL", waiting, len(early),
                 exited, logged, [e["k"] for e in tail[-5:]], replayed))

    g, raw, t = ending_game(os.path.join(root, "ending-save", "pg"), None)
    if ending_key(g, "S", "Really save?"):
        g.send("y", settle=0.0)
    exited = ending_exit(g, t)
    d = feed_events(raw)
    kfs = [e for e in d if e["k"] == "kf"]
    # nothing happens between the first keyframe and the save, so the last
    # is the first again, with the save's why
    diff = []
    if len(kfs) == 2:
        first = dict(kfs[0], why="end")
        diff = sorted(k for k in set(first) | set(kfs[1])
                      if first.get(k) != kfs[1].get(k))
    good = (exited and len(kfs) == 2 and not diff and d[-2] is kfs[1]
            and d[-1]["k"] == "end" and d[-1]["how"] == "exit")
    ok &= good
    print("ending       %-12s %s  exited %s, keyframes %s%s"
          % ("save", "ok  " if good else "FAIL", exited,
             [e["why"] for e in kfs],
             ("; differs from the first in %s" % diff) if diff else ""))
    return ok


def xlog_death(pg):
    """the last xlogfile entry's death field (the game writes it even in
    wizard mode)"""
    p = os.path.join(pg, "xlogfile")
    if not os.path.exists(p):
        return None
    with open(p, "rb") as f:
        last = f.read().decode("utf-8", "replace").splitlines()[-1:]
    for field in (last[0].split("\t") if last else []):
        if field.startswith("death="):
            return field[len("death="):]
    return None


def death_test(root, mode):
    """The feed's death event has the cause topten writes: no "a" or "an"
    before an escape or a quit, and what the hero carried out ("with the
    Amulet", "with a fake Amulet").  (The real Amulet takes the first
    level's up stairs to the endgame, so it goes out with a quit.)"""
    if mode != "wizard":
        print("death        skipped (needs --mode wizard)")
        return True
    climb = [("<", "Still climb?"), ("y", None)]
    quit_ = [("#quit\r", "Really quit"), ("y", None)]
    cases = (
        ("escape", [], climb, "escaped", "escaped"),
        ("quit", [], quit_, "quit", "quit"),
        ("amulet", ["Amulet of Yendor"], quit_, "quit",
         "quit (with the Amulet)"),
        ("fake", ["cheap plastic imitation of the Amulet of Yendor"],
         climb, "escaped", "escaped (with a fake Amulet)"),
    )
    ok = True
    for name, wishes, keys, how, cause in cases:
        pg = os.path.join(root, "death-" + name, "pg")
        g, raw, t = ending_game(pg, None)
        steps = True
        for w in wishes:
            steps &= ending_key(g, "\027", "For what do you wish?")
            g.tail = ""
            g.send(w + "\r", settle=0.5)
            # (getting the Amulet grants a wish of its own: decline it)
            for _ in range(6):
                if "For what do you wish?" in g.screen():
                    g.tail = ""
                    g.send("nothing\r", settle=0.5)
                elif "--More--" in g.screen():
                    g.tail = ""
                    g.send(" ", settle=0.5)
                else:
                    break
        for k, text in keys:
            if text:
                steps &= ending_key(g, k, text)
            else:
                g.send(k, settle=0.0)
        # the end-of-game questions
        end = time.time() + 20
        while g.status is None and time.time() < end:
            g.send("q", settle=0.0)
            g.drain(0.2)
            g.reap()
        exited = ending_exit(g, t)
        deaths = [e for e in feed_events(raw) if e.get("ev") == "death"]
        got = deaths[0] if len(deaths) == 1 else {}
        xlog = xlog_death(pg)
        good = (steps and exited and got.get("how") == how
                and got.get("cause") == cause and xlog == cause)
        ok &= good
        print("death        %-12s %s  how %r, cause %r (want %r),"
              " xlogfile %r%s"
              % (name, "ok  " if good else "FAIL", got.get("how"),
                 got.get("cause"), cause, xlog,
                 "" if steps else "; a prompt never came"))
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-k", type=int, default=600, help="keys per game")
    ap.add_argument("--seeds", type=int, default=3)
    ap.add_argument("--mode", choices=("wizard", "explore", "normal"),
                    default="wizard")
    ap.add_argument("playground", nargs="?", default=nhgame.PLAYGROUND)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()
    nhgame.PLAYGROUND = os.path.abspath(args.playground)
    binary = os.stat(os.path.join(nhgame.PLAYGROUND, "nethack"))
    if binary.st_mode & 0o6000:
        ap.error("run with an unprivileged playground binary")
    root = tempfile.mkdtemp(prefix="feedtest-")
    ok = session_test(root, args.mode)
    ok &= glyph_test(root, args.mode)
    ok &= menu_text_test(root)
    ok &= ending_test(root, args.mode)
    ok &= death_test(root, args.mode)

    for n in range(args.seeds):
        seed = "feedtest-%d" % n

        # no-influence: recorded without the feed, replayed without it and
        # with it, the two state logs compared
        w = os.path.join(root, "noinf-%d" % n)
        pg, rec, _ = record_game(w, seed, args.k, args.mode, False, n)
        c0, _ = replay(pg, rec, statelog=os.path.join(w, "state-off.log"))
        code, tail = replay(pg, rec, os.path.join(w, "replay.ndjson"),
                            statelog=os.path.join(w, "state-on.log"))
        nl = len(lines(os.path.join(w, "replay.ndjson")))
        same, why = same_state(os.path.join(w, "state-off.log"),
                               os.path.join(w, "state-on.log"))
        good = code == 0 and c0 == 0 and same and nl > 100
        ok &= good
        print("no-influence %-12s %s  replay exit %d (without the feed %d),"
              " %d feed lines, state log %s %s; %s"
              % (seed, "ok  " if good else "FAIL", code, c0, nl,
                 "same over" if same else "DIFFERS:", why,
                 " / ".join(tail)))

        # determinism: recorded with the feed, replayed with it
        w = os.path.join(root, "det-%d" % n)
        pg, rec, _ = record_game(w, seed, args.k, args.mode, True, n + 100)
        code, tail = replay(pg, rec, os.path.join(w, "replay.ndjson"))
        # (what the game shows of the wall clock can't replay: ^X's
        # "Total elapsed playing time")
        a = [WALLCLOCK.sub(b"<wall clock>", x)
             for x in lines(os.path.join(w, "feed.ndjson"))]
        b = [WALLCLOCK.sub(b"<wall clock>", x)
             for x in lines(os.path.join(w, "replay.ndjson"))]
        same = 0
        while same < min(len(a), len(b)) and a[same] == b[same]:
            same += 1
        # the replay stops at the session's end; the original goes on to
        # write the dump and its end line
        # (answering the end-of-game questions); the replay ends with its
        # own end line there
        rest = [json.loads(x)["k"] for x in a[same:] if x]
        brest = [json.loads(x)["k"] for x in b[same:] if x]
        if brest == ["end"]:
            brest = []
        # (answering the end-of-game questions: messages, keys, the
        # disclosure windows, the dump)
        good = (code == 0 and not brest
                and set(rest) <= {"dump", "end", "msg", "ev", "key", "ui"})
        ok &= good
        print("determinism  %-12s %s  replay exit %d, %d of %d lines equal,"
              " original goes on with %s%s"
              % (seed, "ok  " if good else "FAIL", code, same, len(a) - 1,
                 sorted(set(rest)),
                 ("; replay differs: %s" % brest[:2]) if brest else ""))

        # signal: SIGUSR1 while playing
        w = os.path.join(root, "sig-%d" % n)
        pg, rec, sent = record_game(w, seed, args.k, args.mode, True,
                                    n + 200, signals=True,
                                    statelog=os.path.join(w, "state-rec.log"))
        ev = record_events(rec)
        kf = [json.loads(x) for x in lines(os.path.join(w, "feed.ndjson"))
              if x and b'"k":"kf"' in x]
        # the recorded game (feed on, signalled) against a replay with no
        # feed at all
        code, tail = replay(pg, rec,
                            statelog=os.path.join(w, "state-off.log"))
        same, why = same_state(os.path.join(w, "state-rec.log"),
                               os.path.join(w, "state-off.log"), prefix=True)
        # (a signal at a prompt waits for the next command, and several
        # there make one keyframe; after the hero's death there are none)
        good = (code == 0 and "hup" not in ev and "intr" not in ev
                and len(kf) >= 2 and same)
        ok &= good
        print("signal       %-12s %s  %d signals, %d keyframes, record has"
              " %d hup %d intr, replay exit %d, state log %s %s; %s"
              % (seed, "ok  " if good else "FAIL", sent, len(kf),
                 ev.count("hup"), ev.count("intr"), code,
                 "same as with no feed over" if same else "DIFFERS:", why,
                 " / ".join(tail)))

    if args.keep or not ok:
        print("kept in", root)
    else:
        shutil.rmtree(root, ignore_errors=True)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
