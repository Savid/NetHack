#!/usr/bin/env python3
"""Check counted searches and rests after travel has ended.

The last step of travel used to leave context.run set; a counted search
then updated "T:" only every seventh turn (runmode "run", the default)
or never (runmode "teleport"), and not when it ended, so the game came
back for a command still showing an older turn.  This plays a seeded game
with the time option on in each of those run modes: travel a few squares,
then counted searches or rests, and after each command compares the turn
status line was drawn with against the feed's turn ("t").  With --gdb,
also check that recovering full health or energy interrupts the count.
Only disposable games are changed by the debugger; Linux is required.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import time

import feedgame

SEED = "11"      # its first room has space to travel four squares west
TRAVEL = "_@hhhh."
COUNTS = (2, 2, 2, 5, 20)
RUNMODES = ("run", "teleport")
RECOVERY = {
    "health": ("uhp", "REGENERATION", "You are in full health."),
    "energy": ("uen", "ENERGY_REGENERATION", "You feel full of energy."),
}


def heroes(g):
    return [json.loads(x) for x in bytes(g.feed).split(b"\n")[:-1]
            if x.startswith(b'{"k":"hero",')]


def command(g, keys):
    """send keys and wait for the game to come back for a command: the
    hero line of that action boundary (lines in between, once a turn of
    a longer action, have the action's own count "a"), and the turn last
    drawn on the status line"""
    action = heroes(g)[-1]["a"]
    shown = shown_turn(g)
    g.tail = ""
    g.send(keys)
    deadline = time.monotonic() + 10
    while g.alive and time.monotonic() < deadline:
        h = [x for x in heroes(g) if x["a"] > action]
        if h:
            g.drain(0.1)  # (the terminal was flushed before the feed)
            return h[0], shown_turn(g) or shown
        if "--More--" in g.tail:
            raise AssertionError("unexpected --More-- after %r" % keys)
        g.drain(0.05)
    raise AssertionError("no command boundary after %r" % keys)


def shown_turn(g):
    turns = re.findall(r"T:(\d+)", g.tail)
    return int(turns[-1]) if turns else None


def prepare_recovery(g, work, recovery):
    """leave one point to recover on the next turn, without a game
    command that would itself clear the leftover running state"""
    field, prop, _ = RECOVERY[recovery]
    script = os.path.join(work, "recovery.gdb")
    with open(script, "w") as f:
        f.write("set pagination off\nset confirm off\n")
        f.write("set var u.%s = u.%smax - 1\n" % (field, field))
        # A timed intrinsic lasts long enough for the whole fixture.
        f.write("set var u.uprops[%s].intrinsic = 1000\n" % prop)
        f.write("detach\nquit\n")
    result = subprocess.run(
        ["gdb", "-q", "-nx", "-batch", "-p", str(g.pid), "-x", script],
        capture_output=True, text=True, timeout=15)
    assert result.returncode == 0, "recovery setup failed: " + result.stderr


def check(source, runmode, activity="s", recovery=None):
    with feedgame.scratch("nhturn-") as work:
        pg = os.path.join(work, "pg")
        feedgame.copy_playground(pg, source)
        g = feedgame.Game(pg, "turncounter", SEED, mode="normal",
                          options="time,!tips,runmode:" + runmode,
                          debuggable=bool(recovery))
        g.read_feed()
        try:
            if not g.first_command(20):
                raise AssertionError("the game never asked for a command")
            start = heroes(g)[-1]
            hero, shown = command(g, TRAVEL)
            assert hero["x"] == start["x"] - 4 and hero["y"] == start["y"], (
                "travel didn't reach its square: %r -> %r"
                % ((start["x"], start["y"]), (hero["x"], hero["y"])))
            assert shown == hero["t"], (
                "runmode %s, after travel: T:%s shown, turn %d"
                % (runmode, shown, hero["t"]))
            if recovery:
                prepare_recovery(g, work, recovery)
            for count in ((20,) if recovery else COUNTS):
                keys = str(count) + activity
                before = hero["t"]
                hero, shown = command(g, keys)
                assert hero["t"] > before, "%r took no time" % keys
                assert shown == hero["t"], (
                    "runmode %s, after %r: T:%s shown, turn %d"
                    % (runmode, keys, shown, hero["t"]))
                if recovery:
                    assert RECOVERY[recovery][2] in g.screen(), (
                        "full %s did not interrupt %r after travel"
                        % (recovery, keys))
                    assert hero["t"] - before < count, (
                        "full %s left %r running for the whole count"
                        % (recovery, keys))
        finally:
            g.close()
            if g.wait(5) is None:
                os.kill(g.pid, signal.SIGKILL)
                g.wait(5)
            g.feed_thread.join(5)
    print("runmode %s: travel then %s%s PASS"
          % (runmode, "search" if activity == "s" else "rest",
             " with full " + recovery if recovery else " turn counter"),
          flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("playground")
    parser.add_argument("--gdb", action="store_true",
                        help="also test recovery interruption (Linux/gdb)")
    args = parser.parse_args()
    if args.gdb and (not sys.platform.startswith("linux")
                     or not shutil.which("gdb")):
        parser.error("--gdb requires Linux and gdb")
    source = Path(args.playground).resolve()
    config = (source / "sysconf").read_text()
    assert not re.search(r"^\s*(SEED|RECORDFILE)\s*=", config, re.M), (
        "use a test playground's sysconf")
    assert not (source.joinpath("nethack").stat().st_mode & 0o6000), (
        "use an unprivileged playground binary")
    for runmode in RUNMODES:
        for activity in ("s", "."):
            check(str(source), runmode, activity)
            if args.gdb:
                for recovery in RECOVERY:
                    check(str(source), runmode, activity, recovery)


if __name__ == "__main__":
    main()
