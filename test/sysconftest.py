#!/usr/bin/env python3
"""Check test configuration guards, signal cleanup and crash recovery."""
import os
from pathlib import Path
import select
import signal
import subprocess
import sys
import tempfile
import unittest

from sysconf import (TemporarySysconf, assert_test_config, backup_path,
                     restore_backup)
import feedgame


CHILD = """
import signal, sys
from sysconf import TemporarySysconf
with TemporarySysconf(sys.argv[1:], b'WIZARDS=\\nSEED=test fixture\\n'):
    print('ready', flush=True)
    signal.pause()
"""


class SysconfTests(unittest.TestCase):
    def setUp(self):
        self.work = tempfile.TemporaryDirectory(prefix="nhsysconf-")
        self.addCleanup(self.work.cleanup)
        self.paths = [Path(self.work.name) / name for name in ("one", "two")]
        self.original = [b"WIZARDS=*\n", b"EXPLORERS=*\n# original\n"]
        self.modes = [0o600, 0o640]
        for path, data, mode in zip(self.paths, self.original, self.modes):
            path.write_bytes(data)
            path.chmod(mode)

    def assert_restored(self):
        for path, data, mode in zip(self.paths, self.original, self.modes):
            self.assertEqual(path.read_bytes(), data)
            self.assertEqual(path.stat().st_mode & 0o777, mode)
            self.assertFalse(backup_path(path).exists())

    def child(self):
        env = dict(os.environ, PYTHONPATH=str(Path(__file__).resolve().parent))
        p = subprocess.Popen([sys.executable, "-c", CHILD,
                              *map(str, self.paths)], env=env,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)

        def cleanup():
            if p.poll() is None:
                p.kill()
            p.communicate(timeout=5)
        self.addCleanup(cleanup)
        self.assertTrue(select.select([p.stdout], [], [], 5)[0],
                        "child did not enter temporary configuration")
        self.assertEqual(p.stdout.readline(), b"ready\n")
        return p

    def test_parser_guards(self):
        for key in ("SEED", "seed", "SeEd", "RECORDFILE", "recordfile"):
            for separator in ("=", ":"):
                with self.subTest(key=key, separator=separator):
                    with self.assertRaisesRegex(ValueError, "test playground"):
                        assert_test_config("\t%s %s private\n" %
                                           (key, separator))
        assert_test_config("# seed: comment\n# RECORDFILE=x\nWIZARDS=*\n")
        assert_test_config(b"EXPLORERS=*\n")

    def test_exception_restores_all_files(self):
        with self.assertRaisesRegex(ValueError, "fixture exception"):
            with TemporarySysconf(self.paths, "WIZARDS=\n"):
                for path, data, mode in zip(self.paths, self.original,
                                            self.modes):
                    self.assertEqual(backup_path(path).read_bytes(), data)
                    self.assertEqual(path.read_bytes(), b"WIZARDS=\n")
                    self.assertEqual(path.stat().st_mode & 0o777, mode)
                raise ValueError("fixture exception")
        self.assert_restored()

    def test_signals_restore_all_files(self):
        for sig in (signal.SIGTERM, signal.SIGHUP):
            with self.subTest(signal=sig):
                p = self.child()
                p.send_signal(sig)
                p.communicate(timeout=5)
                self.assertEqual(p.returncode, 128 + sig)
                self.assert_restored()

    def test_kill_keeps_recoverable_backup_and_refuses_next_test(self):
        p = self.child()
        with self.assertRaisesRegex(RuntimeError, "running test"):
            restore_backup(self.paths[0])
        p.kill()
        p.communicate(timeout=5)
        for path, data in zip(self.paths, self.original):
            self.assertEqual(backup_path(path).read_bytes(), data)
        with self.assertRaisesRegex(RuntimeError, "backup already exists"):
            with TemporarySysconf(self.paths, "other policy\n"):
                self.fail("stale backup was silently overwritten")
        with self.assertRaisesRegex(RuntimeError, "--restore"):
            assert_test_config(self.paths[0].read_bytes(), self.paths[0])
        for path in self.paths:
            self.assertEqual(path.read_bytes(),
                             b"WIZARDS=\nSEED=test fixture\n")
            restore_backup(path)
        self.assert_restored()

    def test_partial_setup_restores_only_owned_backups(self):
        second = self.paths[1]
        backup_path(second).write_bytes(second.read_bytes())
        second.write_bytes(b"interrupted policy\n")
        with self.assertRaisesRegex(RuntimeError, "backup already exists"):
            with TemporarySysconf(self.paths, "other policy\n"):
                self.fail("partial setup reached the body")
        self.assertEqual(self.paths[0].read_bytes(), self.original[0])
        self.assertFalse(backup_path(self.paths[0]).exists())
        self.assertEqual(second.read_bytes(), b"interrupted policy\n")
        self.assertEqual(backup_path(second).read_bytes(), self.original[1])

    def test_playground_copy_omits_transaction_files(self):
        source = Path(self.work.name) / "source"
        target = Path(self.work.name) / "copy"
        source.mkdir()
        (source / "sysconf").write_bytes(b"WIZARDS=*\n")
        for name in ("sysconf.test-backup", "sysconf.test-staging"):
            (source / name).write_bytes(b"temporary\n")
        feedgame.copy_playground(target, source)
        self.assertEqual((target / "sysconf").read_bytes(), b"WIZARDS=*\n")
        self.assertFalse(list(target.glob("sysconf.test-*")))


if __name__ == "__main__":
    unittest.main()
