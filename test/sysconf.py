#!/usr/bin/env python3
"""Temporary test sysconf changes, with atomic restoration and recovery.

Use --restore PATH only after an interrupted test has stopped.  A live
test holds a lock on its backup, and recovery refuses to interfere with it.
SIGKILL cannot run cleanup: its backup remains and the next test refuses
to overwrite it.  No configuration contents are printed.
"""
import argparse
from contextlib import contextmanager
import fcntl
import os
from pathlib import Path
import re
import shutil
import signal
import tempfile


def assert_test_config(config, path=None):
    """Match the game's case-insensitive keys and either separator."""
    if path is not None:
        assert_no_backup(Path(path))
    if isinstance(config, bytes):
        config = config.decode()
    if re.search(r"^[ \t]*(SEED|RECORDFILE)[ \t]*[:=]", config, re.I | re.M):
        raise ValueError("use a test playground without SEED/RECORDFILE")


def backup_path(path):
    path = Path(path)
    return path.with_name(path.name + ".test-backup")


def assert_no_backup(path):
    backup = backup_path(path)
    if os.path.lexists(backup):
        raise RuntimeError(
            "sysconf backup already exists: %s; stop any running test, then"
            " recover with: python3 test/sysconf.py --restore %s"
            % (backup, path))


@contextmanager
def blocked_signals():
    previous = signal.pthread_sigmask(signal.SIG_BLOCK,
                                      (signal.SIGTERM, signal.SIGHUP))
    try:
        yield
    finally:
        signal.pthread_sigmask(signal.SIG_SETMASK, previous)


def staged(path, data):
    """A complete file beside path, with the original mode and metadata."""
    fd, name = tempfile.mkstemp(prefix=path.name + ".test-", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
        shutil.copystat(path, name)
        return Path(name)
    except BaseException:
        os.unlink(name)
        raise


@contextmanager
def TemporarySysconf(paths, content):
    """Replace one or more config files, retaining each original on disk."""
    if isinstance(paths, (str, os.PathLike)):
        paths = (paths,)
    paths = list(dict.fromkeys(Path(p).absolute() for p in paths))
    if isinstance(content, str):
        content = content.encode()
    saved = []
    handlers = {}

    def terminate(signum, frame):
        # SystemExit unwinds nested game contexts, closing their PTYs too.
        raise SystemExit(128 + signum)

    try:
        with blocked_signals():
            for sig in (signal.SIGTERM, signal.SIGHUP):
                handlers[sig] = signal.signal(sig, terminate)
            for path in paths:
                backup = backup_path(path)
                assert_no_backup(path)
                if path.is_symlink():
                    raise ValueError("use a regular sysconf file: %s" % path)
                tmp = staged(path, path.read_bytes())
                lock = open(tmp, "rb")
                try:
                    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    # Unlike replace(), link() never overwrites another
                    # test's backup.  The published backup is already whole.
                    os.link(tmp, backup)
                    saved.append((path, backup, lock))
                except BaseException:
                    lock.close()
                    raise
                finally:
                    tmp.unlink()
            for path, _, _ in saved:
                tmp = staged(path, content)
                try:
                    os.replace(tmp, path)
                finally:
                    tmp.unlink(missing_ok=True)
        yield
    finally:
        with blocked_signals():
            try:
                for path, backup, lock in reversed(saved):
                    try:
                        os.replace(backup, path)
                    finally:
                        lock.close()
            finally:
                for sig, handler in handlers.items():
                    signal.signal(sig, handler)


def SysconfLine(pg, line):
    """The playground's sysconf with another line, for a moment."""
    path = Path(pg) / "sysconf"
    return TemporarySysconf(path, path.read_bytes() + b"\n"
                           + line.encode() + b"\n")


def restore_backup(path):
    path = Path(path)
    backup = backup_path(path)
    with open(backup, "rb") as f:
        try:
            fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise RuntimeError("a running test owns the sysconf backup")
        os.replace(backup, path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--restore", metavar="PATH", required=True)
    args = parser.parse_args()
    restore_backup(args.restore)
    print("restored sysconf from its test backup")


if __name__ == "__main__":
    main()
