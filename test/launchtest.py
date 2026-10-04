#!/usr/bin/env python3
"""Check normal seeded games controlled by a trusted terminal launcher.

Exercise the native arguments, environment, tty setup and shutdown signals
directly. The game runs as this user with matching real/effective IDs;
no external service, privilege wrapper or VM is needed.

Use a dedicated build's playground (or an unpacked release whose compiled
sysconf no longer exists).  Its sysconf is temporarily replaced with the
launcher's strict policy and restored, so do not share this build
with another game or test.  Records, feeds and saves are never printed.
"""
import argparse
import fcntl
import json
import os
import pty
import pwd
import signal
import stat
import struct
import subprocess
import termios
import threading
import time

import feedgame
from feedtest import replay
from layouttest import fnv, fold, STEPS
from sysconf import assert_test_config, TemporarySysconf


OPTIONS = ("color,!legacy,!news,!splash_screen,!tutorial,!autopickup,"
           "!tips,!autodescribe")
SEED = "shutdown-probe"  # public fixture, never a server's race seed
# The prompt scenario's options file: swapping places with the pet moves
# it, then this message waits for a key in the middle of the turn.
SWAP_PROMPT = 'MSGTYPE=stop "You swap places with .*"\n'


def environment(pg):
    username = pwd.getpwuid(os.getuid()).pw_name
    return {"HOME": pg, "USER": username, "LOGNAME": username,
            "NETHACKDIR": pg, "PATH": "/usr/bin:/bin"}


class LauncherGame(feedgame.Game):
    """Start a normal managed game; reuse only the tty driver."""

    def __init__(self, binary, pg):
        env = environment(pg)
        env.update(TERM="xterm", NETHACK_FEED_FD="3",
                   NH_RECORD=os.path.join(pg, "session.nhrec"),
                   NETHACKOPTIONS="seed:" + SEED + "," + OPTIONS)
        args = [binary, "-d", pg, "--managed-session", "-u", "launchtest",
                "-@"]
        self.feed_fd, writer = os.pipe()
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            try:
                os.close(self.feed_fd)
                os.dup2(writer, 3, inheritable=True)
                if writer != 3:
                    os.close(writer)
                fcntl.ioctl(0, termios.TIOCSWINSZ,
                            struct.pack("HHHH", 24, 80, 0, 0))
                attrs = termios.tcgetattr(0)
                attrs[3] &= ~(termios.ISIG | termios.ECHO | termios.ECHONL)
                termios.tcsetattr(0, termios.TCSANOW, attrs)
                os.chdir(pg)
                os.execve(binary, args, env)
            finally:
                os._exit(127)
        os.close(writer)
        self.alive, self.tail, self.nread, self.status = True, "", 0, None
        self.parsed, self.rows = 0, []
        self.early_request = threading.Event()
        self.early_start, self.early_error = None, None
        self.read_feed()

    def read_feed(self):
        """Request the first snapshot in the header reader, without the
        command driver's polling delay.  Count completed rows before the
        signal so a frame already received cannot satisfy the request."""
        self.feed = bytearray()

        def run():
            try:
                while True:
                    data = os.read(self.feed_fd, 65536)
                    if not data:
                        break
                    self.feed.extend(data)
                    if not self.early_request.is_set() and b"\n" in self.feed:
                        hdr = json.loads(self.feed.split(b"\n", 1)[0])
                        assert hdr["k"] == "hdr", "feed did not begin with hdr"
                        self.early_start = self.feed.count(b"\n")
                        os.kill(self.pid, signal.SIGUSR1)
                        self.early_request.set()
            except Exception as error:
                self.early_error = error
                self.early_request.set()
            finally:
                os.close(self.feed_fd)

        self.feed_thread = threading.Thread(target=run, daemon=True)
        self.feed_thread.start()

    def events(self):
        data = bytes(self.feed[self.parsed:])
        end = data.rfind(b"\n") + 1
        self.rows.extend(json.loads(line) for line in data[:end].splitlines())
        self.parsed += end
        return self.rows


def wait_for(g, predicate, description, timeout=10):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        g.drain(0.02)
        if not g.alive:
            break
    raise AssertionError("did not reach " + description)


def prepared_sysconf(source, pg):
    path = os.path.join(source, "sysconf")
    with open(path, "rb") as f:
        previous = f.read()
    assert_test_config(previous, path)
    policy = ("SHELLERS=\nWIZARDS=\nEXPLORERS=\nMAXPLAYERS=25\n"
              "DUMPLOGFILE=" + os.path.join(pg, "dump.log") + "\n")
    # The compiled sysconf wins over -d while it exists.  Updating both
    # paths covers dedicated local builds and relocated release assets.
    return TemporarySysconf((path, os.path.join(pg, "sysconf")), policy)


def artifact(path, data):
    """Keep diagnostics private, even when they contain a dump or a seed."""
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "wb") as f:
        os.fchmod(f.fileno(), 0o600)
        f.write(data)


def generate_layouts(binary, pg):
    results = []
    scratch = os.path.join(pg, ".layouts-test")
    os.mkdir(scratch, 0o700)
    try:
        for mode in ("--layouts", "--layout-hashes"):
            env = environment(pg)
            env["TMPDIR"] = scratch
            try:
                result = subprocess.run([binary, "-d", pg, mode, "-"],
                                        input=(SEED + "\n").encode(),
                                        stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE, env=env,
                                        cwd=pg, timeout=30)
            except subprocess.TimeoutExpired as error:
                artifact(os.path.join(pg, mode[2:] + ".stdout"),
                         error.stdout or b"")
                artifact(os.path.join(pg, mode[2:] + ".stderr"),
                         error.stderr or b"")
                raise AssertionError(mode + " timed out; diagnostics in "
                                     + pg) from None
            artifact(os.path.join(pg, mode[2:] + ".stdout"), result.stdout)
            artifact(os.path.join(pg, mode[2:] + ".stderr"), result.stderr)
            assert result.returncode == 0, (
                mode + " exited %d; diagnostics in %s"
                % (result.returncode, pg))
            assert not result.stderr, mode + " wrote diagnostics in " + pg
            assert 0 < len(result.stdout) <= 1 << 20, \
                "dump exceeds size limit"
            assert result.stdout.endswith(b"\n"), "incomplete layout dump"
            assert not os.listdir(scratch), "layout scratch was not cleaned"
            results.append(result.stdout)
    finally:
        if not os.listdir(scratch):
            os.rmdir(scratch)
    full, brief = [[json.loads(x) for x in data.splitlines()]
                   for data in results]
    assert full[-1] == brief[-1], "layout forms disagree"
    assert full[0]["form"] == "full", "full dump has wrong form"
    assert brief[0]["form"] == "hashes", "hash listing has wrong form"
    assert full[0]["build"] and full[0]["build"] == brief[0]["build"], \
        "layout forms disagree on build identity"
    assert full[0]["datahash"] == brief[0]["datahash"], \
        "layout forms disagree on data identity"
    before_end = results[0][:results[0].rindex(b'{"k":"end"')]
    assert fnv(before_end) == full[-1]["hash"], "layout checksum differs"
    levels = {(x["dn"], x["dl"]): x for x in full if x["k"] == "level"}
    hashes = {(x["dn"], x["dl"]): x["layout"]
              for x in brief if x["k"] == "level"}
    assert len(levels) == full[-1]["levels"] and len(levels) > 50, \
        "layout count is incomplete or disagrees with its trailer"
    assert {key: x["layout"] for key, x in levels.items()} == hashes, \
        "layout hashes differ between full and brief forms"
    return full[0], levels


def snapshot(g, start=None):
    idle = start is None
    if idle:
        start = len(g.events())
        assert g.signal(signal.SIGUSR1), "game exited before keyframe request"
    # Arrival takes precedence over a signal in feed_sync(), and satisfies
    # that request: the response is a complete state, whatever its label.
    frame = wait_for(g, lambda: next((x for x in g.events()[start:]
                     if x["k"] == "kf"), None), "requested keyframe")
    assert not idle or frame["why"] == "signal", \
        "idle snapshot request did not produce a signal keyframe"
    assert all(frame["hero"][key] == frame["level"][key]
               for key in ("dn", "dl")), "snapshot hero and level disagree"
    return frame


def quiet(g, secs=0.3):
    """Wait until the feed has been idle for secs."""
    while True:
        size = len(g.feed)
        g.drain(secs)
        if len(g.feed) == size or not g.alive:
            return


def swap_prompt(g, idle, levels):
    """Swap places with the pet, then snapshot at the in-turn --More--.
    The pet has moved; the feed written before the wait must already
    fold to the snapshot, as a fork's child starts from that state."""
    hero = idle["hero"]
    pet = next((m for m in idle["level"]["monsters"] if m["tame"]
                and (m["x"] - hero["x"], m["y"] - hero["y"]) in STEPS),
               None)
    assert pet, "no pet beside the hero at the first command"
    mark = len(g.events())
    g.tail = ""
    g.send(STEPS[(pet["x"] - hero["x"], pet["y"] - hero["y"])], settle=0)
    wait_for(g, lambda: "--More--" in g.tail and any(
        x["k"] == "msg" and x["text"].startswith("You swap places with")
        for x in g.events()[mark:]), "the swap's --More--")
    quiet(g)
    cut = len(g.events())
    frame = snapshot(g)
    moved = next((m for m in frame["level"]["monsters"]
                  if m["id"] == pet["id"]), None)
    assert moved and (moved["x"], moved["y"]) == (hero["x"], hero["y"]), \
        "the pet did not move before the prompt"
    before = g.events()[:cut]
    _, _, checked, _, _ = fold(before, levels, False)
    problems, _, also, _, _ = fold(before + [frame], levels, False)
    assert not problems and also == checked + 1, (
        "snapshot at the prompt differs from the feed before it: "
        + "; ".join(problems))

    def next_command():
        if "--More--" in g.tail:
            g.tail = ""
            g.send(" ", settle=0)
        return any(x["k"] == "hero" and x["a"] > frame["a"]
                   for x in g.events())

    wait_for(g, next_command, "the command after the prompt")


def check_build(hdr, layout):
    # The feed exposes the raw git hash; build_id() in layout dumps also
    # supports source archives, where it uses version plus build time.
    if hdr["build"]:
        assert hdr["build"] == layout["build"], \
            "feed and layout git build identities differ"
    else:
        prefix = hdr["version"] + "-"
        assert layout["build"].startswith(prefix), \
            "layout build fallback does not match feed version"
        assert layout["build"][len(prefix):].isdigit(), \
            "layout build fallback omits numeric build time"


def death(g):
    """Wait in place until death; answer actual --More-- prompts only."""
    deadline = time.monotonic() + 90
    sent_at = 0
    while time.monotonic() < deadline and g.alive:
        if any(x["k"] == "ev" and x.get("ev") == "death"
               for x in g.events()):
            # Finish the death messages, then leave the actual disclosure
            # question unanswered.  No more keys are sent after that point.
            def disclosure():
                if "possessions identified" in g.tail:
                    return True
                if "--More--" in g.tail:
                    g.tail = ""
                    g.send(" ", settle=0)
                return False

            wait_for(g, disclosure, "death disclosure")
            return
        if "--More--" in g.tail:
            g.tail = ""
            g.send(" ", settle=0)
        elif time.monotonic() - sent_at > 0.2:
            g.send("9999.", settle=0)
            sent_at = time.monotonic()
        g.drain(0.02)
    raise AssertionError("normal game never reached death disclosure")


def shutdown(g):
    # The launcher keeps the feed reader while sending SIGHUP and closing
    # the tty.  Merely sending a signal can leave death disclosure waiting.
    assert g.signal(signal.SIGHUP), "game exited before trusted shutdown"
    g.close()
    assert g.wait(10) == 0, "trusted hangup did not exit cleanly"
    g.feed_thread.join(5)
    assert not g.feed_thread.is_alive(), "final feed did not close"


def check(source, work, scenario):
    config = os.path.join(source, "sysconf")
    with open(config, "rb") as f:
        assert_test_config(f.read(), config)
    pg = os.path.join(work, scenario)
    feedgame.copy_playground(pg, source)
    if scenario == "prompt":
        with open(os.path.join(pg, ".nethackrc"), "w") as f:
            f.write(SWAP_PROMPT)
    binary = os.path.join(source, "nethack")
    with prepared_sysconf(source, pg):
        header, levels = generate_layouts(binary, pg)
        g = LauncherGame(binary, pg)
        try:
            def started():
                hdr = next((x for x in g.events() if x["k"] == "hdr"), None)
                if not hdr and "--More--" in g.tail:
                    g.tail = ""
                    g.send(" ", settle=0)
                return hdr

            hdr = wait_for(g, started, "feed header")
            assert hdr["mode"] == "normal", "launch used the wrong game mode"
            assert hdr["seed"] == SEED, "launch used the wrong player seed"
            assert hdr["restored"] == 0, "fresh launch unexpectedly restored"
            check_build(hdr, header)
            # Header delivery is the launcher's earliest safe signal point,
            # even if the game has not reached its first command yet.
            wait_for(g, g.early_request.is_set, "header snapshot request")
            assert g.early_error is None, "feed reader failed; see artifacts"
            snapshot(g, start=g.early_start)
            assert g.first_command(10), "no first command boundary"
            if scenario == "death":
                g.tail = ""
                death(g)
                assert any(x["k"] == "kf" and x["why"] == "death"
                           for x in g.events()), "death omitted final keyframe"
            else:
                idle = snapshot(g)
                again = snapshot(g)
                assert idle == again, "idle snapshots changed state"
                if scenario == "menu":
                    mark = len(g.events())
                    g.send("i")
                    wait_for(g, lambda: any(x["k"] == "ui"
                             and x.get("ev") == "menu_open"
                             for x in g.events()[mark:]), "inventory menu")
                    assert snapshot(g)["menus"], "pending menu missing"
                elif scenario == "line":
                    mark = len(g.events())
                    g.tail = ""
                    g.send("#")
                    wait_for(g, lambda: "#" in g.screen() and any(
                        x["k"] == "key" and x["key"] == ord("#")
                        for x in g.events()[mark:]), "extended-command prompt")
                    pending = snapshot(g)
                    assert pending["t"] == idle["t"], "line input took a turn"
                elif scenario == "prompt":
                    swap_prompt(g, idle, levels)
                else:
                    g.tail = ""
                    g.send("#exploremode\r")
                    wait_for(g, lambda: "cannot access explore mode" in g.tail,
                             "strict sysconf explore refusal")
                    for command in ("#quit\r", "S"):
                        g.tail = ""
                        g.send(command)
                        wait_for(g, lambda:
                                 "The supervisor controls" in g.tail,
                                 "managed-session refusal")
                    # The launcher disables ISIG: Ctrl-C reaches the game as
                    # a byte rather than becoming a terminal SIGINT.
                    g.send("\003")
                    assert snapshot(g)["t"] == idle["t"], "refusal took a turn"
                snapshot(g)  # final request before the launcher's hangup
            shutdown(g)
            raw = bytes(g.feed)
            assert raw.endswith(b"\n"), "partial final feed line"
            assert max(map(len, raw.splitlines())) < 256 << 10, \
                "oversized feed row"
            events = g.events()
            assert events[-1]["k"] == "end", "missing session end"
            assert events[-1]["how"] == ("done" if scenario == "death"
                                        else "exit"), "wrong session outcome"
            final = [x for x in events if x["k"] == "kf"][-1]
            ending = "death" if scenario == "death" else "end"
            assert final["why"] == ending, \
                "last keyframe does not describe the expected ending"
            problems, nkf, checked, _, _ = fold(events, levels, False)
            assert not problems, ("layout reconstruction failed: "
                                  + "; ".join(problems))
            assert nkf >= 2 and checked >= 1, "no snapshots reconstructed"
            if scenario != "death":
                assert os.listdir(os.path.join(pg, "save")), \
                    "hangup omitted save"
            code, outcome = replay(pg, os.path.join(pg, "session.nhrec"),
                                   timeout=30)
            artifact(os.path.join(pg, "replay.txt"),
                     ("exit=%d\n" % code + "\n".join(outcome)).encode())
            # Zero means all recorded sessions verified.  The tty reader
            # can observe process exit before collecting its final text.
            assert code == 0, ("managed replay exited %d; diagnostics in %s"
                               % (code, pg))
        except AssertionError as error:
            raise AssertionError("launcher %s: %s; artifacts in %s"
                                 % (scenario, error, pg)) from None
        finally:
            g.close()
            if g.wait(2) is None:
                g.signal(signal.SIGKILL)
                g.wait(5)
            g.feed_thread.join(5)
            artifact(os.path.join(pg, "feed.jsonl"), bytes(g.feed))
            artifact(os.path.join(pg, "terminal.txt"), g.tail.encode())
            if g.early_error is not None:
                artifact(os.path.join(pg, "feed-error.txt"),
                         repr(g.early_error).encode())
        print("launcher %-7s PASS" % scenario, flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("playground")
    args = parser.parse_args()
    source = os.path.abspath(args.playground)
    mode = os.stat(os.path.join(source, "nethack")).st_mode
    if mode & (stat.S_ISUID | stat.S_ISGID):
        parser.error("use a playground whose game is not setuid/setgid")
    if os.getuid() != os.geteuid() or os.getgid() != os.getegid():
        parser.error("run with matching real and effective IDs")
    with feedgame.scratch("nhlaunch-") as work:
        for scenario in ("playing", "menu", "line", "prompt", "death"):
            check(source, work, scenario)


if __name__ == "__main__":
    main()
