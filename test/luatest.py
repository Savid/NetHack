#!/usr/bin/env python3
"""Cached Lua interpreters must not suppress later configuration errors.

Uses disposable wizard games and the options menu, without a debugger.
Requires the same playground configuration as savecheck.py.
"""
import argparse
from pathlib import Path
import re
import tempfile
import time

import feedgame
from recordfail import wait_exit
from savecheck import at_command, close_game, game


def check(source, root, restored):
    pg = root / ("restored" if restored else "new")
    feedgame.copy_playground(str(pg), source)
    (pg / "symbols").unlink()
    (pg / "symbols").write_text(
        "start: broken\nDescription: error test\nnot_a_symbol:42\nfinish\n")
    if restored:
        g = game(pg, "wizard")
        try:
            assert at_command(g), "new game did not start"
            g.finish("S")
            assert wait_exit(g) == 0, "game did not save"
        finally:
            close_game(g)

    g = game(pg, "wizard")
    try:
        assert at_command(g, restored=restored), "game did not start/restore"
        g.tail = ""
        g.send("O")
        g.send(":symset\n")
        seen = ""
        deadline = time.monotonic() + 10
        while g.alive and time.monotonic() < deadline:
            g.drain(0.1)
            seen += g.screen()
            if "1 error in symbols" in seen:
                break
            if "--More--" in g.tail:
                g.tail = ""
                g.send(" ")
        assert "Unknown sym keyword" in seen, "configuration error hidden"
        assert "1 error in symbols" in seen, "configuration error not counted"
        print("restored" if restored else "new", "Lua error reporting PASS",
              flush=True)
    finally:
        close_game(g)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("playground")
    args = parser.parse_args()
    source = Path(args.playground).resolve()
    config = (source / "sysconf").read_text()
    assert not re.search(r"^\s*(SEED|RECORDFILE)\s*=", config, re.M), (
        "use a test playground's sysconf")
    assert not (source.joinpath("nethack").stat().st_mode & 0o6000), (
        "use an unprivileged playground binary")
    with tempfile.TemporaryDirectory(prefix="nhluatest-") as work:
        for restored in (False, True):
            check(str(source), Path(work), restored)


if __name__ == "__main__":
    main()
