#!/usr/bin/env python3
"""Play a long random recorded seeded game, then replay it.

Checks that recording and replay (see "Seeding" in the top directory) hold
up over a long game: in a copy of the playground, recording to a file of
its own (NH_RECORD), it plays a seeded game with thousands of random
ordinary keys (moving, searching, picking up and using items, taking
stairs), saving and restoring it several times, then quits, and runs
"nethack --replay RECORD --verify" on the record.  The game runs on a
pseudo-terminal, with no shell.

Usage: replaytest.py [-k KEYS] [-s SESSIONS] [--mode wizard|explore|normal]
                     [--seed SEED] [--rand N] [--signals] [--login] [--keep]
                     PLAYGROUND

KEYS is the number of keys per session.  In wizard mode (the default) the
hero also teleports between levels now and then, so that many levels get
visited; in wizard and explore mode the hero doesn't die, so the game
lasts; in normal mode it ends when the hero dies.  With --signals, the test
also interrupts the game (^C, answered no) now and then, ends each session
but the last with a hangup (SIGHUP) in the middle of a long search instead
of saving, and quits with ^C.  With --login, the game takes the hero's name
from $USER instead of -u, as when a player starts it without -u; the replay
takes that name from the record (wizard mode names every hero "wizard", so
use it with explore or normal mode).  The playground's sysconf must allow
the mode (WIZARDS, EXPLORERS).  Exits 0 if the replay checks out.
"""
import argparse
import fcntl
import os
import pty
import random
import re
import select
import shutil
import signal
import struct
import sys
import tempfile
import termios
import time

ENTRY = re.compile(rb"([a-z]{1,15}) ([0-9]{1,9}):")


def read_entries(path):
    """-> list of (tag, payload bytes); a record is a series of entries
    "TAG LENGTH:PAYLOAD\\n", LENGTH being the payload's length in bytes"""
    with open(path, "rb") as fp:
        data = fp.read()
    out = []
    pos = 0
    while pos < len(data):
        m = ENTRY.match(data, pos)
        if not m:
            raise ValueError("malformed record at byte %d" % pos)
        end = m.end() + int(m.group(2))
        if end >= len(data) or data[end:end + 1] != b"\n":
            raise ValueError("malformed record at byte %d" % pos)
        out.append((m.group(1).decode(), data[m.end():end]))
        pos = end + 1
    return out


def replay_record(pg, record, timeout=600):
    """run "nethack --replay RECORD --verify" in playground pg on a
    pseudo-terminal; -> (exit status, what it printed at the end)"""
    env = dict(os.environ, NETHACKDIR=pg, TERM="xterm", HOME=pg)
    pid, fd = pty.fork()
    if pid == 0:
        try:
            fcntl.ioctl(0, termios.TIOCSWINSZ,
                        struct.pack("HHHH", 24, 80, 0, 0))
            os.chdir(pg)
            os.execve("./nethack", ["./nethack", "--replay", record,
                                    "--verify"], env)
        finally:
            os._exit(127)
    out = b""
    last = time.time()
    status = None
    while status is None:
        r, _, _ = select.select([fd], [], [], 1.0)
        if r:
            try:
                data = os.read(fd, 65536)
            except OSError:
                data = b""
            if data:
                out += data
                last = time.time()
        wpid, wstatus = os.waitpid(pid, os.WNOHANG)
        if wpid:
            status = wstatus
        elif time.time() - last > timeout:
            os.kill(pid, signal.SIGKILL)
            os.waitpid(pid, 0)
            status = -1
    os.close(fd)
    text = re.sub(rb"\x1b\[[0-9;?]*[A-Za-z]", b"", out).decode("latin-1")
    tail = [l.strip() for l in text.replace("\r", "").split("\n")
            if l.startswith("session ") or l.startswith("replay ")]
    code = (os.WEXITSTATUS(status) if status >= 0 and os.WIFEXITED(status)
            else -1)
    return code, tail

# ordinary keys, weighted; no saving, quitting, extended commands, shell
# escape, options or mode changes (the test does those itself)
KEYS = (["h", "j", "k", "l", "y", "u", "b", "n"] * 6
        + ["H", "J", "K", "L", "Y", "U", "B", "N"] * 2
        + ["s"] * 4 + ["20s"] * 4 + ["60s"] * 3 + ["_>.>"] * 6 + ["_<.<"]
        + [">"] * 3 + ["<"]
        + list(",eqrzwWTPRatfdixEpocF") + ["\033"] * 4 + ["\r"] * 3
        + [" "] * 2 + list("abcdefghijklmnopqrstuvwxyz") + ["y", "n"] * 3
        + list("*-.$"))


def copy_playground(src, dst):
    for f in os.listdir(src):
        p = os.path.join(src, f)
        if os.path.isfile(p) and not f[0].isdigit() and "lock" not in f:
            shutil.copy2(p, dst)
    os.makedirs(os.path.join(dst, "save"), exist_ok=True)


class Game:
    """the game, running in a pseudo-terminal"""

    def __init__(self, pg, home, name, seed, mode, record, login=False):
        env = dict(os.environ, HOME=home, NETHACKDIR=pg, TERM="xterm",
                   NH_RECORD=record,
                   NETHACKOPTIONS="seed:%s,!legacy,!news,!splash_screen,"
                                  "!tutorial" % seed)
        for v in ("NH_SEEDFUZZ",):
            env.pop(v, None)
        args = ["./nethack"]
        if login:
            env["USER"] = env["LOGNAME"] = name
        else:
            args += ["-u", name]
        args += {"explore": ["-X"], "wizard": ["-D"]}.get(mode, [])
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            try:
                fcntl.ioctl(0, termios.TIOCSWINSZ,
                            struct.pack("HHHH", 24, 80, 0, 0))
                os.chdir(pg)
                os.execve(args[0], args, env)
            finally:
                os._exit(127)
        self.alive = True
        self.tail = ""  # the end of what the game has shown

    def drain(self, secs=0.0):
        """read (and discard) the game's output; notice if it exited"""
        end = time.time() + secs
        while self.alive:
            r, _, _ = select.select([self.fd], [], [],
                                    max(0.0, end - time.time()))
            if not r:
                break
            try:
                data = os.read(self.fd, 65536)
                if not data:
                    self.alive = False
                self.tail = (self.tail
                             + data.decode("latin-1"))[-2000:]
            except OSError:
                self.alive = False
        if self.alive:
            pid, _ = os.waitpid(self.pid, os.WNOHANG)
            if pid:
                self.alive = False

    def send(self, s):
        """send keys, then wait until the game has shown what it will"""
        if self.alive:
            try:
                os.write(self.fd, s.encode())
            except OSError:
                self.alive = False
            end = time.time() + 1.0
            while self.alive and time.time() < end:
                before = len(self.tail)
                r, _, _ = select.select([self.fd], [], [], 0.03)
                if not r:
                    break
                self.drain(0.0)
                if len(self.tail) == before and len(self.tail) < 2000:
                    break

    def finish(self, command, secs=60):
        """get out of whatever the game is doing and give it the command
        ("S" to save, "#quit\r" or ^C to quit, or none when it has been
        hung up on), answering its questions, until it exits"""
        end = time.time() + secs
        while self.alive and time.time() < end:
            self.tail = ""
            if command:
                # (at a prompt for text, the first Escape only clears what
                # has been typed)
                self.send("\033")
                self.send("\033")
                self.send(command)
            for _ in range(8):
                t = self.tail
                if not self.alive:
                    break
                if "Die?" in t or "Dump core" in t:
                    self.tail = ""; self.send("n")
                elif "Really save" in t or "Overwrite" in t \
                        or "Really quit" in t:
                    self.tail = ""; self.send("y")
                elif "[ynq]" in t or "--More--" in t or "(end)" in t:
                    self.tail = ""; self.send("\033")
                else:
                    break
            self.drain(0.3)
        if self.alive:
            shown = re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", " ", self.tail)
            print("the game didn't stop; it last showed:",
                  " ".join(shown.split())[-300:])
            os.kill(self.pid, signal.SIGKILL)
            self.alive = False
            return False
        return True


def main():
    ap = argparse.ArgumentParser(description=(__doc__ or "").split("\n")[0])
    ap.add_argument("playground")
    ap.add_argument("-k", type=int, default=3000, help="keys per session")
    ap.add_argument("-s", type=int, default=3, help="sessions")
    ap.add_argument("--mode", default="wizard",
                    choices=("normal", "explore", "wizard"))
    ap.add_argument("--seed", default="replaytest")
    ap.add_argument("--rand", type=int, default=1, help="random key seed")
    ap.add_argument("--signals", action="store_true",
                    help="interrupt the game, and hang up on it")
    ap.add_argument("--login", action="store_true",
                    help="name the hero from $USER instead of -u")
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()
    rng = random.Random(args.rand)
    work = tempfile.mkdtemp(prefix="nhreplaytest-")
    pg = os.path.join(work, "playground")
    os.makedirs(pg)
    record = os.path.join(work, "game.rec")
    copy_playground(os.path.abspath(args.playground), pg)
    home = os.path.join(work, "home")
    os.makedirs(home)
    name = "rtest"
    ended = False
    for n in range(args.s):
        g = Game(pg, home, name, args.seed, args.mode, record, args.login)
        g.drain(1.0)
        for _ in range(args.k):
            key = rng.choice(KEYS)
            if args.mode == "wizard" and rng.random() < 0.01:
                # (level teleport, so that many levels get visited)
                key = "\033\026%d\r" % rng.randint(1, 25)
            g.send(key)
            if args.signals and rng.random() < 0.01:
                # ^C, which the terminal turns into SIGINT, then "no" to
                # "Really quit?"
                g.tail = ""
                g.send("\003")
                end = time.time() + 3
                while ("Really quit" not in g.tail and g.alive
                       and time.time() < end):
                    g.drain(0.2)
                if "Really quit" in g.tail:
                    g.send("n")
            if "Die?" in g.tail and args.mode != "normal":
                g.send("n")  # (explore and wizard mode: carry on)
                g.tail = ""
            if not g.alive:
                ended = True  # the hero died (or quit)
                break
        if ended:
            break
        if args.signals and n < args.s - 1:
            # hang up in the middle of a long search; the game saves
            g.send("\03360s")
            os.kill(g.pid, signal.SIGHUP)
            ok = g.finish("")
        else:
            ok = g.finish("S" if n < args.s - 1 else
                          "\003" if args.signals else "#quit\r")
        if not ok:
            print("session %d: the game didn't stop" % (n + 1))
            break
    entries = read_entries(record) if os.path.exists(record) else []
    count = lambda tag: sum(t == tag for t, _ in entries)
    print("record: %d sessions, %d keys, %d seeds, %d checkpoints, %d"
          " hangups, %d interrupts, ends %s"
          % (count("session"), count("k"), count("s"), count("c"),
             count("hup"), count("intr"),
             [p.split()[0].decode() for t, p in entries if t == "end"]))
    checks = [p.split() for t, p in entries if t == "c"]
    if checks:
        print("reached turn %d; levels arrived on: %d"
              % (max(int(c[1]) for c in checks),
                 sum(c[0] == b"level" for c in checks)))
    code, tail = replay_record(pg, record)
    print("\n".join(tail) if tail else "(the replay printed no outcome)")
    if args.keep:
        print("kept in", work)
    else:
        shutil.rmtree(work, ignore_errors=True)
    return code


if __name__ == "__main__":
    sys.exit(main())
