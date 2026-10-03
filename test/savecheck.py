#!/usr/bin/env python3
"""Save refusals preserve the file, and recovery rejects malformed headers.

Uses disposable games with native saves. Requires WIZARDS=* and no server
SEED or RECORDFILE; run as the owner of an unprivileged playground.
"""
import argparse
import gzip
import json
import os
from pathlib import Path
import re
import signal
import stat
import subprocess
import sys
import time

import feedgame
from recordfail import wait_exit

SEED = "savecheck-fixture"
OPTIONS = "checkpoint,!ignintr"


def game(pg, mode="normal", seed=SEED):
    name = "wizard" if mode == "wizard" else "savecheck"
    options = OPTIONS
    if not seed:
        options += ",role:Valkyrie,race:human,gender:female,align:lawful"
    g = feedgame.Game(str(pg), name, seed, mode=mode, options=options)
    g.read_feed()
    return g


def at_command(g, restored=False):
    # The status redraw can precede the welcome, or be discarded when a
    # --More-- is answered.  Neither tells us when restore has finished.
    if not g.first_command(secs=30):
        return False
    headers = [json.loads(line) for line in bytes(g.feed).split(b"\n")[:-1]
               if line.startswith(b'{"k":"hdr",')]
    return len(headers) == 1 and bool(headers[0]["restored"]) == restored


def close_game(g):
    if g.status is None:
        if g.alive:
            os.kill(g.pid, signal.SIGKILL)
        _, g.status = os.waitpid(g.pid, 0)
        g.alive = False
    g.close()
    g.feed_thread.join(5)
    assert not g.feed_thread.is_alive(), "feed reader did not close"


def saved(pg):
    files = list((pg / "save").iterdir())
    assert len(files) == 1, "expected exactly one save"
    path = files[0]
    raw = path.read_bytes()
    if raw.startswith(b"\x1f\x8b"):
        raw = gzip.decompress(raw)
    assert raw[0] == ord("h"), "requires the native save format"
    return path, raw


def write_save(path, raw):
    path.write_bytes(gzip.compress(raw) if path.suffix == ".gz" else raw)


def refuse(pg, mode, seed, expected, interrupt=False):
    scores = {name: (pg / name).read_bytes() if (pg / name).exists() else None
              for name in ("record", "logfile", "xlogfile")}
    g = game(pg, mode, seed)
    seen = ""
    displayed = False
    try:
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            g.drain(0.1)
            g.reap()
            seen += g.screen()
            if not g.alive:
                break
            if expected in " ".join(seen.split()) and not displayed:
                displayed = True
                g.drain(0.2)
                assert g.alive, "refusal did not wait for acknowledgement"
                if interrupt:
                    os.kill(g.pid, signal.SIGINT)
                    g.drain(0.2)
                    seen += g.screen()
                    assert ("Really quit" not in seen
                            and "Switch from the tutorial" not in seen), (
                        "interrupt entered gameplay while refusing a save")
            if "--More--" in g.tail or "Hit space" in g.tail:
                g.tail = ""
                g.send(" ")
        assert expected in " ".join(seen.split()), (
            "expected refusal was not displayed")
        assert displayed, "refusal message disappeared without a prompt"
        status = wait_exit(g, secs=1)
        assert status is not None and os.WIFEXITED(status), (
            "save refusal crashed or did not exit")
        assert os.WEXITSTATUS(status) == 1, (
            "save refusal did not exit with status 1")
    finally:
        close_game(g)
    assert not any(re.search(r"\.\d+$", p.name) for p in pg.iterdir()), (
        "save refusal left a game lock or level file")
    for name, contents in scores.items():
        path = pg / name
        assert (path.read_bytes() if path.exists() else None) == contents, (
            "save refusal wrote a score or game log")


def check_saves(source, root, mode, seed=SEED):
    label = mode + ("-seeded" if seed else "-unseeded")
    base = root / (label + "-base")
    feedgame.copy_playground(str(base), source)
    g = game(base, mode, seed)
    try:
        assert at_command(g), "new game did not start"
        g.finish("S")
        assert wait_exit(g) == 0, "new game did not save"
    finally:
        close_game(g)
    path, original = saved(base)
    g = game(base, mode, seed)
    try:
        assert at_command(g, restored=True), "current save did not restore"
        g.finish("S")
        assert wait_exit(g) == 0, "restored game did not save"
    finally:
        close_game(g)
    print(mode, "current save restores PASS", flush=True)
    count = original[1]
    intsize = original[4]
    version_at = count + 2
    longsize = original[5]
    version = int.from_bytes(original[version_at:version_at + longsize],
                             sys.byteorder)
    variants = [
        ("revision", count + 1, bytes([(original[count + 1] + 1) % 256]),
         "save revision mismatch: file:%d, current:%d"
         % ((original[count + 1] + 1) % 256, original[count + 1])),
        ("version", version_at,
         (version + 1).to_bytes(longsize, sys.byteorder),
         "version mismatch: file:%08x, current:%08x" % (version + 1, version)),
    ]
    if seed:
        seed_field = seed.encode().ljust(64, b"\0")
        assert original.count(seed_field) == 1, "cannot locate fixture seed"
        offset = original.index(seed_field) + len(seed_field)
        genver = int.from_bytes(original[offset:offset + intsize],
                                sys.byteorder)
        variants.insert(0, (
            "generator", offset, (genver + 1).to_bytes(intsize, sys.byteorder),
            "generator version is %d; this build uses %d"
            % (genver + 1, genver)))
        invalid = b"savecheck  fixtur"
        assert len(invalid) == len(seed), "invalid seed changed field length"
        variants.insert(0, (
            "invalid-seed", original.index(seed_field), invalid,
            "The saved game's seed isn't valid."))
    for badcount in (count - 1, count + 1, 255):
        variants.append(("count-%d" % badcount, 1, bytes([badcount]),
                         "critical byte-count mismatch: file:%d, current:%d"
                         % (badcount, count)))
    for kind, at, value, expected in variants:
        pg = root / (label + "-" + kind)
        feedgame.copy_playground(str(pg), source)
        raw = bytearray(original)
        raw[at:at + len(value)] = value
        target = pg / "save" / path.name
        write_save(target, raw)
        # repeating the refusal checks the lock cleanup and server relaunch
        for interrupt in (False, True):
            refuse(pg, mode, seed, expected, interrupt=interrupt)
            kept, contents = saved(pg)
            assert contents == raw, "refusal changed or deleted the saved game"
            assert kept.name == path.name, (
                "refusal did not recompress the save")
            assert stat.S_IMODE(kept.stat().st_mode) & stat.S_IRUSR, (
                "refusal left the save unreadable")
        print(label, kind, "refusal/interrupt/relaunch PASS", flush=True)
    return original[:count + 2]


def check_recovery(source, root, header):
    base = root / "checkpoint"
    feedgame.copy_playground(str(base), source)
    g = game(base, "wizard")
    try:
        assert at_command(g), "checkpoint game did not start"
        g.send("s", settle=0.5)
    finally:
        close_game(g)
    checkpoints = {p.name: p.read_bytes() for p in base.iterdir()
                   if re.search(r"\.\d+$", p.name)}
    zero = next(name for name in checkpoints if name.endswith(".0"))
    assert checkpoints[zero].count(header) == 1, "checkpoint header not found"
    count_at = checkpoints[zero].index(header) + 1
    current = checkpoints[zero]
    header_at = count_at - 1
    version_at = header_at + len(header)
    intsize, longsize = header[4], header[5]
    name_at = version_at + 3 * longsize
    version = int.from_bytes(current[version_at:version_at + longsize],
                             sys.byteorder)
    variants = [("current", current)]
    for label, at, width, value in [
            ("revision", version_at - 1, 1, header[-1] + 1),
            ("version", version_at, longsize, version + 1),
            ("negative-name", name_at, intsize, -1),
            ("large-name", name_at, intsize, 1000000),
            ("count-255", count_at, 1, 255)]:
        data = bytearray(current)
        data[at:at + width] = value.to_bytes(width, sys.byteorder,
                                             signed=value < 0)
        variants.append((label, bytes(data)))
    # remove the entry too: the old reader must reach intact version data
    data = bytearray(current)
    data[count_at] -= 1
    del data[version_at - 1]
    variants.append(("short-count", bytes(data)))
    for label, checkpoint in variants:
        valid = label == "current"
        for builtin in (False, True):
            pg = root / ("recovery-%s-%d" % (label, builtin))
            feedgame.copy_playground(str(pg), source)
            expected = dict(checkpoints)
            data = bytearray(checkpoint)
            # a live PID reaches the prompt instead of stale-lock cleanup
            data[:intsize] = os.getpid().to_bytes(intsize, sys.byteorder)
            expected[zero] = bytes(data)
            for name, contents in expected.items():
                (pg / name).write_bytes(contents)
            if builtin:
                g = game(pg, "wizard")
                try:
                    seen = ""
                    recovered = False
                    deadline = time.monotonic() + 30
                    while time.monotonic() < deadline:
                        g.drain(0.1)
                        g.reap()
                        seen += g.screen()
                        if not g.alive:
                            break
                        if "Recover [r]" in g.tail or "=>" in g.tail:
                            g.tail = ""
                            g.send("r\n")
                            recovered = True
                        elif "--More--" in g.tail or "Hit space" in g.tail:
                            g.tail = ""
                            g.send(" ")
                        elif "keep the save file" in g.tail:
                            g.tail = ""
                            g.send("n")
                        elif (valid and recovered
                              and feedgame.asked_for_command(g.feed)):
                            break
                    if valid:
                        assert recovered, "built-in recovery was not offered"
                        assert at_command(g, restored=True), (
                            "built-in recovery did not restore checkpoint")
                        g.finish("S")
                        assert wait_exit(g) == 0
                    else:
                        assert "Couldn't recover old game" in seen, (
                            "built-in recovery did not refuse the header")
                        status = wait_exit(g, secs=1)
                        assert status is not None and os.WIFEXITED(status)
                        assert os.WEXITSTATUS(status) == 1
                finally:
                    close_game(g)
            else:
                result = subprocess.run(
                    [str(pg / "recover"), "-d", str(pg), zero[:-2]],
                    capture_output=True, timeout=30)
                if valid:
                    assert result.returncode == 0, "recover utility failed"
                    saved(pg)
                    g = game(pg, "wizard")
                    try:
                        assert at_command(g, restored=True), (
                            "recovered save did not restore")
                        g.finish("S")
                        assert wait_exit(g) == 0
                    finally:
                        close_game(g)
                else:
                    assert result.returncode >= 0, "recover utility crashed"
                    assert b"can't recover" in result.stderr, (
                        "recover utility did not refuse the header")
            if valid:
                print("built-in" if builtin else "utility",
                      "current checkpoint restores PASS", flush=True)
                continue
            assert not list((pg / "save").iterdir()), (
                "recovery wrote a save from a rejected checkpoint")
            assert all((pg / name).read_bytes() == contents
                       for name, contents in expected.items()), (
                "recovery changed or deleted a rejected checkpoint")
            print("built-in" if builtin else "utility", "recovery", label,
                  "preserves checkpoints PASS", flush=True)


def check_long_hackdir(source, root):
    """A NETHACKDIR or HACKDIR too long to use stops the game, which must
    not fall back to the compiled-in playground and save there; -d still
    names the playground instead."""
    pg = root / ("p" * 140)
    feedgame.copy_playground(str(pg), source)
    short = root / "pg-d"
    feedgame.copy_playground(str(short), source)
    for var in ("NETHACKDIR", "HACKDIR"):
        env = dict(os.environ, HOME=str(pg), TERM="xterm")
        env.pop("NETHACKDIR", None)
        env.pop("HACKDIR", None)
        env[var] = str(pg)

        def run(*args):
            return subprocess.run(["./nethack"] + list(args), cwd=pg,
                                  env=env, stdin=subprocess.DEVNULL,
                                  capture_output=True, text=True,
                                  timeout=30)
        result = run("-u", "savecheck")
        assert result.returncode == 1, result
        assert var + " is too long." in result.stdout, result
        result = run("-d", str(short), "--showpaths")
        assert result.returncode == 0, result
        assert "too long" not in result.stdout, result
        print("long", var, "refused, -d overrides PASS", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("playground")
    args = parser.parse_args()
    source = os.path.abspath(args.playground)
    config = Path(source, "sysconf").read_text()
    assert not re.search(r"^\s*(SEED|RECORDFILE)\s*=", config, re.M), (
        "use a test playground's sysconf")
    assert not (Path(source, "nethack").stat().st_mode & 0o6000), (
        "use an unprivileged playground binary")
    with feedgame.scratch("nhsavecheck-") as work:
        root = Path(work)
        header = check_saves(source, root, "normal")
        check_saves(source, root, "wizard")
        check_saves(source, root, "normal", seed="")
        check_recovery(source, root, header)
        check_long_hackdir(source, root)


if __name__ == "__main__":
    main()
