#!/usr/bin/env python3
"""Check that the status line's turn counter keeps up after travel.

The last step of travel leaves context.run set; a counted search after it
used to update "T:" only every seventh turn (runmode "run", the default)
or never (runmode "teleport"), and not when it ended, so the game came
back for a command still showing an older turn.  This plays a seeded game
with the time option on in each of those run modes: travel a few squares,
then counted searches, and after each command compares the last turn the
status line was drawn with against the feed's turn ("t").
"""
import argparse
import json
import os
import re
import signal
import time

import feedgame

SEED = "11"      # its first room has space to travel four squares west
TRAVEL = "_@hhhh."
SEARCHES = ("2s", "2s", "2s", "5s", "20s")
RUNMODES = ("run", "teleport")


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


def check(source, runmode):
    with feedgame.scratch("nhturn-") as work:
        pg = os.path.join(work, "pg")
        feedgame.copy_playground(pg, source)
        g = feedgame.Game(pg, "turncounter", SEED, mode="normal",
                          options="time,!tips,runmode:" + runmode)
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
            for keys in SEARCHES:
                before = hero["t"]
                hero, shown = command(g, keys)
                assert hero["t"] > before, "%r took no time" % keys
                assert shown == hero["t"], (
                    "runmode %s, after %r: T:%s shown, turn %d"
                    % (runmode, keys, shown, hero["t"]))
        finally:
            g.close()
            if g.wait(5) is None:
                os.kill(g.pid, signal.SIGKILL)
                g.wait(5)
            g.feed_thread.join(5)
    print("runmode %s: turn counter after travel and counted searches PASS"
          % runmode, flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("playground")
    args = parser.parse_args()
    for runmode in RUNMODES:
        check(os.path.abspath(args.playground), runmode)


if __name__ == "__main__":
    main()
