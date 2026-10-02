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
                try:
                    os.kill(g.pid, signal.SIGUSR1)
                    sent[0] += 1
                except ProcessLookupError:
                    return
                g.drain(pause)
    nhgame.play(g, rng, keys, mode=mode, on_key=on_key)
    g.finish()
    g.close()
    if t:
        t.join(10)
    return pg, rec, sent[0]


def replay(pg, rec, feed_path=None, timeout=600, statelog=None):
    """nethack --replay REC --verify, with the feed to feed_path"""
    env = dict(os.environ, NETHACKDIR=pg, TERM="xterm", HOME=pg)
    env.pop("NETHACK_FEED_FD", None)
    env.pop("NH_STATELOG", None)
    env.pop("NH_FEEDCHECK", None)
    if statelog:
        env["NH_STATELOG"] = statelog
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


def glyph_run(work, mode, color):
    """A dwarf with showrace, color on or off: the feed's lines as dicts."""
    pg = os.path.join(work, "pg")
    nhgame.copy_playground(pg)
    opts = ("role:Valkyrie,race:dwarf,gender:female,align:lawful,"
            "showrace,%scolor,!legacy,!news,!splash_screen,!tutorial,"
            "!autopickup" % ("" if color else "!"))
    g = nhgame.Game(pg, "feedtest", "", mode=mode,
                    extra_env={"NETHACKOPTIONS": opts})
    path = os.path.join(work, "feed.ndjson")
    thread = capture(g.feed_fd, path)
    try:
        g.drain(0.5)
        for _ in range(5):
            if "--More--" not in g.tail:
                break
            g.tail = ""
            g.send(" ")
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
        os.kill(g.pid, signal.SIGUSR1)
        g.drain(1.3)
    finally:
        g.finish()
        g.close()
        thread.join(10)
    return [json.loads(x) for x in lines(path) if x]


def glyph_test(root, mode):
    """The dwarf's showrace color belongs to its cell, not its glyph; the
    glyph metadata is the same with the color option off; text windows
    carry symbols, not \\G escapes."""
    data = glyph_run(os.path.join(root, "glyphs"), mode, True)
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
    good &= checked > 0
    texts = [line for x in data if x["k"] == "ui" and x["ev"] == "text"
             for line in x["lines"]]
    escapes = [line for line in texts if "\\G" in line]
    good &= len(texts) > 0 and not escapes
    # the same game with color off: the same glyph metadata
    data2 = glyph_run(os.path.join(root, "glyphs-nocolor"), mode, False)
    frames2 = [x for x in data2 if x["k"] == "kf" and x["why"] == "signal"]
    same = 0
    if frames2:
        symbols2 = {x[0]: x[1:] for x in frames2[-1]["level"]["sym"]}
        common = set(symbols) & set(symbols2)
        same = sum(1 for k in common if symbols[k] == symbols2[k])
        good &= (len(common) > 0 and same == len(common)
                 and frames2[-1]["hero"]["screen"] == screen)
    else:
        good = False
    print("glyphs       %s  %d incremental cells checked, %d text lines,"
          " %d glyphs same without color"
          % ("ok" if good else "FAIL", checked, len(texts), same))
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
    thread = capture(g.feed_fd, path)
    try:
        g.drain(0.5)
        for _ in range(5):
            if "--More--" not in g.tail:
                break
            g.tail = ""
            g.send(" ")
        g.send("O")
        g.drain(0.3)
        os.kill(g.pid, signal.SIGUSR1)
        g.drain(1.3)
        g.send("\033")
        os.kill(g.pid, signal.SIGUSR1)
        g.drain(1.3)
    finally:
        g.finish()
        g.close()
        thread.join(10)
    data = [json.loads(x) for x in lines(path) if x]
    frames = [e for e in data if e["k"] == "kf"]
    menus = [e for e in data if e.get("ev") == "menu"
             and e.get("prompt") == "Options"]
    good = bool(frames and menus)
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
