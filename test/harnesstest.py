#!/usr/bin/env python3
"""Check that the test harnesses reject missing coverage and handle endings."""
import contextlib
import io
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import replaytest
import seedfuzz


def record_entry(path, tag, payload):
    with open(path, "ab") as record:
        record.write(tag.encode() + b" %d:" % len(payload) + payload + b"\n")


class Fingerprints(unittest.TestCase):
    def fixture(self, root, zero=False, missing=False):
        path = Path(root) / "fuzzer.txt"
        value = "0" if zero else "1"
        parts = " ".join(p + "=" + value * (16 if p == "layout" else 8)
                         for p in seedfuzz.PARTS)
        lines = []
        for n, name in enumerate(sorted(seedfuzz.PASSES)):
            lines += ["P %d %s" % (n, name),
                      "L %d 1 1 draws=0 layout=%s terrain=11111111"
                      % (n, value * 16)]
            if not missing:
                lines.append("F " + parts)
        path.write_text("\n".join(lines) + "\nE\n")
        return path

    def test_complete_fingerprints_are_compared(self):
        with tempfile.TemporaryDirectory() as root:
            path = self.fixture(root)
            self.assertEqual(seedfuzz.check_seed(path), [])
            data = path.read_text().replace("monsters=11111111",
                                            "monsters=deadbeef", 1)
            path.write_text(data)
            self.assertTrue(seedfuzz.check_seed(path))

    def test_missing_and_zero_fingerprints_fail(self):
        with tempfile.TemporaryDirectory() as root:
            for settings in ({"missing": True}, {"zero": True}):
                with self.subTest(**settings):
                    path = self.fixture(root, **settings)
                    problems = seedfuzz.check_seed(path)
                    self.assertTrue(problems)
                    self.assertTrue(all("fingerprint" in p for p in problems))

    def test_zero_component_is_not_the_missing_fingerprint_sentinel(self):
        with tempfile.TemporaryDirectory() as root:
            path = self.fixture(root)
            path.write_text(path.read_text().replace("traps=11111111",
                                                     "traps=00000000"))
            self.assertEqual(seedfuzz.check_seed(path), [])


class ReplayLifecycle(unittest.TestCase):
    def test_quit_interrupt_does_not_prove_a_declined_interrupt(self):
        entries = [("session", b"new"), ("k", b"32"), ("intr", b""),
                   ("end", b"done fixture")]
        problems = replaytest.lifecycle_problems(entries, 1, True)
        self.assertTrue(any("declined interrupt" in p for p in problems))
        self.assertEqual(replaytest.lifecycle_problems(
            entries, 1, True, declined_interrupts=1), [])

    def test_death_is_visible_before_and_after_disclosure_keys(self):
        with tempfile.TemporaryDirectory() as root:
            path = Path(root) / "record"
            record_entry(path, "session", b"new")
            self.assertFalse(replaytest.recorded_done(path))
            record_entry(path, "end", b"done fixture")
            self.assertTrue(replaytest.recorded_done(path))
            record_entry(path, "k", b"27")
            self.assertTrue(replaytest.recorded_done(path))
            record_entry(path, "session", b"new")
            self.assertFalse(replaytest.recorded_done(path))

    def test_pending_quit_prompt_is_drained_before_more_policy_keys(self):
        g = replaytest.Game.__new__(replaytest.Game)
        g.alive, g.tail = True, ""
        sent = []
        g.drain = lambda: setattr(g, "tail", "Really quit?")
        def send(key):
            sent.append(key)
            g.tail = ""
        g.send = send
        g.keep_playing("normal")
        self.assertEqual(sent, ["n"])

    def test_death_confirmation_cancels_a_partially_typed_answer(self):
        g = replaytest.Game.__new__(replaytest.Game)
        g.alive, g.tail = True, "Die? [yes|n] (n) y"
        sent = []
        g.drain = lambda: None
        def send(key):
            sent.append(key)
            g.tail = ""
        g.send = send
        g.keep_playing("wizard")
        self.assertEqual(sent, ["\033\033"])

    def test_finish_answers_disclosure_already_waiting_for_input(self):
        g = replaytest.Game.__new__(replaytest.Game)
        g.alive, g.pid = True, 0
        g.tail = "Do you want your possessions identified? [ynq] (y)"
        sent = []
        g.drain = lambda seconds: None
        def send(key):
            sent.append(key)
            g.alive = False
        g.send = send
        with patch.object(replaytest.os, "kill"), \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertTrue(g.finish("", secs=0.01))
        self.assertEqual(sent, ["\033"])

    def run_normal_policy(self, ending, sessions=2):
        """Model a game staying alive at disclosure after recording done."""
        games = []
        class Game:
            def __init__(self, pg, home, name, seed, mode, record, login):
                games.append(self)
                self.record, self.alive = record, True
                record_entry(record, "session", b"new")

            def drain(self, *args):
                pass

            def keep_playing(self, mode):
                pass

            def send(self, key):
                record_entry(self.record, "k", b"46")
                if ending == "last-key":
                    record_entry(self.record, "end", b"done fixture")

            def interrupt(self):
                record_entry(self.record, "end", b"done fixture")
                return False

            def finish(self, command):
                if not replaytest.recorded_done(self.record):
                    how = b"save" if command == "S" else b"done"
                    record_entry(self.record, "end", how + b" fixture")
                record_entry(self.record, "k", b"27")
                self.alive = False
                return True

        out = io.StringIO()
        argv = ["replaytest.py", "unused", "--mode", "normal", "-k", "1",
                "-s", str(sessions)]
        if ending == "interrupt":
            argv.append("--signals")
        with patch("sys.argv", argv), patch.object(replaytest, "Game", Game), \
                patch.object(replaytest, "KEYS", ["."]), \
                patch.object(replaytest, "copy_playground"), \
                patch.object(replaytest, "replay_record",
                             return_value=(0, [])), \
                contextlib.redirect_stdout(out):
            result = replaytest.main()
        self.assertEqual(result, 0, out.getvalue())
        self.assertEqual(len(games), 1, "started another game after death")
        return out.getvalue()

    def test_normal_death_on_last_policy_key(self):
        output = self.run_normal_policy("last-key")
        self.assertIn("coverage was not reached", output)

    def test_normal_death_while_interrupt_waits(self):
        output = self.run_normal_policy("interrupt")
        self.assertIn("coverage was not reached", output)

    def test_intended_final_quit_is_full_coverage(self):
        output = self.run_normal_policy("quit", 1)
        self.assertNotIn("coverage was not reached", output)


if __name__ == "__main__":
    unittest.main()
