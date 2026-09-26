#!/usr/bin/env python3
"""Replay a recorded seeded game and check it.

A seeded game played with the tty interface is recorded when sysconf's
RECORDFILE is set (see "Seeding" in the top directory): everything the game
takes from outside (every random seed it draws from the system, every key it
reads, where it acted on a hangup or an interrupt), and digests of its state
every 100 turns, on arriving on a level and at the end of each session (a
save, or the end of the game).  This script plays each session of such a
record again, with the same build, and the game checks that every digest
comes out the same.

It runs the game in a copy of the playground, on a pseudo-terminal of the
recorded size, with no shell involved.  For each session it writes the
player's recorded options (the options file's text as ~/.nethackrc, and
NETHACKOPTIONS), and the game takes the recorded command-line arguments and
login name, seeds, keys and signals from the record itself (NH_REPLAY).  A
later session restores the save that the replay of the one before made.
Nothing is written outside the copy: the replay stops at the end of the game,
before the xlogfile, the high score list or a dumplog would be written.

The copy's sysconf is the installed one (or --sysconf's), without SEED,
RECORDFILE, DUMPLOGFILE, MSGHANDLER and CRASHREPORTURL; for a record of a
game with the server's hidden seed ("hidden#..."), SEED is set to the race's
seed: --seed's, else the one in --sysconf's file or the installed sysconf.
The replayed game checks it against the record's digest before anything is
replayed; a player's own seed must match the record's text.

Usage: replay.py [--seed SEED | --sysconf FILE] [--timeout SECS] [--keep]
                 RECORD PLAYGROUND

PLAYGROUND is the installed playground the game was played in.  Exits 0 only
if every session was checked and checks out; a session that ended without a
save or the end of the game (for example because the game was killed) is
replayed but can't be checked.  When a session fails, the sessions after it
aren't replayed: they restore the save it made, which already differs.
"""
import argparse
import fcntl
import os
import pty
import re
import select
import shutil
import signal
import struct
import sys
import tempfile
import termios
import time

HEADER_TAGS = ("name", "mode", "term", "seed", "reseed", "login", "rcfile",
               "envopts", "arg")
NEEDED = ("name", "mode", "term", "seed", "reseed")
EVENT_TAGS = ("s", "k", "u", "hup", "intr", "c", "end")
# the installed playground's files that the replay doesn't take: what a game
# writes (the replay starts them empty) and the sysconf (written anew)
VAR_FILES = ("record", "logfile", "xlogfile", "livelog", "paniclog", "perm")
# sysconf settings the replay drops, with the shortest abbreviation of
# each that the game takes (src/cfgfiles.c)
DROPPED_SYSCONF = (("SEED", 4), ("RECORDFILE", 10), ("DUMPLOGFILE", 7),
                   ("MSGHANDLER", 9), ("CRASHREPORTURL", 13))
ENTRY = re.compile(rb"([a-z]{1,15}) ([0-9]{1,9}):")


class RecordError(Exception):
    pass


def read_entries(path):
    """-> list of (tag, payload bytes); a record is a series of entries
    "TAG LENGTH:PAYLOAD\\n", LENGTH being the payload's length in bytes"""
    with open(path, "rb") as fp:
        data = fp.read()
    out = []
    pos = 0
    while pos < len(data):
        m = ENTRY.match(data, pos)
        if not m:
            raise RecordError("malformed at entry %d (byte %d)"
                              % (len(out) + 1, pos))
        end = m.end() + int(m.group(2))
        if end >= len(data) or data[end:end + 1] != b"\n":
            raise RecordError("malformed at entry %d (byte %d)"
                              % (len(out) + 1, pos))
        out.append((m.group(1).decode(), data[m.end():end]))
        pos = end + 1
    return out


def sessions(path):
    """-> list of sessions: {"kind", "header": {tag: bytes}, "args",
    "events", "ends"}"""
    out = []
    for n, (tag, payload) in enumerate(read_entries(path), 1):
        if tag == "session":
            if payload not in (b"new", b"restore"):
                raise RecordError("malformed at entry %d" % n)
            out.append({"kind": payload.decode(), "header": {}, "args": [],
                        "events": 0, "ends": 0})
        elif not out:
            raise RecordError("entry %d comes before any session" % n)
        elif tag in HEADER_TAGS:
            s = out[-1]
            if s["events"] or (tag != "arg" and tag in s["header"]):
                raise RecordError("malformed at entry %d" % n)
            if tag == "arg":
                s["args"].append(payload)
            else:
                s["header"][tag] = payload
        elif tag in EVENT_TAGS:
            out[-1]["events"] += 1
            out[-1]["ends"] += (tag == "end")
        else:
            raise RecordError("unknown entry %r at entry %d" % (tag, n))
    for i, s in enumerate(out, 1):
        missing = [t for t in NEEDED if t not in s["header"]]
        if missing:
            raise RecordError("session %d's header lacks %s"
                              % (i, ", ".join(missing)))
        try:
            rows, cols = (int(x) for x in s["header"]["term"].split())
        except ValueError:
            raise RecordError("session %d's terminal size is malformed" % i)
        s["term"] = (rows, cols)
        s["seed"] = s["header"]["seed"].decode("latin-1")
    return out


def canonical_seed(text):
    """seed text as the game keeps it: spaces and tabs around it dropped,
    runs of them inside made one space, and a number's leading zeros
    dropped"""
    text = re.sub("[ \t]+", " ", text).strip(" ")
    if text.isascii() and text.isdigit():
        text = text.lstrip("0") or "0"
    return text


def sysconf_setting(line):
    """-> (name as written, in capitals, value) of a sysconf line, read as
    the game reads it (spaces and tabs collapsed, split at the first '=' or
    ':', with spaces before that and one after it skipped), or None"""
    text = b" ".join(line.replace(b"\t", b" ").split())
    m = re.match(rb"([^=:]*)[=:] ?(.*)", text)
    return (m.group(1).rstrip().upper(), m.group(2)) if m else None


def is_setting(setting, name, shortest):
    """whether a sysconf line's setting is name (or an abbreviation of it
    the game takes)"""
    return (setting is not None and len(setting[0]) >= shortest
            and name.encode().startswith(setting[0]))


def sysconf_seed(path):
    """the SEED set in a sysconf file, or None"""
    seed = None
    with open(path, "rb") as fp:
        for line in fp:
            setting = sysconf_setting(line)
            if setting and is_setting(setting, "SEED", 4):
                seed = setting[1].decode("latin-1")
    return seed


def write_sysconf(src, dst, seed):
    """the replay's sysconf: src without the settings the replay mustn't
    act on, and with SEED if it's given"""
    with open(src, "rb") as fp:
        lines = [l for l in fp.read().splitlines(True)
                 if not any(is_setting(sysconf_setting(l), n, k)
                            for n, k in DROPPED_SYSCONF)]
    if lines and not lines[-1].endswith(b"\n"):
        lines[-1] += b"\n"
    if seed is not None:
        lines.append(b"SEED=" + seed.encode("latin-1") + b"\n")
    with open(dst, "wb") as fp:
        fp.writelines(lines)
    os.chmod(dst, 0o600)


def copy_playground(src, dst):
    """the installed playground's program and data files; what a game
    writes starts empty"""
    for f in os.listdir(src):
        p = os.path.join(src, f)
        if (os.path.isfile(p) and not f[0].isdigit() and "lock" not in f
                and f not in VAR_FILES and f != "sysconf"):
            shutil.copy2(p, dst)
    for f in VAR_FILES:
        open(os.path.join(dst, f), "wb").close()
    os.makedirs(os.path.join(dst, "save"), exist_ok=True)


def run_session(record, n, sess, pg, work, result, timeout):
    """replay session n; -> None if the game ran to its end, or why not"""
    home = os.path.join(work, "home%d" % n)
    os.makedirs(home, exist_ok=True)
    rc = sess["header"].get("rcfile")
    if rc is not None:
        with open(os.path.join(home, ".nethackrc"), "wb") as fp:
            fp.write(rc)
    env = {b"HOME": os.fsencode(home), b"TERM": b"xterm",
           b"PATH": os.environb.get(b"PATH", b"/usr/bin:/bin"),
           b"NETHACKDIR": os.fsencode(pg),
           b"NH_REPLAY": os.fsencode(os.path.abspath(record)),
           b"NH_REPLAY_SESSION": str(n).encode(),
           b"NH_REPLAY_RESULT": os.fsencode(result)}
    if "envopts" in sess["header"]:
        env[b"NETHACKOPTIONS"] = sess["header"]["envopts"]
    for v in (b"ASAN_OPTIONS", b"UBSAN_OPTIONS"):  # (a build with sanitizers)
        if v in os.environb:
            env[v] = os.environb[v]
    rows, cols = sess["term"]
    pid, fd = pty.fork()
    if pid == 0:
        try:
            fcntl.ioctl(0, termios.TIOCSWINSZ,
                        struct.pack("HHHH", rows, cols, 0, 0))
            os.chdir(pg)
            # (the game takes its arguments from the record)
            os.execve("./nethack", ["./nethack"], env)
        finally:
            os._exit(127)
    last = time.time()
    status = None
    while status is None:
        r, _, _ = select.select([fd], [], [], 1.0)
        if r:
            try:
                data = os.read(fd, 65536)
            except OSError:
                data = b""
            if data:
                last = time.time()
        wpid, wstatus = os.waitpid(pid, os.WNOHANG)
        if wpid:
            status = wstatus
        elif time.time() - last > timeout:
            os.kill(pid, signal.SIGKILL)
            os.waitpid(pid, 0)
            os.close(fd)
            return "the replay showed nothing for %d seconds" % timeout
    os.close(fd)
    return None


def main():
    ap = argparse.ArgumentParser(description=(__doc__ or "").split("\n")[0])
    ap.add_argument("record")
    ap.add_argument("playground")
    seedarg = ap.add_mutually_exclusive_group()
    seedarg.add_argument("--seed", help="the race's seed (for a record of"
                         " a game with a hidden seed)")
    seedarg.add_argument("--sysconf", help="a sysconf file to use instead"
                         " of the installed one (and to take SEED from)")
    ap.add_argument("--timeout", type=int, default=600,
                    help="give up on a session that shows nothing for this"
                    " many seconds (default 600)")
    ap.add_argument("--keep", action="store_true",
                    help="keep the replay's playground")
    args = ap.parse_args()
    playground = os.path.abspath(args.playground)
    try:
        ss = sessions(args.record)
    except (OSError, RecordError) as e:
        print("can't replay %s: %s" % (args.record, e))
        return 2
    if not ss:
        print("no sessions in", args.record)
        return 2
    if ss[0]["kind"] != "new":
        print("the record doesn't start with a new game, so it can't be"
              " replayed (the save it restores isn't in it)")
        return 2

    # the seed: a hidden one comes from the race's sysconf, a player's own
    # comes with the recorded options
    sysconf = args.sysconf or os.path.join(playground, "sysconf")
    recorded = ss[0]["seed"]
    seed = None
    if recorded.startswith("hidden#"):
        seed = args.seed if args.seed is not None else sysconf_seed(sysconf)
        if seed is None:
            print("the record's seed is hidden (%s): give the race's seed"
                  " with --seed, or its sysconf with --sysconf" % recorded)
            return 2
    elif args.seed is not None and canonical_seed(args.seed) != recorded:
        print("--seed %s isn't the record's seed, %s"
              % (args.seed, recorded))
        return 2

    work = tempfile.mkdtemp(prefix="nhreplay-")
    pg = os.path.join(work, "playground")
    os.makedirs(pg)
    copy_playground(playground, pg)
    write_sysconf(sysconf, os.path.join(pg, "sysconf"), seed)
    result = os.path.join(work, "result")
    failed = cut = None
    for n, sess in enumerate(ss, 1):
        seen = 0
        if os.path.exists(result):
            with open(result, "rb") as fp:
                seen = len(fp.readlines())
        why = run_session(args.record, n, sess, pg, work, result,
                          args.timeout)
        lines = []
        if os.path.exists(result):
            with open(result, "rb") as fp:
                lines = [l.decode("latin-1").rstrip("\n")
                         for l in fp.readlines()[seen:]]
        prefix = "session %d: " % n
        outcome = [l[len(prefix):] for l in lines if l.startswith(prefix)]
        label = "session %d (%s)" % (n, sess["kind"])
        if why:
            failed = n
            print("%s: failed: %s" % (label, why))
        elif not outcome:
            failed = n
            print("%s: failed: the replay stopped without an outcome" % label)
        elif outcome[-1].startswith("failed: "):
            failed = n
            print("%s: %s" % (label, outcome[-1]))
            if ("record's seed is" in outcome[-1]
                    or "the seed differs" in outcome[-1]):
                print("  (the replay's seed doesn't match the record's; check"
                      " --seed or --sysconf)")
        elif outcome[-1].startswith("cut off: "):
            cut = cut or n
            print("%s: %s" % (label, outcome[-1]))
        else:
            print("%s: %s" % (label, outcome[-1]))
            if not sess["ends"]:
                failed = n
                print("%s: failed: checked, but the record has no end for"
                      " it" % label)
        if failed:
            break
    if failed and failed < len(ss):
        later = list(range(failed + 1, len(ss) + 1))
        print("%s %s not replayed, so unchecked: %s on from the save that"
              " session %d's replay made, which already differs"
              % ("session" if len(later) == 1 else "sessions",
                 ", ".join(str(i) for i in later),
                 "it goes" if len(later) == 1 else "they go", failed))
    if failed:
        print("NOT verified")
    elif cut:
        print("not fully verified: session %d ended without a save or the"
              " end of the game, so it could only be replayed, not checked"
              % cut)
    else:
        print("verified")
    if args.keep:
        print("replay playground kept in", pg)
    else:
        shutil.rmtree(work, ignore_errors=True)
    return 0 if not failed and not cut else 1


if __name__ == "__main__":
    sys.exit(main())
