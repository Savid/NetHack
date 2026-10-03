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
from contextlib import contextmanager
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
import time

import feedgame
from feedtest import replay
from layouttest import fnv, fold


OPTIONS = ("color,!legacy,!news,!splash_screen,!tutorial,!autopickup,"
           "!tips,!autodescribe")
SEED = "shutdown-probe"  # public fixture, never a server's race seed


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
        self.read_feed()

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


@contextmanager
def prepared_sysconf(source, pg):
    path = os.path.join(source, "sysconf")
    with open(path, "rb") as f:
        previous = f.read()
    for line in previous.decode().splitlines():
        key, separator, _ = line.partition("=")
        if separator and key.strip() in ("SEED", "RECORDFILE"):
            raise ValueError("use a test playground without SEED/RECORDFILE")
    policy = ("SHELLERS=\nWIZARDS=\nEXPLORERS=\nMAXPLAYERS=25\n"
              "DUMPLOGFILE=" + os.path.join(pg, "dump.log") + "\n")
    try:
        # The compiled sysconf wins over -d while it exists.  Updating both
        # paths covers dedicated local builds and relocated release assets.
        for target in (path, os.path.join(pg, "sysconf")):
            with open(target, "w") as f:
                f.write(policy)
        yield
    finally:
        with open(path, "wb") as f:
            f.write(previous)


def generate_layouts(binary, pg):
    results = []
    scratch = os.path.join(pg, ".layouts-test")
    os.mkdir(scratch, 0o700)
    try:
        for mode in ("--layouts", "--layout-hashes"):
            env = environment(pg)
            env["TMPDIR"] = scratch
            result = subprocess.run([binary, "-d", pg, mode, "-"],
                                    input=(SEED + "\n").encode(),
                                    stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE, env=env, cwd=pg,
                                    timeout=30)
            assert result.returncode == 0, mode + " refused the launch seed"
            assert not result.stderr, mode + " wrote diagnostics"
            assert 0 < len(result.stdout) <= 1 << 20, \
                "dump exceeds size limit"
            assert result.stdout.endswith(b"\n"), "incomplete layout dump"
            assert not os.listdir(scratch), "layout scratch was not cleaned"
            results.append(result.stdout)
    finally:
        os.rmdir(scratch)
    full, brief = [[json.loads(x) for x in data.splitlines()]
                   for data in results]
    assert full[-1] == brief[-1], "layout forms disagree"
    assert full[0]["form"] == "full" and brief[0]["form"] == "hashes"
    before_end = results[0][:results[0].rindex(b'{"k":"end"')]
    assert fnv(before_end) == full[-1]["hash"]
    levels = {(x["dn"], x["dl"]): x for x in full if x["k"] == "level"}
    hashes = {(x["dn"], x["dl"]): x["layout"]
              for x in brief if x["k"] == "level"}
    assert len(levels) == full[-1]["levels"] and len(levels) > 50
    assert {key: x["layout"] for key, x in levels.items()} == hashes
    return full[0], levels


def snapshot(g):
    start = len(g.events())
    assert g.signal(signal.SIGUSR1), "game exited before keyframe request"
    return wait_for(g, lambda: next((x for x in g.events()[start:]
                    if x["k"] == "kf" and x["why"] == "signal"), None),
                    "requested keyframe")


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
    pg = os.path.join(work, scenario)
    feedgame.copy_playground(pg, source)
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
            assert hdr["mode"] == "normal" and hdr["seed"] == SEED
            assert hdr["restored"] == 0 and hdr["build"] == header["build"]
            # Header delivery is the launcher's earliest safe signal point,
            # even if the game has not reached its first command yet.
            snapshot(g)
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
            assert final["why"] == ("death" if scenario == "death" else "end")
            problems, nkf, checked, _, _ = fold(events, levels, False)
            assert not problems, ("layout reconstruction failed: "
                                  + "; ".join(problems))
            assert nkf >= 2 and checked >= 1, "no snapshots reconstructed"
            if scenario != "death":
                assert os.listdir(os.path.join(pg, "save")), \
                    "hangup omitted save"
            code, outcome = replay(pg, os.path.join(pg, "session.nhrec"),
                                   timeout=30)
            verified = any("replay verified" in x for x in outcome)
            assert code == 0 and verified, \
                "managed recording failed replay verification"
        finally:
            g.close()
            if g.wait(2) is None:
                g.signal(signal.SIGKILL)
                g.wait(5)
            g.feed_thread.join(5)
            with open(os.path.join(pg, "feed.jsonl"), "wb") as f:
                f.write(g.feed)
            with open(os.path.join(pg, "terminal.txt"), "w") as f:
                f.write(g.tail)
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
        for scenario in ("playing", "menu", "line", "death"):
            check(source, work, scenario)


if __name__ == "__main__":
    main()
