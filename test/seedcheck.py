#!/usr/bin/env python3
"""Check the seeded startup and save/restore contract in real tty games.

Uses the supplied playground's sysconf temporarily, like layouttest.py.
Run serially against the binary's own compiled-in playground (or a
relocated installation whose compiled-in playground no longer exists).
"""
import argparse
from contextlib import contextmanager
from pathlib import Path
import re
import shutil
import signal

import feedgame
from layouttest import Feed, SysconfLine, layouts, take_stairs
from replaytest import read_entries, replay_record
from sessiontest import wait_for


OPTIONS = "!legacy,!news,!splash_screen,!tutorial,!autopickup"
SEEDS = ("seedcheck", "fuzz0", "fuzz1")  # public fixtures, never a race seed
CHARACTER = ("role", "race", "gender", "align", "god", "quest")


@contextmanager
def game(source, pg, seed, mode="normal", restored=False, options="", args=(),
         record=None, name="seedcheck"):
    # An installed build reads the compiled-in sysconf; a relocated build
    # reads the playground's.  Both must see this fixture's configuration.
    shutil.copyfile(source / "sysconf", pg / "sysconf")
    opts = OPTIONS + (",seed:" + seed if seed else "")
    opts += "," + (options or "role:Valkyrie,race:human,gender:female,"
                             "align:lawful")
    g = feedgame.Game(str(pg), name, "", mode=mode, record=record,
                      extra_args=args,
                      extra_env={"NETHACKOPTIONS": opts,
                                 "ROGUEOPTS": "name=outside input"})
    feed = Feed(g)
    try:
        assert g.first_command(), "game never reached its first command"
        hdr = next(x for x in feed.lines if x["k"] == "hdr")
        command = next(x for x in feed.lines
                       if x["k"] == "hero" and x["a"] > 0)
        # The hero line precedes the large keyframe.  A slow pipe reader
        # can have one without the other; wait for the matching complete
        # JSON line before taking the immutable snapshot.
        kf = wait_for(g, lambda: next((x for x in feed.lines
                      if x["k"] == "kf" and x["a"] == command["a"]), None),
                      "the first command's keyframe")
        assert hdr["mode"] == mode, "requested mode was not granted"
        assert bool(hdr["restored"]) == restored, "wrong new/restore path"
        yield g, feed, hdr, kf
    finally:
        g.close()
        if g.wait(5) is None:
            g.signal(signal.SIGKILL)
            g.wait(5)
        g.feed_thread.join(5)


def without_ids(value):
    if isinstance(value, dict):
        return {k: without_ids(v) for k, v in value.items() if k != "id"}
    if isinstance(value, list):
        return [without_ids(x) for x in value]
    return value


def snapshot(hdr, kf):
    """Only startup guarantees: no gameplay RNG or display state."""
    assert kf["t"] == 1, "startup fixture advanced a turn"
    assert len(kf["hero_x"]["attrs"]) == 6, "missing starting attributes"
    pets = [m for m in kf["level"]["monsters"] if m["tame"]]
    assert pets, "startup fixture never observed its pet"
    return {
        "character": {k: hdr["character"][k] for k in CHARACTER},
        "attributes": kf["hero_x"]["attrs"],
        "hp/power": [kf["hero"][k] for k in ("hpmax", "pwmax")],
        "inventory": without_ids(kf["inv"]["items"]),
        "pets": without_ids(pets),
        "discoveries": kf["disc"],
        "layout": kf["level"]["layout"],
    }


def same(a, b, fields=None):
    for field in fields or a:
        assert a[field] == b[field], "seed guarantee changed: " + field


def save(g, pg):
    g.finish("S", secs=10)
    assert g.status == 0, "save did not exit cleanly"
    assert any((pg / "save").iterdir()), "save was not created"


def fresh(source, work, label, seed, canonical=None, **settings):
    pg = Path(work) / label
    feedgame.copy_playground(pg, source)
    with game(source, pg, seed, **settings) as (g, feed, hdr, kf):
        assert hdr["seed"] == (seed if canonical is None else canonical), (
            "player seed was not used/canonicalized")
        result = snapshot(hdr, kf)
        save(g, pg)
    return result


def check_startup(source, work):
    layouts_seen = set()
    for n, seed in enumerate(SEEDS):
        baselines = {}
        for mode in ("normal", "explore", "wizard"):
            label = "%s-%d" % (mode, n)
            base = fresh(source, work, label, seed, mode=mode)
            assert base["inventory"], "baseline inventory was empty"
            # Options and command-line role/race disagree with each other;
            # none may override the seed, and pettype:none must not remove
            # the seeded pet.  A different hero name must not reroll it.
            other = fresh(
                source, work, label + "-options", seed, mode=mode,
                name="otherhero",
                options="role:Wizard,race:elf,gender:male,align:chaotic,"
                        "pettype:none,tutorial,fruit:durian",
                args=("-p", "Barbarian", "-r", "orc", "-@"))
            same(base, other)
            baselines[mode] = base
        layouts_seen.add(baselines["normal"]["layout"])
        # Explore/wizard add a wishing wand, and wizard generation may
        # differ.  Identity and attributes remain the seed's in all modes.
        for mode in ("explore", "wizard"):
            same(baselines["normal"], baselines[mode],
                 ("character", "attributes", "hp/power"))
        same(baselines["normal"], baselines["explore"], ("layout",))
        for handicap in ("pauper", "nudist"):
            got = fresh(source, work, "handicap-%s-%d" % (handicap, n),
                        seed, options=handicap)
            same(baselines["normal"], got,
                 ("character", "attributes", "hp/power", "layout"))
            if handicap == "pauper":
                assert not got["inventory"], "pauper fixture kept its kit"
            else:
                assert not any(o["cls"] == "[" for o in got["inventory"]), (
                    "nudist fixture kept armor")
        print("startup fixture %d: modes, options and handicaps PASS" % n,
              flush=True)
    assert len(layouts_seen) > 1, "different seeds produced the same layouts"
    for n, (given, canonical) in enumerate((("00042", "42"),
                                            ("seed\t  check", "seed check"))):
        a = fresh(source, work, "canonical-%d" % n, canonical)
        b = fresh(source, work, "spaced-%d" % n, given, canonical=canonical)
        same(a, b)
    print("numeric/text seed canonicalization PASS", flush=True)


def check_restore(source, work):
    for seeded in (False, True):
        pg = Path(work) / ("restore-seeded" if seeded else "restore-unseeded")
        feedgame.copy_playground(pg, source)
        seed = SEEDS[0] if seeded else ""
        with game(source, pg, seed) as (g, feed, hdr, kf):
            before = snapshot(hdr, kf)
            assert hdr["seed"] == seed, "wrong original seed"
            save(g, pg)
        # An unseeded save must stay unseeded, and a player's seeded save
        # must stay public even when restored in a hidden-seed installation.
        with SysconfLine(source, "SEED=different fixture"):
            with game(source, pg, "different option", restored=True) as data:
                g, feed, hdr, kf = data
                assert hdr["seed"] == seed, "restore changed saved seed"
                same(before, snapshot(hdr, kf))
                save(g, pg)
        print(("seeded" if seeded else "unseeded")
              + " restore under another server/player seed PASS", flush=True)


def check_hidden(source, work):
    pg = Path(work) / "hidden"
    feedgame.copy_playground(pg, source)
    record = str(Path(work) / "hidden.rec")
    seed = SEEDS[0]
    reference = fresh(source, work, "hidden-reference", seed)
    with SysconfLine(source, "SEED=" + seed):
        with game(source, pg, "ignored option", record=record) as data:
            g, feed, hdr, kf = data
            hidden = hdr["seed"]
            assert hidden.startswith("hidden#"), (
                "sysconf SEED was not read: use the compiled-in playground")
            same(reference, snapshot(hdr, kf))
            save(g, pg)
    # Preserve secrecy with the original server seed absent, changed, and
    # invalid.  Invalid server configuration must still permit restore.
    for server in (None, "another fixture", ""):
        with SysconfLine(source, "# no server seed" if server is None
                         else "SEED=" + server):
            with game(source, pg, "another option", restored=True,
                      record=record) as data:
                g, feed, hdr, kf = data
                assert hdr["seed"] == hidden, "restore lost hidden seed"
                same(reference, snapshot(hdr, kf))
                save(g, pg)
    entries = read_entries(record)
    assert [p for t, p in entries if t == "session"] == (
        [b"new"] + [b"restore"] * 3), "hidden record lost restore sessions"
    assert [p.split()[0] for t, p in entries if t == "end"] == [b"save"] * 4, (
        "hidden record lost saved session endings")
    shutil.copyfile(source / "sysconf", pg / "sysconf")
    code, _ = replay_record(str(pg), record, timeout=30)
    assert code == 1, "hidden record replayed without its server seed"
    code, _ = replay_record(str(pg), record, timeout=30,
                            extra_args=("--seed", "wrong fixture"))
    assert code == 1, "hidden record accepted the wrong server seed"
    code, _ = replay_record(str(pg), record, timeout=30,
                            extra_args=("--seed", seed))
    assert code == 0, "hidden multi-session record failed verification"
    print("hidden seed: startup, changed/absent/invalid sysconf, replay PASS",
          flush=True)


def check_new_level(source, work):
    # Explore mode permits a deterministic route without a death ending
    # coverage early.  Startup and save/restore above also cover normal play.
    seed = SEEDS[0]
    levels, _ = layouts(str(source), seed, work)
    pg = Path(work) / "new-level"
    feedgame.copy_playground(pg, source)
    with SysconfLine(source, "SEED=" + seed):
        with game(source, pg, "ignored", mode="explore") as data:
            g, feed, hdr, kf = data
            assert hdr["seed"].startswith("hidden#"), "server seed not active"
            save(g, pg)
    with SysconfLine(source, "SEED=different fixture"):
        with game(source, pg, "changed", mode="explore",
                  restored=True) as data:
            g, feed, hdr, kf = data
            assert take_stairs(g, feed, levels, False), (
                "never generated a new level after restore")
            arrivals = [x for x in feed.lines if x["k"] == "kf"
                        and x["level"]["dl"] != 1]
            assert arrivals, "missing new level keyframe"
            for arrival in arrivals:
                lv = arrival["level"]
                assert lv["layout"] == levels[lv["dn"], lv["dl"]]["layout"], (
                    "new level used another seed after restore")
            save(g, pg)
    print("hidden seed: unvisited level after restore matches original PASS",
          flush=True)


def check_invalid_server(source, work):
    for n, value in enumerate(("", "x" * 64)):
        pg = Path(work) / ("invalid-%d" % n)
        with SysconfLine(source, "SEED=" + value):
            feedgame.copy_playground(pg, source)
            g = feedgame.Game(str(pg), "invalid", SEEDS[0], mode="normal")
            feed = Feed(g)
            try:
                wait_for(g, lambda: "sysconf's SEED isn't a valid seed"
                         in g.tail, "the invalid-server-seed refusal")
                g.finish("\033", secs=5)
                assert g.status == 256, "invalid server seed did not fail"
                assert not any(e["k"] in ("hdr", "kf") for e in feed.lines), (
                    "invalid server seed allowed a new game")
                assert not any((pg / "save").iterdir()), (
                    "invalid server seed created a save")
            finally:
                g.close()
                if g.wait(5) is None:
                    g.signal(signal.SIGKILL)
                    g.wait(5)
                g.feed_thread.join(5)
    print("invalid server seed: no fallback to player's seed PASS", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("playground")
    args = parser.parse_args()
    source = Path(args.playground).resolve()
    config = (source / "sysconf").read_text()
    if re.search(r"^\s*(SEED|RECORDFILE)\s*=", config, re.M):
        parser.error("use a test playground's sysconf (no SEED or RECORDFILE)")
    if (source / "nethack").stat().st_mode & 0o6000:
        parser.error("use an unprivileged playground binary")
    with feedgame.scratch("nhseedcheck-") as work:
        check_startup(source, work)
        check_restore(source, work)
        check_hidden(source, work)
        check_new_level(source, work)
        check_invalid_server(source, work)


if __name__ == "__main__":
    main()
