#!/usr/bin/env python3
"""Check the turn counter and counted actions once running is over.

This plays seeded games with the time option on, in runmode "run" and
"teleport".  In one the hero travels to an open door, then towards a
square diagonally off it (out of a doorway, travel takes one orthogonal
step and stops), and after each travel searches or rests with counts.  In
another an Archeologist digs into a wall with the fight and run prefixes.
Whenever the game asks for a command, the status line must show the
feed's turn ("t") and the hero must not be travelling, and each turn of
the dig must be drawn.  With --gdb, after each travel the hero is left one
point short of full health or energy, regenerating it, and a long search
or rest must stop on recovering it.  Only disposable games are changed by
the debugger; Linux is required.
"""
import argparse
import contextlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import sys
import time

import feedgame

SEED = "18"      # its first room has an open door seven squares east
DOOR = (7, 0)
DIG_SEED = "5"   # an Archeologist five squares east of a diggable wall
WALL = (-5, 0)
COUNTS = (2, 2, 2, 5, 20)
RUNMODES = ("run", "teleport")
RECOVERY = {
    "health": ("uhp", "REGENERATION", "You are in full health."),
    "energy": ("uen", "ENERGY_REGENERATION", "You feel full of energy."),
}


def feed_lines(g, prefix):
    return [json.loads(x) for x in bytes(g.feed).split(b"\n")[:-1]
            if x.startswith(prefix)]


def heroes(g):
    return feed_lines(g, b'{"k":"hero",')


def inventory(g):
    return feed_lines(g, b'{"k":"kf",')[-1]["inv"]["items"]


def monster_squares(g):
    """the squares the feed has monsters on"""
    at = {}
    for x in feed_lines(g, b'{"k":"'):
        if x["k"] == "kf":
            at = {m["id"]: (m["x"], m["y"]) for m in x["level"]["monsters"]}
        elif x["k"] == "mon":
            at.update((m["id"], (m["x"], m["y"])) for m in x.get("upd", ()))
            for i in x.get("rm", ()):
                at.pop(i, None)
    return set(at.values())


def command(g, keys, more=False):
    """send keys and wait for the game to come back for a command: the
    hero line of that action boundary (lines in between, once a turn of
    a longer action, have the action's own count "a"), and the turn last
    drawn on the status line once it shows the feed's turn, or a second
    later (the game flushes the terminal after writing the feed); with
    more, --More-- is answered rather than failing"""
    action = heroes(g)[-1]["a"]
    shown = shown_turn(g)
    g.tail = ""
    g.send(keys)
    hero = None
    deadline = time.monotonic() + 10
    while g.alive and time.monotonic() < deadline:
        if hero is None:
            h = [x for x in heroes(g) if x["a"] > action]
            if h:
                hero = h[0]
                deadline = min(deadline, time.monotonic() + 1)
        if hero and shown_turn(g) == hero["t"]:
            break
        if "--More--" in g.tail:
            if not more:
                raise AssertionError("unexpected --More-- after %r" % keys)
            g.tail = g.tail.replace("--More--", "")
            g.send(" ")
        g.drain(0.05)
    if hero is None:
        raise AssertionError("no command boundary after %r" % keys)
    return hero, shown_turn(g) or shown


def shown_turn(g):
    turns = re.findall(r"T:(\d+)", g.tail)
    return int(turns[-1]) if turns else None


def travel_keys(dx, dy):
    """travel to the square dx, dy from the hero"""
    return ("_@" + ("l" * dx if dx > 0 else "h" * -dx)
            + ("j" * dy if dy > 0 else "k" * -dy) + ".")


def prepare_recovery(g, recovery):
    """leave the hero one point short of full health or energy, with
    enough regeneration for the rest of the game"""
    field, prop, _ = RECOVERY[recovery]
    result = g.gdb(["set var u.%s = u.%smax - 1" % (field, field),
                    "set var u.uprops[%s].intrinsic = 1000" % prop])
    assert result.returncode == 0, "recovery setup failed: " + result.stderr


def checked(g, keys, runmode, more=False):
    """command(), and what must hold whenever the game asks for one"""
    hero, shown = command(g, keys, more)
    assert shown == hero["t"], (
        "runmode %s, after %r: T:%s shown, turn %d"
        % (runmode, keys, shown, hero["t"]))
    assert "travel" not in hero, (
        "runmode %s, after %r: still travelling to %r"
        % (runmode, keys, hero["travel"]))
    return hero


def counts(g, hero, runmode, activity, recovery, after):
    """search or rest with counts where travel ended: the last hero"""
    if recovery:
        prepare_recovery(g, recovery)
    for count in ((20,) if recovery else COUNTS):
        keys = str(count) + activity
        before = hero
        hero = checked(g, keys, runmode)
        assert hero["t"] > before["t"], "%r took no time" % keys
        if recovery:
            assert RECOVERY[recovery][2] in g.screen(), (
                "full %s did not interrupt %r after %s"
                % (recovery, keys, after))
            assert hero["t"] - before["t"] < count, (
                "full %s left %r running for the whole count"
                % (recovery, keys))
    return hero


def travel_to(g, hero, square, runmode):
    """travel to a square, again if the pet stopped travel by being in
    the way: the hero there"""
    for _ in range(5):
        hero = checked(g, travel_keys(square[0] - hero["x"],
                                      square[1] - hero["y"]), runmode)
        if (hero["x"], hero["y"]) == square:
            return hero
    raise AssertionError("travel never reached %r" % (square,))


@contextlib.contextmanager
def playing(source, seed, runmode, debuggable=False):
    """a disposable game, asking for its first command"""
    with feedgame.scratch("nhturn-") as work:
        pg = os.path.join(work, "pg")
        feedgame.copy_playground(pg, source)
        g = feedgame.Game(pg, "turncounter", seed, mode="normal",
                          options="time,!tips,runmode:" + runmode,
                          debuggable=debuggable)
        g.read_feed()
        try:
            if not g.first_command(20):
                raise AssertionError("the game never asked for a command")
            yield g
        finally:
            g.close()
            if g.wait(5) is None:
                os.kill(g.pid, signal.SIGKILL)
                g.wait(5)
            g.feed_thread.join(5)


def check(source, runmode, activity="s", recovery=None):
    with playing(source, SEED, runmode, bool(recovery)) as g:
        hero = heroes(g)[-1]
        door = (hero["x"] + DOOR[0], hero["y"] + DOOR[1])
        hero = travel_to(g, hero, door, runmode)
        hero = counts(g, hero, runmode, activity, recovery,
                      "travel to the door")
        # travel to a square next to the hero can't leave a doorway
        # diagonally: it takes one orthogonal step instead and stops, unless
        # a monster on that square or the target changes the route
        step = (door[0] - 1, door[1])
        for _ in range(20):
            if not monster_squares(g) & {step, (step[0], step[1] - 1)}:
                break
            hero = checked(g, "s", runmode)
        else:
            raise AssertionError("monsters stayed beside the door")
        hero = checked(g, travel_keys(-1, -1), runmode)
        assert (hero["x"], hero["y"]) == step, (
            "travel off the door ended at %r, not %r"
            % ((hero["x"], hero["y"]), step))
        counts(g, hero, runmode, activity, recovery, "travel off the door")
    print("runmode %s: travel then %s%s PASS"
          % (runmode, "search" if activity == "s" else "rest",
             " with full " + recovery if recovery else " turn counter"),
          flush=True)


def check_dig(source, runmode):
    with playing(source, DIG_SEED, runmode) as g:
        hero = heroes(g)[-1]
        hero = travel_to(g, hero, (hero["x"] + WALL[0] + 1,
                                   hero["y"] + WALL[1]), runmode)
        pick = [o["let"] for o in inventory(g) if o["true"] == "pick-axe"]
        # (two messages: the pick-axe, and the bullwhip as alternate)
        hero = checked(g, "w" + pick[0], runmode, more=True)
        before = hero
        hero = checked(g, "FGh", runmode, more=True)
        drawn = {int(t) for t in re.findall(r"T:(\d+)", g.tail)}
        missing = sorted(set(range(before["t"] + 1, hero["t"])) - drawn)
        assert hero["t"] - before["t"] > 2, (
            "digging took %d turns" % (hero["t"] - before["t"]))
        assert not missing, (
            "runmode %s: turns %r of the dig weren't drawn"
            % (runmode, missing))
    print("runmode %s: dig turn counter PASS" % runmode, flush=True)


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
        check_dig(str(source), runmode)
        for activity in ("s", "."):
            check(str(source), runmode, activity)
            if args.gdb:
                for recovery in RECOVERY:
                    check(str(source), runmode, activity, recovery)


if __name__ == "__main__":
    main()
