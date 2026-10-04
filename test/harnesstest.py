#!/usr/bin/env python3
"""Check test harnesses' coverage, command waits and ending behavior."""
import contextlib
import io
from pathlib import Path
import signal
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import feedgame
import layouttest
import launchtest
import panictest
import recordfail
import replaytest
import savecheck
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


class SaveStartup(unittest.TestCase):
    def game(self, chunks):
        g = feedgame.Game.__new__(feedgame.Game)
        g.alive, g.tail, g.feed = True, "", bytearray()
        pending, sent = iter(chunks), []

        def drain(seconds):
            chunk = next(pending, None)
            if chunk is None:
                g.alive = False
            else:
                tty, feed = chunk
                g.tail += tty
                g.feed.extend(feed)
        g.drain, g.send = drain, sent.append
        g.reap = lambda: None
        return g, sent

    def test_restore_status_can_precede_welcome(self):
        g, sent = self.game([
            ("Dlvl:1", b""),
            ("welcome back to NetHack",
             b'{"k":"hdr","restored":1}\n{"k":"hero","a":1}\n')])
        self.assertTrue(savecheck.at_command(g, restored=True))
        self.assertEqual(sent, [])

    def test_more_can_clear_the_only_status_redraw(self):
        g, sent = self.game([
            ("Dlvl:1 welcome back to NetHack --More--", b""),
            ("Something is written here in the dust.",
             b'{"k":"hdr","restored":1}\n{"k":"hero","a":1}\n')])
        self.assertTrue(savecheck.at_command(g, restored=True))
        self.assertEqual(sent, [" "])
        self.assertNotIn("Dlvl:", g.tail)

    def test_startup_requires_the_expected_session_path(self):
        for restored in (False, True):
            for observed in (0, 1):
                with self.subTest(restored=restored, observed=observed):
                    header = b'{"k":"hdr","restored":%d}\n' % observed
                    g, _ = self.game([
                        ("", header + b'{"k":"hero","a":1}\n')])
                    self.assertEqual(savecheck.at_command(g, restored),
                                     restored == bool(observed))

    def test_header_without_a_command_is_not_ready(self):
        g, _ = self.game([
            ("Dlvl:1 welcome back to NetHack",
             b'{"k":"hdr","restored":1}\n{"k":"hero","a":0}\n')])
        self.assertFalse(savecheck.at_command(g, restored=True))

    def test_record_startup_waits_past_status_and_welcome(self):
        g, sent = self.game([
            ("Dlvl:1", b""),
            ("Welcome --More--", b'{"k":"hero","a":0}\n'),
            ("", b'{"k":"hero","a":1}\n')])
        self.assertTrue(recordfail.at_command(g))
        self.assertTrue(feedgame.asked_for_command(g.feed))
        self.assertEqual(sent, [" "])

    def test_record_startup_requires_a_command(self):
        g, sent = self.game([
            ("Dlvl:1", b'{"k":"hero","a":0}\n')])
        self.assertFalse(recordfail.at_command(g))
        self.assertEqual(sent, [])


class GameCleanup(unittest.TestCase):
    def game(self, alive, status):
        g = feedgame.Game.__new__(feedgame.Game)
        g.alive, g.status, g.pid = alive, status, 123
        g.close = lambda: None
        g.feed_thread = SimpleNamespace(join=lambda seconds: None,
                                       is_alive=lambda: False)
        return g

    def test_unreaped_child_is_killed_even_after_terminal_closes(self):
        for alive in (False, True):
            with self.subTest(alive=alive):
                g = self.game(alive, None)
                with patch.object(feedgame.os, "kill") as kill:
                    def waitpid(pid, options):
                        kill.assert_called_once_with(g.pid, signal.SIGKILL)
                        self.assertEqual((pid, options), (g.pid, 0))
                        return pid, signal.SIGKILL
                    with patch.object(feedgame.os, "waitpid", waitpid):
                        feedgame.close_game(g)
                self.assertFalse(g.alive)
                self.assertEqual(g.status, signal.SIGKILL)

    def test_reaped_child_is_not_signalled_or_waited_for(self):
        g = self.game(False, 0)
        with patch.object(feedgame.os, "kill") as kill, \
                patch.object(feedgame.os, "waitpid") as waitpid:
            feedgame.close_game(g)
        kill.assert_not_called()
        waitpid.assert_not_called()


class PanicStartup(unittest.TestCase):
    def check(self, chunks, ready=True):
        g = feedgame.Game.__new__(feedgame.Game)
        g.alive, g.status, g.pid, g.tail = True, None, 0, ""
        pending, sent = iter(chunks), []

        def read_feed():
            g.feed = bytearray()
            g.feed_thread = SimpleNamespace(join=lambda seconds: None,
                                           is_alive=lambda: False)

        def drain(seconds):
            if "#panic\r" in sent:
                return
            chunk = next(pending, None)
            if chunk is None:
                g.alive = False
                g.status = 0
            else:
                tty, feed = chunk
                g.tail += tty
                if hasattr(g, "feed"):
                    g.feed.extend(feed)

        g.read_feed, g.drain = read_feed, drain
        g.reap = g.close = lambda: None
        with tempfile.TemporaryDirectory() as root:
            save = Path(root, "pg", "save")
            save.mkdir(parents=True)

            def send(key):
                sent.append(key)
                if key == "#panic\r":
                    self.assertTrue(feedgame.asked_for_command(
                        getattr(g, "feed", b"")),
                        "#panic was sent before command readiness")
                    g.tail = "Do you want to call panic()"
                elif key == "yes\r":
                    (save / "wizard.e").write_bytes(b"error-save fixture")
                    g.alive, g.status = False, 0

            g.send = send
            with patch.object(feedgame, "Game", return_value=g), \
                    patch.object(feedgame, "copy_playground"), \
                    patch.object(feedgame, "scratch", return_value=
                                 contextlib.nullcontext(root)), \
                    patch.object(feedgame.os, "kill"), \
                    patch.object(feedgame.os, "waitpid",
                                 return_value=(0, 0)), \
                    contextlib.redirect_stdout(io.StringIO()):
                if ready:
                    panictest.check("unused", "panic-regression")
                else:
                    with self.assertRaisesRegex(AssertionError,
                                                "first command"):
                        panictest.check("unused", "panic-regression")
        return sent

    def test_status_before_welcome_does_not_start_panic(self):
        sent = self.check([
            ("Dlvl:1", b""),
            ("Welcome --More--", b'{"k":"hero","a":0}\n'),
            ("", b'{"k":"hero","a":1}\n')])
        self.assertEqual(sent, [" ", "#panic\r", "yes\r"])

    def test_startup_hero_without_a_command_does_not_start_panic(self):
        sent = self.check([
            ("Dlvl:1", b'{"k":"hero","a":0}\n')], ready=False)
        self.assertEqual(sent, [])


class LayoutCommands(unittest.TestCase):
    def game(self, lines, pending=()):
        feed = SimpleNamespace(lines=list(lines))
        g = SimpleNamespace(alive=True, now=0.0, sent=[],
                            pending=list(pending))

        def drain(seconds):
            g.now += seconds
            if g.pending:
                feed.lines.extend(g.pending.pop(0))

        def send(key, settle=0.0):
            g.sent.append(key)

        g.drain, g.send = drain, send
        return g, feed

    def settle(self, g, feed, keys, secs=0.1):
        with patch.object(layouttest.time, "time", lambda: g.now):
            return layouttest.settle(g, feed, 0, keys, secs=secs)

    def test_in_turn_hero_updates_are_not_command_boundaries(self):
        g, feed = self.game([
            {"k": "key", "a": 7},
            {"k": "hero", "a": 7, "t": 20}], [
            [{"k": "hero", "a": 7, "t": 21}],
            [{"k": "hero", "a": 7, "t": 22}]])
        self.assertFalse(self.settle(g, feed, 1))
        self.assertEqual(g.sent, [])

    def test_every_submitted_key_must_be_consumed(self):
        g, feed = self.game([
            {"k": "key", "a": 7}, {"k": "hero", "a": 8}])
        self.assertFalse(self.settle(g, feed, 2))

    def test_boundary_must_follow_the_last_key_in_a_batch(self):
        # Escape ends one command; the move waits at an in-turn prompt.
        g, feed = self.game([
            {"k": "key", "a": 7}, {"k": "hero", "a": 8},
            {"k": "key", "a": 8}, {"k": "hero", "a": 8}], [
            [{"k": "hero", "a": 9}]])
        self.assertTrue(self.settle(g, feed, 2))
        self.assertEqual(feed.lines[-1]["a"], 9)
        self.assertEqual(g.sent, [])

    def test_escape_recovery_needs_its_own_key_and_boundary(self):
        g, feed = self.game([
            {"k": "key", "a": 7}, {"k": "hero", "a": 7}])

        def send(key, settle=0.0):
            g.sent.append(key)
            # The old command finishes before the queued Escape is read.
            feed.lines.append({"k": "hero", "a": 8})
            g.pending = [
                [{"k": "key", "a": 8}, {"k": "hero", "a": 8}],
                [{"k": "hero", "a": 9}]]

        g.send = send
        self.assertTrue(self.settle(g, feed, 1, secs=2))
        self.assertEqual(g.sent, ["\033"])
        self.assertEqual(feed.lines[-1]["a"], 9)

    def test_stair_walking_stops_when_settling_fails(self):
        g = SimpleNamespace(alive=True, tail="")
        feed = SimpleNamespace(
            lines=[], hero=lambda: (2, 2, 0, 1),
            stairs=lambda: [(3, 2, 0, 0, 0, 2)], terrain=lambda levels: None)
        with patch.object(layouttest, "settle", return_value=False), \
                patch.object(feedgame, "send_keys") as send:
            self.assertFalse(layouttest.take_stairs(
                g, feed, {}, False, tries=3))
        self.assertEqual(send.call_count, 1)


class LauncherPrompt(unittest.TestCase):
    def game(self, outcomes):
        def frame(action, pet):
            return {"a": action, "hero": {"x": 2, "y": 2},
                    "level": {"monsters": [
                        {"id": 1, "tame": True, "x": pet[0], "y": pet[1]}]}}

        initial = frame(1, (3, 2))
        g = SimpleNamespace(alive=True, tail="", now=0.0, sent=[], rows=[],
                            pending=[], frame=initial)
        choices = iter(outcomes)
        g.events = lambda: g.rows

        def send(key, settle=0):
            g.sent.append(key)
            action = g.frame["a"]
            g.rows.append({"k": "key", "a": action, "key": ord(key)})
            if key == " ":
                g.rows.append({"k": "hero", "a": action + 1})
                return
            outcome = next(choices)
            if outcome in ("swap", "missing-prompt"):
                g.rows.append({"k": "msg", "text": "You swap places with"
                               " your pony."})
                if outcome == "missing-prompt":
                    g.rows.append({"k": "hero", "a": action + 1})
                else:
                    g.tail = "--More--"
                    pet = g.frame["level"]["monsters"][0]
                    g.frame = frame(action, (2, 2))
                    g.frame["hero"] = {"x": pet["x"], "y": pet["y"]}
                return
            if outcome != "wait":
                g.rows.append({"k": "msg", "text": "You stop.  Your pony"
                               " is in the way!"})
            # A same-action hero update cannot finish the attempt.
            g.rows.append({"k": "hero", "a": action})
            if outcome != "in-turn-only":
                position = ((4, 2) if outcome == "flee" else
                            (3, 3) if outcome == "diagonal" else (2, 3))
                g.pending.append(frame(action + 1, position))

        def drain(seconds):
            g.now += seconds
            if g.pending:
                g.frame = g.pending.pop(0)
                g.rows.append({"k": "hero", "a": g.frame["a"]})

        def snapshot(game):
            self.assertIs(game, g)
            self.assertFalse(g.pending, "snapshot before command boundary")
            return g.frame

        g.send, g.drain, g.snapshot = send, drain, snapshot
        return g, initial

    def run_prompt(self, g, initial, problems=()):
        with patch.object(launchtest.time, "monotonic", lambda: g.now), \
                patch.object(launchtest, "snapshot", side_effect=g.snapshot), \
                patch.object(launchtest, "quiet"), \
                patch.object(launchtest, "fold", side_effect=[
                    ([], 0, 0, 0, []), (problems, 0, 1, 0, [])]):
            launchtest.swap_prompt(g, initial, {})

    def test_refusal_waits_for_pet_and_uses_its_new_position(self):
        g, initial = self.game(["flee", "wait", "swap"])
        self.run_prompt(g, initial)
        self.assertEqual(g.sent, ["l", ".", "j", " "])

    def test_refusal_requires_a_new_command_boundary(self):
        g, initial = self.game(["in-turn-only"])
        with self.assertRaisesRegex(AssertionError, "did not reach"):
            self.run_prompt(g, initial)
        self.assertEqual(g.sent, ["l"])

    def test_diagonal_pet_waits_for_an_orthogonal_swap(self):
        g, initial = self.game(["diagonal", "wait", "swap"])
        self.run_prompt(g, initial)
        self.assertEqual(g.sent, ["l", ".", "j", " "])

    def test_missing_swap_prompt_is_not_retried(self):
        g, initial = self.game(["missing-prompt"])
        with self.assertRaisesRegex(AssertionError, "without its prompt"):
            self.run_prompt(g, initial)
        self.assertEqual(g.sent, ["l"])

    def test_refusals_cannot_replace_a_successful_swap(self):
        g, initial = self.game(["refuse"] * 40)
        with self.assertRaisesRegex(AssertionError, "no successful pet swap"):
            self.run_prompt(g, initial)
        self.assertEqual(len(g.sent), 40)

    def test_snapshot_mismatch_still_fails(self):
        g, initial = self.game(["refuse", "swap"])
        with self.assertRaisesRegex(AssertionError, "differs from the feed"):
            self.run_prompt(g, initial, ["stale pet position"])
        self.assertEqual(g.sent, ["l", "j"])


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
            self.assertTrue(g.finish("", secs=1))
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
