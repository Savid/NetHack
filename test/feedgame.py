"""Drive a seeded NetHack game on a pseudo-terminal, with its live feed.

The game runs in a copy of the fork's playground, recording to its own
record file (NH_RECORD, so `nethack --replay FILE --verify` can check it)
and writing its live feed to a pipe (NETHACK_FEED_FD).  Keys come from a
seeded random policy, like the fork's test/replaytest.py, with some
travel-to-stairs thrown in so the hero gets around.
"""
import contextlib
import fcntl
import json
import os
import pty
import re
import select
import shutil
import signal
import struct
import subprocess
import tempfile
import termios
import threading
import time

NH = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PLAYGROUND = os.path.join(NH, "playground")

# what the game reads from the environment that changes how it plays or
# what it writes: a test game sets these itself or goes without
GAME_ENV = ("DEBUGFILES", "HACKDIR", "HACKOPTIONS", "MAIL", "MAILREADER",
            "NETHACKOPTIONS", "NETHACK_FEED_FD", "NETHACK_FEED_KF_EVERY",
            "NETHACK_USE_GDB", "NH_FEEDCHECK", "NH_HEAPLOG", "NH_RECORD",
            "NH_SEEDFUZZ", "NH_STATELOG", "ROGUEOPTS", "SHOPTYPE",
            "SPLEVTYPE", "TTYINV", "WIZKIT")

# nh_getenv() ignores a value longer than this; these hold paths
GETENV_MAX = 128
GETENV_PATHS = ("HOME", "NETHACKDIR", "NH_STATELOG")

# ordinary keys, weighted (from replaytest.py), plus travel to the stairs
KEYS = (["h", "j", "k", "l", "y", "u", "b", "n"] * 6
        + ["H", "J", "K", "L", "Y", "U", "B", "N"] * 3
        + ["s"] * 4 + ["20s"] * 2 + ["_>.>"] * 8 + ["_<.<"]
        + [">"] * 3 + ["<"]
        + list(",eqrzwWTPRatfdixEpocF") + ["\033"] * 4 + ["\r"] * 3
        + [" "] * 2 + list("abcdefghijklmnopqrstuvwxyz") + ["y", "n"] * 3
        + list("*-.$"))


# (read-only and large: linked, not copied, so a run of many forks doesn't
# fill the disk with copies of the game)
SHARED = ("nethack", "nhdat", "recover", "symbols", "license")


def game_env(**settings):
    """the environment for a test game: this one's, less GAME_ENV, with
    settings"""
    env = {k: v for k, v in os.environ.items() if k not in GAME_ENV}
    env.update(settings)
    for k in GETENV_PATHS:
        if k in env and len(os.fsencode(env[k])) > GETENV_MAX:
            raise ValueError("%s=%s is longer than the %d bytes the game"
                             " takes: use a shorter TMPDIR"
                             % (k, env[k], GETENV_MAX))
    return env


@contextlib.contextmanager
def scratch(prefix):
    """a scratch directory, removed afterwards unless what used it raised
    (then kept, and named)"""
    work = tempfile.mkdtemp(prefix=prefix)
    try:
        yield work
    except BaseException:
        print("scratch files kept in", work, flush=True)
        raise
    shutil.rmtree(work, ignore_errors=True)


def asked_for_command(feed):
    """whether a game's feed (bytes) has it asking for a command: a "hero"
    line with an action count"""
    return any(json.loads(x)["a"] > 0
               for x in bytes(feed).split(b"\n")[:-1]
               if x.startswith(b'{"k":"hero",'))


def copy_playground(dst, src=None):
    src = src or PLAYGROUND
    os.makedirs(dst, exist_ok=True)
    for f in os.listdir(src):
        p = os.path.join(src, f)
        # (not the backups `make update` leaves: *.old)
        if (os.path.isfile(p) and not f[0].isdigit() and "lock" not in f
                and not f.endswith(".old")
                and not f.startswith("sysconf.test-")):
            if f in SHARED:
                to = os.path.join(dst, f)
                if os.path.lexists(to):
                    os.unlink(to)
                os.symlink(os.path.realpath(p), to)
            else:
                shutil.copy2(p, dst)
    os.makedirs(os.path.join(dst, "save"), exist_ok=True)


class Game:
    """the game on a pty; feed_fd is the read end of its feed pipe"""

    def __init__(self, pg, name, seed, mode="explore", record=None,
                 feed=True, extra_env=None, options="", debuggable=False,
                 extra_args=()):
        settings = dict(HOME=pg, NETHACKDIR=pg, TERM="xterm",
                        NETHACKOPTIONS="seed:%s,!legacy,!news,"
                                       "!splash_screen,!tutorial,"
                                       "!autopickup" % seed
                                       + ("," + options if options else ""))
        if record:
            settings["NH_RECORD"] = record
        settings.update(extra_env or {})
        env = game_env(**settings)
        self.feed_fd = None
        wfd = None
        if feed:
            rfd, wfd = os.pipe()
            os.set_inheritable(wfd, True)
            env["NETHACK_FEED_FD"] = str(wfd)
            self.feed_fd = rfd
        args = ["./nethack", "-u", name]
        args += {"explore": ["-X"], "wizard": ["-D"]}.get(mode, [])
        args += list(extra_args)
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            try:
                if debuggable:
                    # gdb fixtures (Game.gdb()) attach from a sibling process.
                    import ctypes
                    libc = ctypes.CDLL(None)
                    if libc.prctl(0x59616d61, ctypes.c_ulong(-1).value,
                                  0, 0, 0) != 0:
                        os._exit(126)
                if self.feed_fd is not None:
                    os.close(self.feed_fd)
                fcntl.ioctl(0, termios.TIOCSWINSZ,
                            struct.pack("HHHH", 24, 80, 0, 0))
                os.chdir(pg)
                os.execve(args[0], args, env)
            finally:
                os._exit(127)
        if wfd is not None:
            os.close(wfd)
        self.alive = True
        self.tail = ""
        self.nread = 0
        self.status = None

    def ready(self, secs):
        """output waiting within secs (poll: select can't take a
        descriptor past 1024, and a run of many forks gets there)"""
        if self.fd is None or self.fd < 0:
            return False
        p = select.poll()
        p.register(self.fd, select.POLLIN | select.POLLHUP | select.POLLERR)
        return bool(p.poll(max(0.0, secs) * 1000))

    def close(self):
        """close the terminal (once the game is gone: a sandbox's
        descriptors are freed when it ends)"""
        fd, self.fd = self.fd, -1
        if fd is not None and fd >= 0:
            try:
                os.close(fd)
            except OSError:
                pass

    def drain(self, secs=0.0):
        end = time.time() + secs
        while self.alive:
            if not self.ready(end - time.time()):
                break
            try:
                data = os.read(self.fd, 65536)
                if not data:
                    self.alive = False
                self.nread += len(data)
                self.tail = (self.tail + data.decode("latin-1"))[-4000:]
            except OSError:
                self.alive = False
        self.reap()

    def reap(self):
        if self.status is not None:
            return
        pid, st = os.waitpid(self.pid, os.WNOHANG)
        if pid:
            self.status = st
            self.alive = False

    def wait(self, secs=10):
        """reap the game once it exits, waiting up to secs: its status"""
        end = time.time() + secs
        self.reap()
        while self.status is None and time.time() < end:
            time.sleep(0.02)
            self.reap()
        return self.status

    def send(self, s, settle=1.0):
        """send s, then read what the game writes until it has been quiet
        for 30 ms (at most settle seconds)"""
        if not self.alive:
            return
        try:
            os.write(self.fd, s.encode("latin-1"))
        except OSError:
            self.alive = False
        end = time.time() + settle
        while self.alive and time.time() < end:
            before = self.nread
            if not self.ready(0.03):
                break
            self.drain(0.0)
            if self.nread == before:
                break

    def screen(self):
        return re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", " ", self.tail)

    def signal(self, sig):
        """send sig to the game; False if it has already gone"""
        try:
            os.kill(self.pid, sig)
        except ProcessLookupError:
            return False
        return True

    def gdb(self, commands, timeout=15):
        """run gdb commands against a debuggable game (Linux), then
        detach: the finished gdb process"""
        with tempfile.NamedTemporaryFile("w", suffix=".gdb") as f:
            f.write("set pagination off\nset confirm off\n"
                    "set print elements 40\n")
            f.write("\n".join(commands) + "\ndetach\nquit\n")
            f.flush()
            return subprocess.run(
                ["gdb", "-q", "-nx", "-batch", "-p", str(self.pid),
                 "-x", f.name],
                text=True, capture_output=True, timeout=timeout)

    def read_feed(self):
        """collect the feed into self.feed as it comes, on a thread
        (self.feed_thread) that ends when the game closes the pipe"""
        fd = self.feed_fd
        if fd is None:
            raise ValueError("the game was started without a feed")
        self.feed = bytearray()

        def run():
            try:
                while True:
                    data = os.read(fd, 65536)
                    if not data:
                        break
                    self.feed.extend(data)
            finally:
                os.close(fd)
        self.feed_thread = threading.Thread(target=run, daemon=True)
        self.feed_thread.start()

    def first_command(self, secs=20):
        """answer --More-- (and, restoring in explore or debug mode, not
        keeping the save file) until read_feed()'s feed has the game
        asking for its first command; False if it never does"""
        end = time.time() + secs
        while self.alive and time.time() < end:
            if asked_for_command(self.feed):
                return True
            if "keep the save file" in self.tail:
                self.tail = ""
                self.send("n")
            elif "--More--" in self.tail:
                self.tail = ""
                self.send(" ")
            self.drain(0.1)
        return False

    def finish(self, command="#quit\r", secs=30):
        end = time.time() + secs
        while self.alive and time.time() < end:
            self.tail = ""
            if command:
                self.send("\033")
                self.send("\033")
                self.send(command)
            for _ in range(12):
                t = self.tail
                if not self.alive:
                    break
                if "Die?" in t or "Dump core" in t:
                    self.tail = ""; self.send("n")
                elif ("Really save" in t or "Overwrite" in t
                      or "Really quit" in t):
                    self.tail = ""; self.send("y")
                elif "[ynq]" in t or "--More--" in t or "(end)" in t:
                    self.tail = ""; self.send("\033")
                else:
                    break
            self.drain(0.3)
        if self.alive:
            os.kill(self.pid, signal.SIGKILL)
            self.alive = False
        self.wait()


DIRS = "hjklyubn"

# wizard mode: map the level (^F), travel to the stairs and take them, with
# running, fighting, picking up and using things along the way
WIZKEYS = (["\006_>.>"] * 6 + ["_<.<"] + ["\006_<.<"]
           + [d for d in DIRS] * 3 + [d.upper() for d in DIRS] * 3
           + ["F" + d for d in DIRS] * 2 + [",\r", ",", "s", "20s", "5s"]
           + ["e", "q", "r", "z", "w", "W", "T", "P", "R", "a", "t", "f",
              "d"]
           + ["\033"] * 6 + ["\r"] * 2)


# wizard mode: things a random walk never does, so the feed sees them
# (level changes by trap door, hole, digging, level teleport; terrain made
# and changed; polymorph, engulfing, blindness, hallucination, prayer,
# genocide, identification; monsters that carry and use things).  A list
# is sent step by step; "{L}" is the letter of the item just wished for,
# "{D}" a direction, "{N}" a dungeon level.  ^W wishes, ^G makes a monster,
# ^V level-teleports.
WISH = "\027"
MACROS = [
    [WISH + "blessed +0 pick-axe\r", "\033", "a{L}", ">", "\033"],
    [WISH + "wand of digging\r", "\033", "z{L}", ">", "\033"],
    [WISH + "trap door\r", "\033", ">", "\033"],
    [WISH + "hole\r", "\033", "\033"],
    [WISH + "level teleporter\r", "\033", "{D}", "{d}", "\033"],
    ["\026", "{N}\r", "\033"],
    [WISH + "cursed scroll of teleportation\r", "\033", "r{L}", "\033"],
    [WISH + "potion of hallucination\r", "\033", "q{L}", "\033"],
    [WISH + "potion of blindness\r", "\033", "q{L}", "\033"],
    [WISH + "uncursed scroll of genocide\r", "\033", "r{L}", "newt\r",
     "\033"],
    [WISH + "blessed scroll of identify\r", "\033", "r{L}", "\033",
     "\033"],
    [WISH + "fountain\r", "\033", "q", "y", "\033"],
    [WISH + "sink\r", "\033"], [WISH + "throne\r", "\033", "#sit\r"],
    [WISH + "altar\r", "\033"], [WISH + "tree\r", "\033"],
    [WISH + "7 daggers\r", "\033", "t{L}", "{D}", "\033"],
    [WISH + "bag of holding\r", "\033", "a{L}", "\033", "\033"],
    ["#polyself\r", "dwarf\r", "\033"],
    ["#polyself\r", "xorn\r", "\033"],
    ["\007", "dust vortex\r", "\033"], ["\007", "dwarf\r", "\033"],
    ["\007", "gnome lord\r", "\033"], ["\007", "nymph\r", "\033"],
    ["\007", "floating eye\r", "\033"],
    ["#pray\r", "y", "\033", "\033"],
    ["E", "-", "Elbereth\r", "\033"],
    ["\004{D}", "\033"], ["o{D}", "\033"], ["c{D}", "\033"],
    # what the feed's own reviews found it could disturb when naming things:
    # a tin of unset variety once known, a glowing Sting, a renamed type, a
    # leash (its monster's name), a shopkeeper while hallucinating
    [WISH + "tin\r", "\033", WISH + "wand of probing\r", "\033", "z{L}",
     ".", "\033", "\033"],
    [WISH + "Sting\r", "\033", "w{L}", "\033", "\007", "hill orc\r",
     "\033"],
    [WISH + "potion of sickness\r", "\033", "C", "o", "{L}", "foo\r",
     "\033", "C", "o", "{L}", "bar\r", "\033"],
    [WISH + "leash\r", "\033", "a{L}", "{D}", "\033"],
    ["\007", "shopkeeper\r", "\033"],
    # effects the feed follows (tmp_at()): a ray and its bounces, an
    # explosion, a broken wand, arrows in flight
    [WISH + "wand of fire\r", "\033", "z{L}", "{D}", "\033", "\033"],
    [WISH + "wand of cold\r", "\033", "z{L}", "{D}", "\033", "\033"],
    [WISH + "scroll of fire\r", "\033", "r{L}", "\033", "\033"],
    [WISH + "wand of striking\r", "\033", "a{L}", "y", "\033", "\033"],
    [WISH + "bow\r", "\033", "w{L}", "\033", WISH + "20 arrows\r", "\033",
     "f{L}", "{D}", "\033"],
]
MACRO_P = 0.1


def policy_key(rng, mode):
    """the next keys: a string, or (wizard mode, sometimes) a macro, a
    list of steps for send_keys()"""
    if mode == "wizard":
        if rng.random() < MACRO_P:
            m = rng.choice(MACROS)
            return [x.replace("{D}", rng.choice(DIRS))
                     .replace("{N}", str(rng.randint(1, 12))) for x in m]
        k = rng.choice(WIZKEYS)
        if k in ("e", "q", "r", "z", "w", "W", "T", "P", "R", "a", "t",
                 "f", "d"):
            k += rng.choice("abcdefghijklmnopqrstuvwxyz$-*") + "\033"
        return k
    return rng.choice(KEYS)


# "x - a blessed +0 pick-axe." (or "(weapon in hand)" etc.)
ITEM_RE = re.compile(r"(?:^|\s)([a-zA-Z]) - (?:an? |the |\d+ )")
BACK = dict(zip(DIRS, "lkjhnbyu"))


def send_keys(g, k, mode, settle=1.0):
    """send one policy decision: a string, or a macro's steps"""
    steps = [k] if isinstance(k, str) else k
    letter = "a"
    last_dir = "h"
    for step in steps:
        if "{L}" in step:
            m = ITEM_RE.findall(g.screen())
            letter = m[-1] if m else letter
            step = step.replace("{L}", letter)
        if "{d}" in step:
            step = step.replace("{d}", BACK.get(last_dir, "h"))
        if step[:1] in DIRS and len(step) == 1:
            last_dir = step
        if step.startswith(WISH):
            g.tail = ""
        for ch in step:
            g.send(ch, settle=settle)
            if "Die?" in g.tail and mode != "normal":
                # (explore and wizard mode: carry on)
                g.tail = g.tail.replace("Die?", "")
                g.send("n", settle=settle)
            if "Still climb?" in g.tail:
                # (don't leave the dungeon from its first level)
                g.tail = g.tail.replace("Still climb?", "")
                g.send("n", settle=settle)
        if not g.alive:
            return


def keys_text(k):
    return k if isinstance(k, str) else "".join(k)


def play(g, rng, keys, mode="explore", on_key=None):
    """send keys from the policy; stop early if the game ends"""
    for n in range(keys):
        send_keys(g, policy_key(rng, mode), mode)
        if on_key:
            on_key(n)
        if not g.alive:
            return False
    return True
