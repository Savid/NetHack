# AGENTS.md

A fork of NetHack 5.0 (C, bundled Lua 5.5) that adds seeded games, so
several players can race through the same dungeon. `Seeding` in this
directory is the design document; read it before touching anything it lists.
`README` has the player-facing summary. Everything else is upstream NetHack.

## Build and test

```sh
sh sys/unix/setup.sh sys/unix/hints/linux.501  # writes the Makefiles
make all install                       # installs to ./playground (HACKDIR)
make update                            # reinstall, keeping playground/sysconf
make -C src nethack                    # rebuild the game only
make -C util sfctool                   # save-file converter, must keep building
rm -f src/date.o                       # before a race build, so build= is honest
```

`make install` deletes `./playground` first, `sysconf` included, and copies
`sys/unix/sysconf` in; `make update` replaces only the game and data files.

Ubuntu needs `build-essential libncurses-dev uuid-dev pkg-config curl`, plus
`universal-ctags` for sfctool (its build regenerates `include/sfproto.h`
and `util/sfdata.c`); macOS the same from Homebrew (`ncurses pkg-config
universal-ctags`). No groff is needed unless you build the Guidebook.

The build must stay warning-free with the Linux hints. The test scripts need
`playground/sysconf` to have `WIZARDS=*`, `EXPLORERS=*`, `MAXPLAYERS=25` and
no `SEED` or `RECORDFILE`; the installed `sysconf` has `WIZARDS=root games`
and `MAXPLAYERS=10`, so set those two after `make install`. Run them as the
playground's owner (they refuse to run setgid):

```sh
python3 test/seedfuzz.py -n 300 -j 22 playground      # history fuzzer
python3 test/replaytest.py -k 3000 -s 4 playground    # record, then replay
python3 test/replaytest.py -k 3000 -s 4 --signals playground
python3 test/recordfail.py playground                 # records that fail
python3 test/savecheck.py playground                  # save refusals/recovery
python3 test/luatest.py playground                    # cached Lua diagnostics
python3 test/panictest.py playground                  # separate error saves
python3 test/layouttest.py playground                 # layout dumps, keyframes
playground/nethack --replay RECORD --verify           # check one record
```

Run `seedfuzz.py` after any change to level generation, monster or object
creation, or `src/rnd.c`; it also checks that every level's layout (the
first part of its fingerprint) never depends on the hero's history. Run
`replaytest.py`, `recordfail.py` and `savecheck.py` after any change to
input, signals, saving, restoring, or `src/files.c`.
`test/README.md` explains them.

CI (`.github/workflows/ci.yml`) does all of the above on every push and PR,
and a `v*` tag publishes release tarballs made by `sys/unix/mkrelease.sh`
(unpack anywhere, run `./nethack`; the game falls back to the playground's
own `sysconf` when the compiled-in path is missing, see `sysconf_file()`).
Linux runs the fuzzer and replay tests; macOS builds, packages and makes
one seed's levels from its tarball. Every platform writes one seed's
layout hashes (`--layout-hashes`) from its tarball, and a final job
requires them to be the same bytes. Windows is compiled (MSYS2) but never
run; that job may fail without blocking anything, and isn't released.
Both Linux architectures build in Ubuntu 22.04 containers (glibc 2.35),
independently of the hosted runner's OS. Separate Debian 12 and 13 jobs
run both x86_64 and aarch64 release tarballs and gate publication on
compatibility. The supported Linux baseline is glibc 2.35 or newer.

## Invariants (the reason this fork exists)

- Same seed, same dungeon, whatever the player did before and in whatever
  order levels are visited. Generation may read the level, the seed, and the
  stand-in hero (`stand_in_hero_begin()`), never the real hero, the clock,
  `mvitals`, artifacts, the high score list, or `svc.context.ident`.
- Every monster or object made while a level is generated goes through
  `rng_content_enter()`/`rng_content_leave()` (already inside `makemon()`,
  `mksobj()`, `mkobj()`, `mkcorpstat()`). A stream that is only sometimes
  needed is still entered every time. Placement retries use
  `rng_placement_begin()`. See Seeding B4.2 before adding a stream.
- Genocide checks during play use `species_genocided()`, not `mvflags`
  directly. Generation ignores genocide and drops the genocided monster.
- Unseeded play follows NetHack 5.0, with the two tie-breaking sorts
  and the Lua error-reporting, panic-save, save-refusal, long-NETHACKDIR
  and tty hangup fixes in Seeding.
  The opt-in `--managed-session` policy (Seeding B13)
  restricts quit and save-and-exit; unseeded save files remain
  interchangeable with the matching upstream version either way.
- After the first race release, if a change makes the same seed give a
  different dungeon, bump `SEED_GEN_VERSION` in `include/global.h`.
  Before that release, keep it at 1 and document generation changes in
  Seeding; compare prerelease games by build and data hash too.
- Seeded state lives in `gseed` (`include/decl.h`, initialised in
  `src/decl.c`); the recorder's in `nhrec` (`src/files.c`). A seeded
  save holds what `savegamestate()` writes behind `SEEDED_GAME_BIT`, in
  one format: change `save.c`, `restore.c` and Seeding B7 together.
- Keep Seeding in step: its "changes" bullets, C1 file list, and the README
  summary describe the code as it is, not as it was.
- The live feed (`src/feed.c`, on with `NETHACK_FEED_FD=N`) only reads the
  game: no random numbers (not even the display RNG), no state changes.
  Naming is not pure: name objects with `gd.quietnaming` and `gd.distantname`
  set and the knowledge bits put back, as `feed_obj_block()` does, and name
  monsters from the tables, never with `x_monnam()` (`shkname()` draws from
  the core RNG while hallucinating). Anything new reachable from naming that
  writes state or draws a number goes behind `gd.quietnaming` (`objnam.c`,
  `eat.c`, `artifact.c`, `invent.c` have the cases). After changing the feed,
  its hooks or naming code, run `python3 test/feedtest.py playground`: it
  compares `NH_STATELOG` logs (RNG draws and a state hash after every key)
  with the feed on and off, which the record's digests don't cover. After
  changing keyframes, the layout (`layout_trap()`, `layout_hash()` in
  `mklev.c`) or `nethack --layouts`, also run `test/layouttest.py`: it
  folds feeds against the dump and the `chk` lines.

## Code style

Upstream NetHack conventions, not modern C. Match the surrounding file:
`staticfn` for file-local functions, prototypes at the top of the file,
`Sprintf`/`Snprintf`/`Strcpy`, `genericptr_t` casts, `boolean`/`TRUE`/`FALSE`,
declarations at block start, 79-column lines, comments in the existing
lower-case voice. Seeded-game additions are guarded so that builds without
`USE_ISAAC64`, `DUMPLOG`, `SYSCF` or `HANGUPHANDLING`, and the `SFCTOOL`
build, still compile.

## Boundaries

- Never commit or print a server's `SEED` value, a record file, or a seeded
  save; they give away the race.
- Do not touch `submodules/`, the `dat/*.lua` level scripts, or the
  `include/monsters.h` and `include/objects.h` tables unless the task is
  about them; they change what every seed produces.
- Ask before changing the record format, the fingerprint (`level_fingerprint()`,
  `layout_hash()`), or the digest (`nhrec_digest()`): existing records and
  reference games stop verifying.
- Commit only when asked. Never rebase or force-push `master`.

## Branches and taking upstream changes

- `master` is the fork: upstream plus the seeded-game work. Releases are
  tags on it named `vMAJOR.MINOR.PATCH`. `v0.x` tags, and any tag with a
  suffix such as `v1.0.0-rc.1`, are pre-releases for trying the pipeline
  and the game (CI marks them so); `v1.0.0` is the first build meant for
  a real race. After that, bump PATCH for fixes,
  MINOR for features, MAJOR when the same seed gives a different dungeon
  (which also bumps `SEED_GEN_VERSION`, a separate counter that tracks the
  generator, not the release).
- `NetHack-5.0` is a mirror of upstream NetHack's branch of the same name.
  Never commit to it; only fast-forward it. `git diff NetHack-5.0..master`
  is always the whole seeded patch.
- Remotes: `origin` is this fork, `upstream` is
  `https://github.com/NetHack/NetHack.git` (add it if missing).

To pull upstream in:

```sh
git fetch upstream
git checkout NetHack-5.0 && git merge --ff-only upstream/NetHack-5.0
git push origin NetHack-5.0
git checkout master && git merge NetHack-5.0      # merge, never rebase
```

Conflicts land in the files Seeding C1 lists; resolve them keeping the
invariants above, then rebuild warning-free and run `seedfuzz.py` and
`replaytest.py` before pushing. If the merge changed anything in level
generation (dungeon.lua, `dat/*.lua`, makemon, mkobj, mklev, sp_lev, the
monster or object tables), the same seed now gives a different dungeon:
bump `SEED_GEN_VERSION` after the first race release; keep it at 1 during
prerelease development, and say what changed in Seeding. Check upstream
commits for new places that read `mvitals` directly, the clock, `ubirthday`
or `u.ulevel` during generation, since those are the leaks this fork plugs.
Keep save-revision migrations out of the fork: resolve changes to the
deleted `src/revision.c` and `include/revision.h` by retaining their deletion
and removing any new callers or build references. Saves require the current
version and revision.
