#!/usr/bin/env python3
"""A game whose record can't be written ends cleanly and saved (the
session's header on /dev/full, where a restored game keeps its save file
as it was and an interrupt can't quit it; a later key once the record
can't grow past the file size limit), a replay never writes into the
playground, malformed records are rejected, a game that goes on over an
old game's files (destroying or recovering them, or past a panic save)
has a record that verifies, and an unseeded game, which isn't recorded,
keeps its save file when it's hung up while restored.  Needs the same
sysconf and permissions as replaytest.py.
"""
import argparse
import fcntl
import gzip
import os
import resource
import signal
import stat
import time

import feedgame
from feedtest import replay

NAME = "recfail"
PAD = 4 << 20  # record padding; a save file must fit under it


def wait_exit(g, secs=30):
    """the game's exit status, answering --More-- on the way"""
    end = time.time() + secs
    while g.status is None and time.time() < end:
        g.drain(0.1)
        g.reap()
        if g.alive and "--More--" in g.tail:
            g.seen = getattr(g, "seen", "") + g.tail
            g.tail = ""
            g.send(" ")
    return g.status


def shown(g):
    """everything the game wrote to the terminal"""
    return getattr(g, "seen", "") + g.screen()


def game(pg, record, limit=None, mode="normal"):
    """the game recorded to record; with limit, its writes past limit
    bytes fail (SIGXFSZ ignored) as on a full disk"""
    if limit is not None:
        soft, hard = resource.getrlimit(resource.RLIMIT_FSIZE)
        old = signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
        resource.setrlimit(resource.RLIMIT_FSIZE, (limit, hard))
    try:
        g = feedgame.Game(pg, NAME, NAME, mode=mode, record=record)
    finally:
        if limit is not None:
            resource.setrlimit(resource.RLIMIT_FSIZE, (soft, hard))
            signal.signal(signal.SIGXFSZ, old)
    g.read_feed()
    return g


def at_command(g, secs=30):
    """past the welcome prompts, waiting for the first command"""
    return g.first_command(secs=secs)


def saved(pg):
    return bool(os.listdir(os.path.join(pg, "save")))


def save_files(pg):
    """the save directory's files: their modes and uncompressed contents
    (a gzip header can differ)"""
    out = {}
    d = os.path.join(pg, "save")
    for f in os.listdir(d):
        path = os.path.join(d, f)
        with open(path, "rb") as fp:
            data = fp.read()
        if f.endswith(".gz"):
            data = gzip.decompress(data)
        out[f] = (stat.S_IMODE(os.stat(path).st_mode), data)
    return out


def xlog_size(pg):
    p = os.path.join(pg, "xlogfile")
    return os.path.getsize(p) if os.path.exists(p) else 0


def until(g, text, secs=20):
    """answer --More-- until text is on the screen"""
    end = time.time() + secs
    while g.alive and time.time() < end and text not in g.screen():
        if "--More--" in g.tail:
            g.tail = ""
            g.send(" ")
        g.drain(0.1)
    return text in g.screen()


def check(source):
    with feedgame.scratch("nhrecfail-") as work:
        pg = os.path.join(work, "pg")
        feedgame.copy_playground(pg, source)
        with open(os.path.join(pg, ".nethackrc"), "w") as f:
            f.write("OPTIONS=!autopickup\n")  # recorded as rcfile
        record = os.path.join(work, "game.rec")

        g = game(pg, record)
        try:
            assert at_command(g), "the first game didn't start"
            g.send("10s", settle=2.0)
            g.finish(command="S")
            assert wait_exit(g) == 0 and saved(pg), (
                "the first game didn't save")
        finally:
            feedgame.close_game(g)
        with open(record, "rb") as f:
            good = f.read()
        assert good.startswith(b"session 3:new\n") and b"\nk " in good
        keys_at = good.index(b"\nk ") + 1
        good_copy = os.path.join(work, "good.rec")
        with open(good_copy, "wb") as f:
            f.write(good)

        # the header can't be written: the save file stays as it was, and
        # an interrupt at the refusal doesn't quit the game
        before = save_files(pg)
        logged = xlog_size(pg)
        for mode in ("normal", "explore"):
            g = game(pg, "/dev/full", mode=mode)
            try:
                assert until(g, "can't be recorded"), (
                    "no refusal on a record that can't be written: %r"
                    % shown(g))
                g.seen = g.tail
                g.signal(signal.SIGINT)
                g.drain(1.0)
                assert "Really quit" not in g.screen(), (
                    "an interrupt at the refused %s game's message asked"
                    " to quit" % mode)
                status = wait_exit(g, 60)
                assert status == 0 and saved(pg), (
                    "the refused %s game didn't save and exit cleanly: %r"
                    % (mode, status))
                assert save_files(pg) == before, (
                    "the refused %s game changed its save file" % mode)
                assert xlog_size(pg) == logged, (
                    "the refused %s game wrote an xlogfile entry" % mode)
            finally:
                feedgame.close_game(g)
        print("header write failure PASS", flush=True)

        # the header fits under the limit, the keys after it don't
        with open(record, "ab") as f:
            f.write(b"x" * (PAD - len(good)))
        g = game(pg, record, limit=PAD + keys_at + 120)
        try:
            assert at_command(g), "the restored game didn't start"
            for _ in range(40):
                if not g.alive:
                    break
                g.send("s", settle=0.3)
            status = wait_exit(g, 60)
            assert status == 0, "the game didn't end cleanly: %r" % status
            assert "record couldn't be written" in shown(g), (
                "the game didn't say the record failed: %r"
                % shown(g)[-400:])
            assert saved(pg), "the game wasn't saved"
        finally:
            feedgame.close_game(g)
        print("record write failure PASS", flush=True)

        markers = {}
        for nm in ("replay.results", "replay.nethackrc", "seedfuzz.txt"):
            markers[nm] = ("marker " + nm + "\n").encode()
            with open(os.path.join(pg, nm), "wb") as f:
                f.write(markers[nm])
        code, tail = replay(pg, good_copy, timeout=120)
        assert code == 0, "the good record didn't verify: %r" % tail
        for nm, text in markers.items():
            with open(os.path.join(pg, nm), "rb") as f:
                assert f.read() == text, "replay wrote " + nm
        print("replay scratch isolation PASS", flush=True)

        # a permission answer ("u") is 0 or 1
        bad = [("permission value",
                good[:keys_at] + b"u 1:5\n" + good[keys_at:])]
        key = good[keys_at:good.index(b"\n", keys_at) + 1]
        n = int(key.split(b":")[0].split()[1])
        bad.append(("embedded NUL", good.replace(
            key, b"k %d:" % n + b"1" + b"\0" * (n - 1) + b"\n", 1)))
        for what, data in bad:
            path = os.path.join(work, "bad.rec")
            with open(path, "wb") as f:
                f.write(data)
            code, tail = replay(pg, path, timeout=120)
            assert code != 0 and not any("verified" in l for l in tail), (
                "a record with a bad %s verified: %r" % (what, tail))
        print("malformed records rejected PASS", flush=True)


def check_old_games(source):
    """In debug mode (as at MAXPLAYERS 0) a killed game's files make the
    next game ask what to do with them; a game that goes on (y: destroy
    them, r: recover them) has a record that verifies, which the answer
    isn't in, since its replay is never asked; so has one started past a
    panic save ("Start a new game instead?")."""
    lock = "%dwizard" % os.getuid()
    for answer in ("y", "r", "panic"):
        with feedgame.scratch("nhrecfail-") as work:
            pg = os.path.join(work, "pg")
            feedgame.copy_playground(pg, source)
            first = os.path.join(work, "first.rec")
            record = os.path.join(work, "game.rec")
            if answer == "panic":
                open(os.path.join(pg, "save", lock + ".e"), "w").close()
                question = "Start a new game instead?"
            else:
                g = game(pg, first, mode="wizard")
                try:
                    assert at_command(g), "the first game didn't start"
                    g.send("5s", settle=1.0)
                    g.signal(signal.SIGKILL)
                    g.wait()
                finally:
                    feedgame.close_game(g)
                question = "Old game in progress"
                if answer == "r":
                    record = first  # the recovered game goes on in it
            g = game(pg, record, mode="wizard")
            try:
                assert until(g, question), (
                    "no %r: %r" % (question, g.screen()[-400:]))
                g.tail = ""
                g.send("y" if answer == "panic" else answer)
                assert at_command(g), "the game didn't go on"
                g.send("10s", settle=2.0)
                g.finish(command="S")
                assert wait_exit(g) == 0 \
                    and os.path.exists(os.path.join(pg, "save",
                                                    lock + ".gz")), (
                    "the game didn't save")
            finally:
                feedgame.close_game(g)
            code, tail = replay(pg, record, timeout=120)
            # (a recovered game's record has the killed game's session,
            # cut off)
            assert code == (2 if answer == "r" else 0) and tail \
                and tail[-2].startswith("session") \
                and "verified" in tail[-2], (
                "the record of a game that went on (%s) didn't verify: %r"
                % (answer, tail))
            # (a recovered game's replay ignores a stray r at its first
            # --More--, so look for it)
            with open(record, "rb") as f:
                data = f.read()
            last = data[max(data.rfind(b"\nsession "), 0):]
            assert b"\nk 3:114\n" not in last, (
                "the answer (%s) is in the record" % answer)
    print("games over an old game's files PASS", flush=True)


def check_unrecorded_hangup(source):
    """An unseeded game isn't recorded, though a record path is set; one
    hung up while it's restored (here while it waits for the perm lock)
    ends with its save file as it was."""
    opts = {"NETHACKOPTIONS": "!legacy,!news,!splash_screen,!tutorial,"
                              "!autopickup,role:Valkyrie,race:human,"
                              "gender:female,alignment:lawful"}
    with feedgame.scratch("nhrecfail-") as work:
        pg = os.path.join(work, "pg")
        feedgame.copy_playground(pg, source)
        record = os.path.join(work, "game.rec")
        g = feedgame.Game(pg, NAME, NAME, mode="normal", record=record,
                          extra_env=opts)
        g.read_feed()
        try:
            end = time.time() + 30
            while g.alive and time.time() < end \
                    and not feedgame.asked_for_command(g.feed):
                if "Is this ok?" in g.tail:
                    g.tail = ""
                    g.send("y")
                elif "--More--" in g.tail:
                    g.tail = ""
                    g.send(" ")
                g.drain(0.1)
            assert feedgame.asked_for_command(g.feed), (
                "the unseeded game didn't start: %r" % g.screen()[-400:])
            g.send("5s", settle=1.0)
            g.finish(command="S")
            assert wait_exit(g) == 0 and saved(pg), (
                "the unseeded game didn't save")
        finally:
            feedgame.close_game(g)
        assert not os.path.exists(record), "the unseeded game was recorded"
        before = save_files(pg)
        fd = os.open(os.path.join(pg, "perm"), os.O_RDWR)
        try:
            fcntl.lockf(fd, fcntl.LOCK_EX)
            g = feedgame.Game(pg, NAME, NAME, mode="normal", record=record,
                              extra_env=opts)
            g.read_feed()
            end = time.time() + 20
            while g.alive and time.time() < end \
                    and "Waiting for release" not in g.tail:
                g.drain(0.1)
            waited = "Waiting for release" in g.tail
            g.signal(signal.SIGHUP)
            g.drain(0.5)
        finally:
            fcntl.lockf(fd, fcntl.LOCK_UN)
            os.close(fd)
        try:
            assert waited, "the restore didn't wait for the perm lock"
            assert wait_exit(g) is not None, "the hung-up game didn't end"
            assert save_files(pg) == before, (
                "the hung-up restore changed its save file: %r"
                % sorted(save_files(pg)))
        finally:
            feedgame.close_game(g)
    print("unrecorded restore hung up PASS", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("playground")
    args = parser.parse_args()
    check(os.path.abspath(args.playground))
    check_old_games(os.path.abspath(args.playground))
    check_unrecorded_hangup(os.path.abspath(args.playground))


if __name__ == "__main__":
    main()
