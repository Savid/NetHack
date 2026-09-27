#!/usr/bin/env python3
"""Linux/gdb regression fixtures for feed error recovery and rendering.

Only disposable games are modified. Requires a debug build and WIZARDS=*.
Run: python3 test/feedfaults.py playground
"""
import argparse
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
import feedgame as nhgame


class Trial:
    def __init__(self, options="", feed=True, setup=None, race="human"):
        self.work = tempfile.mkdtemp(prefix="feedfaults-")
        self.pg = os.path.join(self.work, "pg")
        nhgame.copy_playground(self.pg)
        if setup:
            setup(self.pg)
        self.raw = bytearray()
        self.g = nhgame.Game(
            self.pg,
            "feedfaults",
            "",
            mode="wizard",
            feed=feed,
            debuggable=True,
            extra_env={
                "NETHACKOPTIONS": "role:Valkyrie,race:"
                + race
                + ",gender:female,align:lawful,!legacy,!news,!splash_screen,!tutorial,!autopickup"
                + ("," + options if options else "")
            },
        )
        if feed:

            def capture():
                try:
                    while True:
                        d = os.read(self.g.feed_fd, 65536)
                        if not d:
                            break
                        self.raw.extend(d)
                finally:
                    os.close(self.g.feed_fd)

            self.thread = threading.Thread(target=capture, daemon=True)
            self.thread.start()
        self.g.drain(0.5)
        for i in range(5):
            if "--More--" not in self.g.tail:
                break
            self.g.tail = ""
            self.g.send(" ")

    def debug(self, commands, timeout=15):
        p = os.path.join(self.work, "commands.gdb")
        with open(p, "w") as f:
            f.write(
                "set pagination off\nset confirm off\nset print elements 40\n"
            )
            f.write("\n".join(commands) + "\ndetach\nquit\n")
        return subprocess.run(
            ["gdb", "-q", "-nx", "-batch", "-p", str(self.g.pid), "-x", p],
            text=True,
            capture_output=True,
            timeout=timeout,
        )

    def data(self):
        good = []
        bad = []
        for ln in bytes(self.raw).split(b"\n")[:-1]:
            try:
                good.append(json.loads(ln))
            except Exception:
                bad.append(ln)
        return good, bad

    def close(self):
        if self.g.alive:
            self.g.finish(secs=5)
        if self.g.alive:
            os.kill(self.g.pid, signal.SIGKILL)
        self.g.close()
        if hasattr(self, "thread"):
            self.thread.join(2)
        shutil.rmtree(self.work)


def drive_debug(x, commands, seconds=20):
    box = []

    def work():
        try:
            box.append(x.debug(commands, timeout=seconds + 5))
        except Exception as e:
            box.append(e)

    th = threading.Thread(target=work)
    th.start()
    end = time.time() + seconds
    while th.is_alive() and time.time() < end:
        x.g.drain(0.05)
        if "--More--" in x.g.tail:
            x.g.tail = ""
            x.g.send(" ", settle=0.03)
        if "Report now?" in x.g.tail:
            x.g.tail = ""
            x.g.send("n", settle=0.03)
    th.join(6)
    if not box:
        raise RuntimeError("debugger did not complete")
    if isinstance(box[0], Exception):
        raise box[0]
    return box[0]


def check(name, test):
    x = Trial(options="showrace,color,pettype:cat", race="dwarf")
    try:
        test(x)
        assert not x.data()[1], "malformed JSON"
        print(name, "PASS", flush=True)
    finally:
        x.close()


def nested(x):
    r = x.debug(
        [
            'call (void) fb_begin("outer")',
            'call (void) fb_str("before", "one")',
            "set $n1 = (int) fb_nest()",
            'call (void) fb_begin("middle")',
            "set $n2 = (int) fb_nest()",
            "call (void) feed_key(32)",
            "call (void) fb_unnest($n2)",
            'call (void) fb_str("value", "two")',
            "call (void) fb_end()",
            "call (void) fb_unnest($n1)",
            'call (void) fb_str("after", "three")',
            "call (void) fb_end()",
            "call (void) feed_flush()",
        ]
    )
    assert not r.stderr, r.stderr
    time.sleep(0.1)
    d, b = x.data()
    assert any(
        v.get("k") == "outer"
        and v.get("before") == "one"
        and v.get("after") == "three"
        for v in d
    )
    assert any(v.get("k") == "middle" and v.get("value") == "two" for v in d)


def overrides(x):
    r = x.debug(
        [
            "set $sx = sizeof(gs.showsyms)/sizeof(gs.showsyms[0])-6",
            "set sysopt.accessibility = 1",
            "set go.ov_primary_syms[$sx+4] = 33",
            "set go.ov_primary_syms[$sx+5] = 64",
            "set gs.showsyms[$sx+4] = 33",
            "set gs.showsyms[$sx+5] = 64",
            "call (void) reset_glyphmap(gm_accessibility_change)",
            "call (void) feed_boundary()",
            "set feed_signalled = 1",
            "call (void) feed_boundary()",
        ]
    )
    assert not r.stderr, r.stderr
    time.sleep(0.1)
    d, b = x.data()
    kf = [v for v in d if v["k"] == "kf"][-1]
    assert kf["hero"]["screen"][1] == "h", kf["hero"]["screen"]
    pets = [v for v in kf["level"]["sym"] if v[3] == "kitten"]
    assert pets and all(v[1] == "f" for v in pets)


def transition(x):
    before = len(x.data()[0])
    x.g.tail = ""
    x.g.send("\026valley\r")
    x.g.drain(0.3)
    assert "--More--" in x.g.tail
    os.kill(x.g.pid, signal.SIGUSR1)
    x.g.drain(1.3)
    assert not any(v["k"] == "kf" for v in x.data()[0][before:])
    for _ in range(12):
        if "--More--" not in x.g.tail:
            break
        x.g.tail = ""
        x.g.send(" ")
        x.g.drain(0.1)
    d = x.data()[0][before:]
    lev = [i for i, v in enumerate(d) if v.get("ev") == "level"]
    kf = [i for i, v in enumerate(d) if v["k"] == "kf"]
    assert len(lev) == 1 and len(kf) == 1 and lev[0] < kf[0], (lev, kf)


def naming_fault(x):
    before = len(x.data()[0])
    result = drive_debug(
        x,
        [
            "set gi.invent->unpaid = 1",
            "call (void) feed_boundary()",
            "set gi.invent->unpaid = 0",
        ],
    )
    assert not result.stderr, result.stderr
    assert x.g.alive
    events = x.data()[0][before:]
    assert any("unpaid_cost" in v.get("text", "") for v in events)
    assert any(v.get("k") == "key" for v in events)


def idle_fault(x):
    result = x.debug(
        [
            'call (void) fb_begin("outer")',
            'call (void) fb_str("before", "one")',
            "set feed_signalled = 1",
            "set feed.waiting = 1",
            "call (void) feed_idle()",
            'call (void) fb_str("after", "two")',
            "call (void) fb_end()",
            "call (void) feed_flush()",
        ]
    )
    assert not result.stderr, result.stderr
    time.sleep(0.1)
    assert any(
        v.get("k") == "outer"
        and v.get("before") == "one"
        and v.get("after") == "two"
        for v in x.data()[0]
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("playground")
    args = parser.parse_args()
    if not sys.platform.startswith("linux") or not shutil.which("gdb"):
        parser.error("these fixtures require Linux and gdb")
    nhgame.PLAYGROUND = os.path.abspath(args.playground)
    for name, test in [
        ("naming error prompt", naming_fault),
        ("idle during a line", idle_fault),
        ("double nesting", nested),
        ("accessibility characters", overrides),
        ("level transition signal", transition),
    ]:
        check(name, test)


if __name__ == "__main__":
    main()
