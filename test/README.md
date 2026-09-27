### how to use

 * compile NetHack without DLB
 * install
 * copy the test lua files into the nethack playground dir
 * start nethack in wizmode
 * use wizloadlua extended command to load and run one of the test files.

### feedtest.py: live feed

`python3 test/feedtest.py -k 600 --seeds 3 playground` compares per-key
state and RNG logs with the feed enabled and disabled, checks record/replay
feed determinism and SIGUSR1 snapshots, and checks save/restore replay
sessions, dwarf `showrace` glyph metadata, UTF-8 names, and automatically
assigned menu shortcuts, menu events before input and pending menus in
keyframes. It includes its terminal driver (`feedgame.py`) and needs no
separate checkout. Linux CI runs a
shorter wizard-mode pass, exercising its level-change and naming macros.
Wizard mode needs `WIZARDS=*`; the UTF-8 fixture and `--mode explore`
need `EXPLORERS=*`. Use an unprivileged tty build and a
test playground with no server `SEED` or `RECORDFILE` configured, as with
the replay tests. `--keep` retains scratch files; failures retain them too.
The tests never print a recording or feed contents.

`python3 test/feedfaults.py playground` adds Linux/gdb fixtures for nested
JSON lines, a naming error which prompts for input, a pending signal during
an unfinished line, accessibility overrides, and a signal during level
arrival. It also checks SIGINT during a partially written keyframe and
that the feed does not generate an extra dumplog or draw RNG at game end,
and that naming unpaid items leaves shop bills and surcharge flags alone.
It checks remembered price quotes in the feed's object names directly,
including that unpaid items don't gain a remembered quote instead.
It needs a debug build, gdb, and `WIZARDS=*`. It modifies only
disposable games and checks that their feed remains valid JSON.

The feed is tty-only. Its glyph metadata uses default characters and
colors, whatever the player's `color` option, with `hero.screen` giving the
hero cell's positional styling. Kill events report movement `phase`, not
the killer.

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

### Replaying recorded games

With `RECORDFILE` set in sysconf (in a build with `DUMPLOG`), or
`NH_RECORD=path` in the environment of a game running with your own
permissions, a seeded game played with the tty interface is recorded: its
command-line arguments, login name and options, every random seed it
draws, every key it reads, where it acted on a hangup or an interrupt, and
a digest of its state every 100 turns, on arriving on a level and whenever
a session ends (see `Seeding` in the top directory).

A record is played again with "nethack --replay FILE": the game runs it
in a scratch copy of the playground, shows it on the terminal at a
watchable pace (space pauses, "." steps one key, "+" and "-" change the
speed, ">" runs flat out, "q" stops) and checks it as it goes; "--verify"
runs flat out and only reports, "--seed SEED" gives a server's hidden
seed.

 * `nethack --replay RECORD` to watch it, `nethack --replay RECORD --verify`
   to check it; each session's outcome is printed at the end, and the exit
   status is 0 only if the whole record checked out (1: a session failed,
   2: one was cut off or you stopped it)
 * for a game with the server's hidden seed, the replay needs that seed:
   the installed sysconf's `SEED`, or `--seed SEED` (for example after the
   next race has changed it); the game checks it against the record's
   digest before replaying anything

`replaytest.py` checks recording and replay over a long game: in a copy of
the playground it plays a recorded seeded game with thousands of random
ordinary keys (by default in wizard mode, also teleporting between
levels), saving and restoring it several times, then replays the record
with `--verify`; `--signals` also interrupts the game (^C) now and then, hangs
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
