### how to use

 * compile NetHack without DLB
 * install
 * copy the test lua files into the nethack playground dir
 * start nethack in wizmode
 * use wizloadlua extended command to load and run one of the test files.

### seedfuzz.py: seeded games

`seedfuzz.py` checks that a seeded game's levels don't depend on the
hero's history (see `Seeding` in the top directory). For each seed it
starts a game in explore mode with `NH_SEEDFUZZ` set; the game
(`seedfuzz_run()` in `src/wizcmds.c`, also available as the debug-mode
command `#wizseedfuzz`) makes every level (apart from the tutorial and the
endgame's placeholder level; the Astral Plane with its player-monsters) as
a baseline, then again under each of these, putting the game's state back
in between:

 * `order-down`, `order-shuffle`: levels made in another order
 * `genocide`: three random species genocided
 * `extinct`: five random species extinct
 * `born`: random birth counts
 * `artifacts`: up to six random artifacts already exist
 * `aggravate`: hero wears a ring of aggravate monster
 * `amulet`: hero carries the Amulet
 * `hero`: hero's experience level, Luck, alignment, position, protection
   from shape changers, demigod and quest status changed
 * `fruit`: a different fruit name
 * `uniques`: several uniques already killed
 * `progress`: the bottom of every dungeon reached, the invocation done
 * `hallu`: hero hallucinating
 * `gear`: hero sees invisible and has converted (these may change
   monsters and what they carry, not the map or other objects)
 * `turn`: every level made on a random later turn
 * `ids`: many monsters and objects made before (the id counter moved on)
 * `name`: a different hero's name

It then compares each pass with the baseline, level by level: terrain, the
number of random draws the layout made, traps, objects, monsters, the
wandering monster timeline (which species turns up on which turn) and the
level's fingerprint (as `#levelhash` and the dumplog show it), with only
the differences each change is allowed to make (see the script's
docstring).

To run it:

 * build and install as usual
 * in playground/sysconf, allow explore mode (`EXPLORERS=*`), set
   `MAXPLAYERS=25` (at least the number of parallel jobs; 25 at most), and
   leave `SEED` unset (the fuzzer needs each game to have its own seed,
   and won't run for a server's hidden one)
 * run it as the playground's owner: the fuzzer only runs with the
   player's own permissions
 * `python3 test/seedfuzz.py -n 300 -j 22 playground`

Each game runs on a pseudo-terminal, with no shell. It takes about two
minutes for 300 seeds with 22 jobs on a 32-core machine. It exits non-zero
if any seed shows a difference and prints where; `--keep` keeps the
per-seed output files for a closer look.

### replay.py: recorded seeded games

With `RECORDFILE` set in sysconf (in a build with `DUMPLOG`), a seeded game
played with the tty interface is recorded: its command-line arguments,
login name and options, every random seed it draws, every key it reads,
where it acted on a hangup or an interrupt, and a digest of its state
every 100 turns, on arriving on a level and whenever a session ends (see
`Seeding` in the top directory). `replay.py` plays each session of such a
record again and the game checks every digest:

 * `python3 test/replay.py RECORD playground`
 * for a game with the server's hidden seed, the replay needs that seed:
   it takes the installed sysconf's `SEED`, or give it with `--seed SEED`
   or `--sysconf FILE` (for example after the next race has changed it);
   the game checks it against the record's digest before replaying
   anything

It runs the game in a copy of the playground, on a pseudo-terminal of the
recorded size, with no shell, with the recorded options and a copy of the
sysconf (without `SEED`, but for a hidden seed, and without
`RECORDFILE`, `DUMPLOGFILE`, `MSGHANDLER` and `CRASHREPORTURL`); the
replay stops at the end of the game, so nothing is written to the real
playground, dumplog directory or record files. It prints each session's
outcome, and exits 0 only if every session was checked and checks out; a
session that ended without a save or the end of the game (for example
because the game was killed) is replayed but can't be checked. When a
session fails, the sessions after it aren't replayed (they go on from the
save that the failed session's replay made), and it says which. A session
that shows nothing for `--timeout` seconds (600 by default) is given up.

`replaytest.py` checks recording and replay over a long game: in a copy of
the playground it plays a recorded seeded game with thousands of random
ordinary keys (by default in wizard mode, also teleporting between
levels), saving and restoring it several times, then runs `replay.py` on
the record; `--signals` also interrupts the game (^C) now and then, hangs
up on it in the middle of a long search instead of saving, and quits with
^C; `--login` starts the game without `-u`, so that it takes the hero's
name from `$USER` and the replay takes it from the record (use it with
explore or normal mode: wizard mode names every hero "wizard"):

 * `python3 test/replaytest.py -k 3000 -s 4 playground`
 * `python3 test/replaytest.py -k 3000 -s 4 --signals playground`
 * `python3 test/replaytest.py -k 1000 -s 3 --mode explore --login playground`

For `replaytest.py`, the playground's sysconf must allow the mode played
(`WIZARDS`, `EXPLORERS`) and have `RECORDFILE` and `SEED` unset: it records
to a file of its own with `NH_RECORD`, which is ignored when `RECORDFILE`
is set, and plays with a seed of its own. Run both scripts as the
playground's owner: `NH_RECORD` and replaying only work with the player's
own permissions.
