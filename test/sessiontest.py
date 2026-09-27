#!/usr/bin/env python3
"""Check managed-session refusals, interrupts, hangup and replay on tty."""
import argparse
import json
import os
import signal
import tempfile
import termios
import threading
import time
from contextlib import contextmanager

import feedgame
from feedtest import replay


def events(raw):
    return [json.loads(line) for line in bytes(raw).split(b"\n")[:-1]]


def wait_for(g, predicate, description):
    deadline = time.monotonic() + 10
    while g.alive and time.monotonic() < deadline:
        result = predicate()
        if result:
            return result
        g.drain(0.05)
    raise AssertionError("timed out waiting for " + description)


def wait_for_exit(g):
    deadline = time.monotonic() + 5
    while g.status is None and time.monotonic() < deadline:
        g.reap()
        time.sleep(0.02)
    return g.status


@contextmanager
def session(pg, record=None, managed=True):
    log = os.path.join(pg, "state.log")
    options = "!legacy,!news,!splash_screen,!tutorial,!autopickup"
    options += ",disclose:-i -a -v -g -c -o"
    options += ",seed:sessiontest" if record else (
        ",role:Valkyrie,race:human,gender:female,align:lawful")
    g = feedgame.Game(
        pg, "sessiontest", "", mode="normal", record=record,
        extra_args=["--managed-session"] if managed else [],
        extra_env={"NETHACKOPTIONS": options, "NH_STATELOG": log})
    raw = bytearray()

    def capture():
        try:
            while True:
                data = os.read(g.feed_fd, 65536)
                if not data:
                    break
                raw.extend(data)
        finally:
            os.close(g.feed_fd)

    thread = threading.Thread(target=capture, daemon=True)
    thread.start()
    try:
        def command_ready():
            # The welcome prompt precedes startup RNG draws.  Only the
            # first command boundary is a valid baseline for refusals.
            if any(e["k"] == "hero" and e["a"] > 0 for e in events(raw)):
                return True
            if "--More--" in g.tail:
                g.tail = ""
                g.send(" ")
            return False

        wait_for(g, command_ready, "the first command boundary")
        yield g, log, raw
    finally:
        g.close()
        if wait_for_exit(g) is None:
            os.kill(g.pid, signal.SIGKILL)
            os.waitpid(g.pid, 0)
        thread.join(5)


def state(g, log):
    # The log is written before a key is handled; Escape samples the
    # completed command without advancing play.
    def last_sample():
        if not os.path.exists(log):
            return []
        with open(log) as f:
            lines = f.readlines()
        return lines[-1].split() if lines and lines[-1].endswith("\n") else []

    previous = last_sample()
    sequence = int(previous[0]) if previous else 0
    g.send("\033")

    def sampled():
        sample = last_sample()
        if sample and int(sample[0]) > sequence and sample[1] == "27":
            return tuple(sample[2:5])
        return None

    return wait_for(g, sampled, "the Escape state sample")


def refuse(g, log, raw, command=None):
    before = state(g, log)
    g.tail = ""
    if command is None:
        os.kill(g.pid, signal.SIGINT)
    else:
        g.send(command)
    wait_for(g, lambda: "The supervisor controls" in g.tail, "the refusal")
    assert "--More--" not in g.tail, "refusal requested another key"
    assert "Really" not in g.tail, "refusal asked for confirmation"
    after = state(g, log)
    assert g.alive and after == before, (
        "refusal advanced play/RNG: %r: %r -> %r" % (command, before, after))
    assert not any(e["k"] == "end" for e in events(raw)), "refusal ended game"


def hangup(g, pg):
    g.close()  # the supervisor drops the last PTY master
    assert wait_for_exit(g) == 0, "hangup did not exit cleanly"
    assert os.listdir(os.path.join(pg, "save")), "hangup did not save"


def check(source, recorded):
    with tempfile.TemporaryDirectory(prefix="nhsession-") as work:
        pg = os.path.join(work, "pg")
        feedgame.copy_playground(pg, source)
        with open(os.path.join(pg, ".nethackrc"), "w") as f:
            f.write('BINDINGS=Q:quit,V:save\n'
                    'MSGTYPE=stop "The supervisor controls*"\n')
        record = os.path.join(work, "game.rec") if recorded else None
        with session(pg, record) as (g, log, raw):
            for command in ("#quit\r", "S", "#save\r", "Q", "V"):
                refuse(g, log, raw, command)
            # Exercise rearming twice, in both raw and deferred handlers.
            refuse(g, log, raw)
            refuse(g, log, raw)
            # Literal Ctrl-C is a byte, distinct from an actual SIGINT.
            attrs = termios.tcgetattr(g.fd)
            attrs[3] &= ~termios.ISIG
            termios.tcsetattr(g.fd, termios.TCSANOW, attrs)
            before = state(g, log)
            g.send("\003")
            assert g.alive and state(g, log) == before
            hangup(g, pg)
        # Restoring with the flag reapplies it; without it normal saving
        # works, proving that the policy was not written into the save.
        with session(pg, record, managed=recorded) as (g, log, raw):
            if recorded:
                refuse(g, log, raw, "S")
                g.tail = ""
                g.send("<")  # still on the starting stairs
                assert "Still climb?" in g.tail, "not restored on stairs"
                g.send("y")
                g.finish(command="\033", secs=5)
                assert wait_for_exit(g) == 0, "normal escape failed"
            else:
                g.finish(command="S", secs=5)
                assert wait_for_exit(g) == 0, "restore could not save"
        if recorded:
            events = [json.loads(line) for line in raw.splitlines()]
            assert any(e["k"] == "end" and e["how"] == "done"
                       for e in events), "escape omitted the game-end event"
            code, outcome = replay(pg, record, timeout=30)
            assert code == 0, "restricted replay failed: " + str(outcome)
        print(("recorded" if recorded else "unrecorded")
              + " managed session PASS", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("playground")
    args = parser.parse_args()
    for recorded in (False, True):
        check(os.path.abspath(args.playground), recorded)


if __name__ == "__main__":
    main()
