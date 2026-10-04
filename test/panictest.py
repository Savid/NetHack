#!/usr/bin/env python3
"""Panic saves keep their error-save names, in seeded and unseeded games.

Run on Unix with WIZARDS=* and no server SEED, RECORDFILE or CRASHREPORTURL.
Only disposable games are used; core dumps are disabled.
"""
import argparse
import os
from pathlib import Path
import re
import resource
import signal
import time

import feedgame


def check(source, seed):
    with feedgame.scratch("nhpanic-") as work:
        pg = os.path.join(work, "pg")
        feedgame.copy_playground(pg, source)
        g = feedgame.Game(
            pg, "wizard", seed, mode="wizard",
            options="role:Valkyrie,race:human,gender:female,align:lawful")
        g.read_feed()
        try:
            # The status redraw precedes the welcome prompts, which can
            # consume #panic if it is sent before the first command.
            assert g.first_command(secs=30), (
                "the game didn't reach its first command")
            g.tail = ""
            g.send("#panic\r")
            end = time.monotonic() + 30
            confirmed = False
            while g.alive and time.monotonic() < end:
                g.drain(0.1)
                if "Do you want to call panic()" in g.tail:
                    confirmed = True
                    g.tail = ""
                    g.send("yes\r")
                elif "--More--" in g.tail or "Hit space" in g.tail:
                    g.tail = ""
                    g.send(" ")
            assert confirmed, "the wizard panic command wasn't offered"
            assert not g.alive, "panic didn't terminate the game"
            saves = list(Path(pg, "save").iterdir())
            assert len(saves) == 1, "panic didn't leave exactly one save"
            assert saves[0].name.endswith((".e", ".e.gz")), (
                "panic wrote a normal save instead of an error save")
            assert saves[0].stat().st_size > 0, "the error save is empty"
            print(("seeded" if seed else "unseeded")
                  + " panic save PASS", flush=True)
        finally:
            if g.status is None:
                if g.alive:
                    os.kill(g.pid, signal.SIGKILL)
                _, g.status = os.waitpid(g.pid, 0)
                g.alive = False
            g.close()
            g.feed_thread.join(5)
            assert not g.feed_thread.is_alive(), "feed reader did not close"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("playground")
    args = parser.parse_args()
    source = os.path.abspath(args.playground)
    config = Path(source, "sysconf").read_text()
    assert not re.search(r"^\s*(SEED|RECORDFILE|CRASHREPORTURL)\s*=",
                         config, re.M), "use a test playground's sysconf"
    old_limit = resource.getrlimit(resource.RLIMIT_CORE)
    try:
        resource.setrlimit(resource.RLIMIT_CORE, (0, old_limit[1]))
        for seed in ("", "panic-regression"):
            check(source, seed)
    finally:
        resource.setrlimit(resource.RLIMIT_CORE, old_limit)


if __name__ == "__main__":
    main()
