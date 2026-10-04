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
keyframes. Its `ending` checks hang up games waiting at a level change's
`--More--`, recorded (and replayed) and not, and save one at its first
command; each must end with an `"end"` keyframe of its final state. Its
games wait for the feed's first command boundary, answering every startup
`--More--`, before sending keys. It
includes its terminal driver (`feedgame.py`) and needs no
separate checkout. The harnesses run each game in a copy of the playground
under the temporary directory, named by `NETHACKDIR`; keep `TMPDIR` short,
since the game ignores a `NETHACKDIR`, `HOME` or `NH_STATELOG` longer than
128 bytes (`feedgame.game_env()` stops with the path instead). Test games
get none of the caller's environment variables that change a game
(`feedgame.GAME_ENV`: `WIZKIT`, `SHOPTYPE`, `NETHACK_FEED_KF_EVERY`...),
and a harness that fails keeps its scratch directory and says where.
Playground copies isolate writable game files, but an installed binary
still reads its compiled-in sysconf first. Run tests which temporarily
edit sysconf (`layouttest.py`, `seedcheck.py`, `launchtest.py`) serially,
with no other games using that binary. Use its original playground, or a
relocated installation whose compiled-in playground no longer exists.
Temporary configuration changes use `sysconf.test-backup` and atomic
replacement. Normal exit, exceptions, SIGTERM and SIGHUP restore the
original bytes and file mode. SIGKILL cannot run cleanup: stop any surviving
test games, then run `python3 test/sysconf.py --restore playground/sysconf`
before testing or playing again. Tests refuse a retained backup and
recovery refuses to interfere with a live test.
Linux CI runs a
shorter wizard-mode pass, exercising its level-change and naming macros.
Wizard mode needs `WIZARDS=*`, and the `ending` checks run only in it;
the UTF-8 fixture and `--mode explore` need `EXPLORERS=*`. Use an
unprivileged tty build and a
test playground with no server `SEED` or `RECORDFILE` configured, as with
the replay tests. `--keep` retains scratch files; failures retain them too.
The tests never print a recording or feed contents.

`python3 test/feedfaults.py playground` adds Linux/gdb fixtures for nested
JSON lines, a naming error which prompts for input, a pending signal during
an unfinished line, accessibility overrides, and a signal during level
arrival. It also checks SIGINT during a partially written keyframe and
that the feed does not generate an extra dumplog or draw RNG at game end,
and that naming unpaid items leaves shop bills and surcharge flags alone.
The SIGINT fixture waits at a command boundary without searching (which
can find a monster and prompt), then waits for the pipe to fill before
interrupting its writer and for the quit prompt before answering it.
It leaves 4096 bytes of pipe capacity for the frame, including on systems
whose larger page size prevents shrinking the pipe that far.
It checks remembered price quotes in the feed's object names directly,
including that unpaid items don't gain a remembered quote instead.
It needs a debug build, gdb, and `WIZARDS=*`. It modifies only
disposable games and checks that their feed remains valid JSON.

The feed is tty-only. Its glyph metadata uses default characters and
colors, whatever the player's `color` option, with `hero.screen` giving the
hero cell's positional styling. Kill events report movement `phase`, not
the killer.

### layouttest.py: layout dumps and keyframes

`python3 test/layouttest.py playground` checks `nethack --layouts` and
`--layout-hashes`: the same bytes on every run, both forms agreeing,
options files, `NETHACKOPTIONS`, `ROGUEOPTS` and a window type (`-w`, or
in sysconf) ignored, sysconf's `SEED`
used over standard input, the refusals (`-D`, invalid or missing seed,
standard output closed by its reader) leaving no output and no scratch
directory, a file it makes and an existing one ending up mode 0600 holding
the dump (a directory refused), a run in a read-only copy of
the playground with a private `TMPDIR` (standard output exactly the dump,
nothing changed, well under 10 s), and a run beside an open game that leaves
the game's files alone. It then plays seeded games started as a race server
starts them (the seed in `NETHACKOPTIONS`), with the feed and
`NH_FEEDCHECK`, walking them down two levels, saving, restoring and back up
one, and folds each feed against the dump: every keyframe names the dump's
layout for its level, a keyframe on the level the game was already on
equals the folded state, and the folded terrain, map, screen and view hash
to every `chk` line. A seeded wizard-mode game, level-teleporting, checks
the same with no layout. It needs `WIZARDS=*` and `EXPLORERS=*`, no `SEED`
or `RECORDFILE`, and the playground's owner; it adds lines to the
playground's sysconf temporarily, using the backup and recovery helper
described above.

### sessiontest.py: managed terminal sessions

`python3 test/sessiontest.py playground` checks `--managed-session` with
unrecorded and recorded games. It covers quit/save aliases, repeated
SIGINT and literal Ctrl-C, unchanged turn and RNG counters, hangup by
closing the PTY master, restore with and without the flag, normal escape
and replay using the recorded policy. It uses disposable playgrounds and
the same tty driver as the feed tests; no debugger is needed. Run with a
Unix tty build with DUMPLOG, as the playground's owner with matching real
and effective IDs, and no server SEED or RECORDFILE configured. Both cases
use normal play; wizard and explore permissions are not needed. Linux
x86-64 CI runs this check.

Startup waits for the feed's first command boundary, after the welcome
prompts and startup RNG draws. State comparisons wait for the matching
Escape in `NH_STATELOG`, so a slow runner cannot supply an old sample.

### launchtest.py: normal seeded games under a trusted launcher

`python3 test/launchtest.py playground` checks normal seeded games with
`-d DIR --managed-session -u PLAYER -@`, a clean environment
with the seed in `NETHACKOPTIONS`, `NH_RECORD`, feed descriptor 3, an 80x24
terminal with ISIG and echo disabled, and empty `SHELLERS`, `WIZARDS` and
`EXPLORERS` in sysconf. It generates both layout forms through standard
input, checks their agreement and output limits, and requests a snapshot
directly from the feed reader when the header arrives. The initial
`arrive` keyframe can satisfy that request; a separate `signal` keyframe
is not guaranteed. Later requests while idle must produce `signal` frames.

Four cases hang up during gameplay, an inventory menu, extended-command
input and actual death disclosure. The gameplay case checks mode and
managed-session refusals; the live-input cases check idle snapshot stability.
They send SIGHUP and close the terminal
while continuing to read the feed. Every final feed must reconstruct
against the layouts and every record must verify.

Use an unprivileged Unix tty build with DUMPLOG and the playground's owner.
The test temporarily replaces the playground's sysconf with a strict
launcher policy and restores it, so run it alone against that build. It
refuses configured SEED or RECORDFILE entries. Failures retain diagnostics.
Linux x86-64 CI runs it. No external service or launcher is required.

### sftagstest.py: save-file converter generation

After `make -C util sfctool`, run `python3 test/sftagstest.py util/sftags`.
It runs the real generator in temporary directories and checks that short
member tags after longer lines still generate pointer serializers. The
fixtures include long extension fields, CRLF, and a missing final newline.
CI runs it after building sfctool on Linux and macOS.

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

It then compares each pass with the baseline, level by level: the level's
layout hash, which must never differ, its terrain, engravings, stairs and
rooms, the number of random draws the layout made, traps, objects,
monsters, the wandering monster timeline (which species turns up on which
turn) and the level's fingerprint (as `#levelhash` and the dumplog show
it), with only the differences each change is allowed to make (see the
script's docstring).
The checker requires all 18 passes, nonempty baseline levels, the same
level set in every pass, and all four fingerprint components. An end
marker alone cannot turn skipped comparisons into success.

To run it:

 * build and install as usual
 * in playground/sysconf, allow explore mode (`EXPLORERS=*`), set
   `MAXPLAYERS=25` (at least the number of parallel jobs; 25 at most), and
   leave `SEED` unset (the fuzzer needs each game to have its own seed,
   and won't run for a server's hidden one)
 * run it as the playground's owner: the fuzzer only runs with the
   player's own permissions
 * `python3 test/seedfuzz.py -n 300 -j 22 playground`

Each game runs on a pseudo-terminal, with no shell, in a copy of the
playground made for the run, so the playground itself is only read and
runs don't share level or lock files. It takes about two minutes for 300
seeds with 22 jobs on a 32-core machine. It exits non-zero if any seed
shows a difference and prints where, keeping the run's directory (the
failing seeds' output and the copy) for a closer look; `--keep` keeps it
whatever the outcome.

### seedcheck.py: startup and the saved seed

`python3 test/seedcheck.py playground` checks public fixture seeds
in normal, explore and wizard modes. Each must preserve its character,
attributes, starting inventory, pet and observed object appearances when
the player's identity options, `-p`, `-r`, `-@` and pet choice change.
Name changes are checked in normal and explore modes; wizard mode fixes
the name to `wizard`. Repeated starts with the same handicap must agree,
but strength, constitution and power can change with the starting kit.
The fixtures include paupers whose missing spells lower starting power.
It also checks numeric/text seed canonicalization. Snapshots wait for the first
command's complete keyframe; gameplay randomness and object IDs are not
compared across independent games.

Normal-mode saves, both seeded and unseeded, must retain their own seed
when the player's seed option changes without a server seed, and separately
when a server seed is configured. A hidden-seed normal game must
match its public-seed reference, stay hidden when sysconf's seed changes,
disappears or becomes invalid, and replay its four saved sessions only
with the correct seed. A hidden explore game saved on level 1 must make a
previously unvisited level from the original seed after restoring with
another configured seed. Invalid server seeds must refuse new games
instead of falling back to the player's option.
Expected replay refusals check the reason, and their scratch files stay
inside the suite's temporary directory. Normal-mode saves here occur at
the first command; random gameplay and hangup saves are covered elsewhere.

Use the playground's owner, `WIZARDS=*`, `EXPLORERS=*`, and no server
`SEED` or `RECORDFILE`. This suite temporarily edits sysconf; the isolation
and serial execution requirements above apply. Linux x86_64 CI runs it.

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

Wizard/explore runs must reach every requested save/restore session;
`--signals` also requires the intervening hangups and an interrupt that
the driver actually declined during play, separately from the final quit.
Random play declines escape from the dungeon and waits through `--More--`
for an interrupt's quit prompt before declining it. Wizard/explore tests
require full confirmation for death, so a queued movement key cannot
accept it before the driver has read the prompt. A verified shorter
record or a failure to stop a session fails the test. A normal-mode death
may end random play early; the report states its reduced coverage, while
`seedcheck.py` checks normal-mode save/restore at the first command.

For `replaytest.py`, the playground's sysconf must allow the mode played
(`WIZARDS`, `EXPLORERS`) and have `RECORDFILE` and `SEED` unset: it records
to a file of its own with `NH_RECORD`, which is ignored when `RECORDFILE`
is set, and plays with a seed of its own. Run both scripts as the
playground's owner: `NH_RECORD` and replaying only work with the player's
own permissions.

### harnesstest.py and sysconftest.py: test harness regressions

`python3 test/harnesstest.py` checks fingerprint failure detection,
declined-interrupt coverage and normal-mode ending logic with synthetic
inputs. `python3 test/sysconftest.py` checks configuration guards, atomic
restoration after exceptions and signals, and recoverable backups after
SIGKILL. These use disposable files and need no game build. Linux CI runs
both before the game tests.

### recordfail.py: records that can't be written

`python3 test/recordfail.py playground` records a short game, then
restores it with the record on `/dev/full` (normal and explore mode) and
with the record padded up to the process's file size limit: each time the
game must end saved. It then checks that a replay leaves `replay.results`,
`replay.nethackrc` and `seedfuzz.txt` in the playground alone, and that a
record with an out-of-range value or an embedded NUL is rejected. Same
sysconf and permissions as `replaytest.py`; Linux CI runs it.

### panictest.py: error saves

`python3 test/panictest.py playground` invokes `#panic` in disposable
seeded and unseeded wizard games. Each must leave a separate `.e` error
save, never a normal save. Core dumps are disabled. Use a Unix tty build,
`WIZARDS=*`, and no server `SEED`, `RECORDFILE` or `CRASHREPORTURL`.
Linux CI runs it alongside the feed checks.

### savecheck.py: save refusals and recovery headers

`python3 test/savecheck.py playground` changes versions, revisions,
critical-byte counts and seed text in disposable seeded and unseeded
saves. Header refusals must show the file and build values. Each refusal
must exit normally with status 1, preserve a readable, recompressed save,
remove game locks, and leave score files alone, including after Ctrl-C and
a relaunch. Both recovery paths must reject incompatible headers and bad
name lengths without changing checkpoints or creating a save. A short
header fixture removes the entry too, so rejection cannot rely on the
remaining fields being misaligned. Current saves and checkpoints must
still restore. Successful startup waits for the feed's command boundary
and checks its new/restore flag; terminal redraws and `--More--` prompts
cannot hide a successful restore. A `NETHACKDIR` or `HACKDIR` too long to
use must stop the game rather than let it fall back to the compiled-in
playground, unless `-d` names the playground. Use a Unix tty build,
`WIZARDS=*`, and no server `SEED` or `RECORDFILE`. Linux CI runs it
alongside the feed checks.

### luatest.py: cached interpreters and configuration errors

`python3 test/luatest.py playground` opens a malformed symbol set through
the options menu in a new game and after saving and restoring. Both must
display and count the configuration error after Lua's cached interpreter
has loaded. Same playground requirements as `savecheck.py`; no debugger
is needed. Linux CI runs it.

### turncounter.py: running state after travel

`python3 test/turncounter.py playground` plays a seeded game with the
`time` option, travels a few squares and then searches or rests with counts,
with `runmode` run and teleport. After each command the last `T:` drawn on the
terminal must be the feed's turn: travel used to leave `context.run` set,
which held the counter back and suppressed interruption on full recovery.
With `--gdb`, the test also sets health or energy one below maximum and
grants regeneration after travel, without a command that would clear the
stale state. Both searches and rests must stop early with the recovery
message. This option needs Linux and gdb, and a build with debug symbols;
Linux CI runs it. Games use normal mode in disposable playground copies,
with no server `SEED` or `RECORDFILE` and matching real/effective IDs.
