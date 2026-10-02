#!/usr/bin/env python3
"""History fuzzer for seeded games.

For each seed, run a game in explore mode with NH_SEEDFUZZ set, on a
pseudo-terminal, with no shell.  The game (see seedfuzz_run() in
src/wizcmds.c) makes every level (apart from the tutorial and the endgame's
placeholder level; the Astral Plane with its player-monsters) as a
baseline, then again under each perturbation of the hero's history, and
writes what each level contains.  This script checks each pass against the
baseline, level by level:

  every pass       the level's layout (its hash, as the game made it; see
                   layout_trap() in src/mklev.c), terrain, engravings,
                   stairs, rooms and the number of draws the layout made
                   must match; so must traps (but, in the visit-order
                   passes, the Fort Ludios portal in a vault and where the
                   one on the Fort Ludios level leads, and a giant spider's
                   web on a monster's square where monsters may differ, all
                   of them outside the layout), objects, monsters and the
                   wandering monster timeline, but for what's allowed below;
                   and which artifact a random object becomes depends on
                   which artifacts already exist, so in any pass an artifact
                   (on either side), the object of the same type on its
                   square that stands in for it, and the rest of the
                   contents of a container holding one may differ, and so
                   may a monster's inventory holding one
  genocide         only monsters of the genocided species may be missing;
                   others may be placed on other squares, when one of the
                   two is near a gone monster or is one that another
                   monster placed elsewhere left or took (a gone monster
                   frees its square, and so does one placed elsewhere), a
                   group with a member near a gone monster may be placed
                   differently as a whole and fit in another number of
                   members, and a shapeshifter some of whose forms are gone
                   may be made differently
  uniques          only the "already killed" uniques may be missing (another
                   monster may take one's square, e.g. a king on a throne),
                   and others around may be placed differently, as for
                   genocide
  aggravate,       a monster whose species was picked by the level's
  amulet           difficulty (the game marks these) may be another, or
                   missing, or placed elsewhere; the others keep their own
                   species, but may be placed on other squares (one placed
                   differently takes or frees a square), or be missing if
                   one of those was near (it may have been turned away, or
                   its group may have fit in fewer members); on a level
                   where none of the marked ones differs, every monster
                   keeps its species and square; their level may differ,
                   and with it their hit points, peacefulness, a
                   shapeshifter's form and inventory (they're drawn after)
  gear             a monster's inventory may differ (seeing invisible lets
                   one that's peaceful when it's made carry invisibility),
                   and an Astral Plane player-monster's level and hit
                   points, drawn after its inventory; nothing else
  wandering        each level's timeline (which species turns up on which
  monsters         turn) must match, but for gone species and in the
                   aggravate and amulet passes
  everything else  identical: extinct, born, hero, fruit, progress (the
                   bottom of every dungeon reached, the invocation done),
                   hallu (hallucinating), turn (levels made on later turns),
                   ids (many monsters and objects made before), name (the
                   hero's name), and the visit orders apart from the
                   Fort Ludios portal

In any pass where a monster may differ, so may what was made as part of it
(the game marks these: a hider's object to hide under, and its contents).

Each level's fingerprint (the "F" line, as #levelhash shows it and the
dumplog lists it) must match too, part by part (the layout always), but a
part may differ where
the level's items of that part do differ (the itemized lines hold all that
the fingerprint covers, but its order) and the pass may change that part:
traps in the visit-order passes; traps (for webs), objects and monsters in
genocide, uniques, aggravate and amulet; objects and monsters in gear and
artifacts, and in any pass where an artifact is among the items that
differ.  A difference in the fingerprint alone means something it covers
depends on the hero's history without showing in the items.

Usage: seedfuzz.py [-n SEEDS] [-j JOBS] [--start N] [--keep]
                   [--magic-portal N] [--web N] PLAYGROUND

PLAYGROUND is an installed playground (with nethack, and a sysconf allowing
explore mode, e.g. EXPLORERS=*, with MAXPLAYERS (at most 25) at least JOBS,
and no SEED); run it as the playground's owner, since the game runs the
fuzzer only with the player's own permissions.  Seeds are "fuzz<N>".  Exits
non-zero if any seed shows a divergence.
"""
import argparse
import collections
import concurrent.futures
import fcntl
import os
import re
import select
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import time

MAGIC_PORTAL = 17  # trap type numbers (include/trap.h)
WEB = 18
PARTS = ("layout", "traps", "objects", "monsters")

ORDER_PASSES = {"order-down", "order-shuffle"}
# passes where monsters themselves may change: then so may what they leave
# on their square (a hider's hiding object, a giant spider's web, a king on
# a throne whose unique is gone)
MONSTER_PASSES = {"genocide", "uniques", "aggravate", "amulet", "gear"}
TOUGHER_PASSES = {"aggravate", "amulet"}
# the fingerprint's parts each pass may change
MAY_CHANGE = {
    "order-down": {"traps"}, "order-shuffle": {"traps"},
    "genocide": {"traps", "objects", "monsters"},
    "uniques": {"traps", "objects", "monsters"},
    "aggravate": {"traps", "objects", "monsters"},
    "amulet": {"traps", "objects", "monsters"},
    "gear": {"objects", "monsters"},
    "artifacts": {"objects", "monsters"},
}
FPART = re.compile(r"(layout)=([0-9a-f]{16})"
                   r"|(traps|objects|monsters)=([0-9a-f]{8})")
SANITIZER_ENV = ("ASAN_OPTIONS", "UBSAN_OPTIONS")
GROUP_REACH = 6  # how far from a gone monster its neighbouring group reaches
# the player-monsters' species (their neutral names)
MPLAYERS = {"archeologist", "barbarian", "cave_dweller", "healer", "knight",
            "monk", "cleric", "ranger", "rogue", "samurai", "tourist",
            "valkyrie", "wizard"}


def run_seed(playground, seed, workdir, timeout=300):
    """run the fuzzer for one seed, on a pseudo-terminal, with no shell"""
    out = os.path.join(workdir, "%s.txt" % seed)
    home = os.path.join(workdir, "home_" + seed)
    os.makedirs(home, exist_ok=True)
    name = seed.replace("fuzz", "fz")
    for f in os.listdir(playground):  # stale lock files from a crash
        if f.split(".")[0].endswith(name) and f[-1].isdigit():
            os.remove(os.path.join(playground, f))
    env = {"HOME": home, "TERM": "xterm", "NH_SEEDFUZZ": out,
           "PATH": os.environ.get("PATH", "/usr/bin:/bin"),
           "NETHACKOPTIONS": "seed:%s,!legacy,!tutorial,!news,"
                             "!splash_screen" % seed}
    # (NETHACKDIR: a playground other than the one compiled in, e.g. an
    # unpacked release tarball)
    for v in SANITIZER_ENV + ("NETHACKDIR",):
        if v in os.environ:
            env[v] = os.environ[v]
    master, slave = os.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
    proc = subprocess.Popen(["./nethack", "-X", "-u", name], cwd=playground,
                            env=env, stdin=slave, stdout=slave, stderr=slave,
                            start_new_session=True)
    os.close(slave)
    end = time.time() + timeout
    nextkey = 0.0
    try:
        while proc.poll() is None:
            if time.time() > end:
                os.killpg(proc.pid, signal.SIGKILL)
                proc.wait()
                raise TimeoutError("the game didn't finish")
            r, _, _ = select.select([master], [], [], 0.2)
            if r:
                try:
                    os.read(master, 65536)
                except OSError:
                    pass
            if time.time() > nextkey:
                # (Escape, in case anything asks something)
                try:
                    os.write(master, b"\033\r")
                except OSError:
                    pass
                nextkey = time.time() + 0.5
    finally:
        os.close(master)
    return out


def parse(path):
    """-> ({pass name: {"genocided": set, "uniques": set,
                         "levels": {ledger: level}}}, complete)"""
    passes = {}
    cur: dict = {}
    lvl: dict = {}
    lastpos = (0, 0)
    complete = False
    with open(path) as fp:
        for line in fp:
            f = line.split()
            if not f:
                continue
            tag = f[0]
            if tag == "P":
                cur = {"genocided": set(), "uniques": set(), "levels": {}}
                passes[f[2]] = cur
            elif tag == "G":
                cur["genocided"].add(int(f[1]))
            elif tag == "U":
                cur["uniques"].add(int(f[1]))
            elif tag == "L":
                lvl = {"depth": int(f[3]), "draws": f[4], "layout": f[5],
                       "terrain": f[6], "traps": [], "engr": [],
                       "stairs": [], "rooms": [], "objects": [],
                       "monsters": [], "spawns": [], "fp": None}
                cur["levels"][int(f[2])] = lvl
            elif tag == "F":
                lvl["fp"] = {m[0] or m[2]: m[1] or m[3]
                             for m in FPART.findall(line)}
            elif tag == "R":
                # ttyp, x, y, seen, to dn, to dl, "l" if part of the layout
                lvl["traps"].append(tuple(int(x) for x in f[1:7]) + (f[7],))
            elif tag == "N":  # an engraving: x, y, type, read, text
                lvl["engr"].append(line.rstrip("\n").split(" ", 5)[1:])
            elif tag == "T":  # stairs: x, y, up, ladder, to dn, to dl
                lvl["stairs"].append(tuple(int(x) for x in f[1:]))
            elif tag == "Q":  # a room: lx, ly, hx, hy, rtype, lit
                lvl["rooms"].append(tuple(int(x) for x in f[1:]))
            elif tag in "IJ":  # a monster's inventory (J: in a container)
                # otyp, quan, spe, blessed, cursed, species, artifact,
                # erodeproof
                m = lvl["monsters"][-1]
                m["inv"].append(tuple(int(x) for x in (f[1], f[4], f[5],
                                                       f[6], f[7], f[8],
                                                       f[9], f[12])))
            elif tag in "OBC":
                obj = [int(x) for x in f[1:10]]
                if tag == "C":  # contents: at their container's square
                    obj[1], obj[2] = lastpos
                else:
                    lastpos = (obj[1], obj[2])
                # f[10]: name, f[11]: "m" if made as part of a monster,
                # f[12]: erodeproof
                lvl["objects"].append((tag,) + tuple(obj) + tuple(f[10:13]))
            elif tag == "M":
                # f[8]: its own species, f[9]: shapeshifter, f[10]: "p" if
                # its species was picked by the level's difficulty, f[11]:
                # its hit points
                lvl["monsters"].append({"species": int(f[1]),
                                        "x": int(f[2]), "y": int(f[3]),
                                        "lev": int(f[4]),
                                        "peaceful": int(f[5]),
                                        "name": f[7], "own": int(f[8]),
                                        "shifter": f[9] == "1",
                                        "picked": f[10] == "p",
                                        "hp": int(f[11]), "inv": []})
            elif tag == "W":  # a wandering monster: turn, species
                lvl["spawns"].append((int(f[1]), int(f[2])))
            elif tag == "E":
                complete = True
    return passes, complete


def monkey(m, inventory=True):
    """a monster for comparison: species, square, level, peacefulness,
    name, hit points, and (normally) inventory"""
    k = (m["species"], m["x"], m["y"], m["lev"], m["peaceful"], m["name"],
         m["hp"])
    return k + (tuple(sorted(m["inv"])),) if inventory else k


def has_artifact(key):
    return any(i[6] for i in key[-1])


def monster_diff(bmons, qmons):
    """monsters that differ, as (key, own species, in base, shapeshifter);
    a monster whose inventory differs only because one side holds an
    artifact (which artifact is made depends on which already exist) isn't
    counted"""
    bm = collections.Counter(monkey(m) for m in bmons)
    qm = collections.Counter(monkey(m) for m in qmons)
    only_b, only_q = bm - qm, qm - bm
    for k in list(only_b.elements()):
        for j in list(only_q.elements()):
            if (only_b[k] and only_q[j] and k[:-1] == j[:-1]
                    and (has_artifact(k) or has_artifact(j))):
                only_b[k] -= 1
                only_q[j] -= 1
                break
    species = {monkey(m): m["own"] for m in bmons + qmons}
    shifter = {monkey(m): m["shifter"] for m in bmons + qmons}
    return ([(k, species[k], True, shifter[k]) for k in only_b.elements()]
            + [(k, species[k], False, shifter[k])
               for k in only_q.elements()])


def near(squares, dist=2):
    return {(x + dx, y + dy) for x, y in squares
            for dx in range(-dist, dist + 1) for dy in range(-dist, dist + 1)}


def tougher_monsters_diff(bmons, qmons):
    """aggravate, amulet: monsters that differ beyond what tougher ones can
    change"""
    changed = collections.Counter((m["own"], m["x"], m["y"]) for m in bmons
                                  if m["picked"])
    changed.subtract((m["own"], m["x"], m["y"]) for m in qmons
                     if m["picked"])
    if not any(changed.values()):
        # none of those differs: every monster keeps its species and square
        bk = collections.Counter((m["own"], m["x"], m["y"]) for m in bmons)
        qk = collections.Counter((m["own"], m["x"], m["y"]) for m in qmons)
        return ["species %d at %d,%d %s" % (k + (what,))
                for diff, what in ((bk - qk, "missing"), (qk - bk, "added"))
                for k in diff.elements()]
    # squares near a monster picked by difficulty that differs: another
    # there may have been turned away, and a group with a member there may
    # have fit in another number of members
    squares = {(k[1], k[2]) for k, n in changed.items() if n}
    moved_near = near(squares)
    group_near = near(squares, GROUP_REACH)
    grouped = {m["own"] for m in bmons + qmons
               if not m["picked"] and (m["x"], m["y"]) in moved_near}
    # the others keep their own species; they may be placed on other
    # squares (one placed differently takes or frees a square, which
    # changes where later ones land), and one near a changed monster may be
    # missing; their level (so hit points, and the random numbers drawn
    # after, hence also peacefulness, a shapeshifter's form and inventory)
    # may differ
    bad = []
    for mons, other, what in ((bmons, qmons, "missing"),
                              (qmons, bmons, "added")):
        mine = collections.Counter(m["own"] for m in mons if not m["picked"])
        theirs = collections.Counter(m["own"] for m in other
                                     if not m["picked"])
        for own, n in (mine - theirs).items():
            reach = group_near if own in grouped else moved_near
            nearby = sum(1 for m in mons if not m["picked"]
                         and m["own"] == own and (m["x"], m["y"]) in reach)
            if nearby < n:
                bad.append("%s %s species %d" % (n, what, own))
    return bad


def gear_monsters_diff(bmons, qmons):
    """gear: only a monster's inventory may differ (seeing invisible lets
    one that's peaceful when made carry invisibility, which shifts the rest
    of its random numbers; after its inventory, only an Astral Plane
    player-monster draws more, for its level and hit points)"""
    key = lambda m: (m["own"], m["x"], m["y"], m["peaceful"]) + (
        () if m["name"] in MPLAYERS else (m["lev"], m["hp"]))
    bk = collections.Counter(key(m) for m in bmons)
    qk = collections.Counter(key(m) for m in qmons)
    return list(((bk - qk) + (qk - bk)).elements())


def artifact_excused(diff):
    """the objects in diff that an artifact accounts for: artifacts, the
    object of the same type on the same square on the other side that
    stands in for one, and the other contents of a container holding
    one"""
    excused = set()
    arts = [i for i, o in enumerate(diff) if o[0][9]]
    for i in arts:
        o, side = diff[i]
        excused.add(i)
        for j, (p, pside) in enumerate(diff):
            if (j not in excused and pside != side and not p[9]
                    and p[:4] == o[:4]):
                excused.add(j)
                break
        if o[0] == "C":
            excused.update(j for j, (p, _) in enumerate(diff)
                           if p[0] == "C" and (p[2], p[3]) == (o[2], o[3]))
    return excused


def check_seed(path):
    """-> list of problem strings for this seed"""
    problems = []
    if not os.path.exists(path):
        return ["no output (the game didn't run the fuzzer)"]
    passes, complete = parse(path)
    if not complete:
        return ["output incomplete (the game crashed or timed out)"]
    base = passes["base"]["levels"]
    for pname, p in passes.items():
        if pname == "base":
            continue
        gone = p["genocided"] | p["uniques"]
        for ledger, b in base.items():
            q = p["levels"].get(ledger)
            where = "%s Dlvl %d (ledger %d)" % (pname, b["depth"], ledger)
            if q is None:
                problems.append(where + ": level missing")
                continue
            differs = set()  # parts whose items differ
            artifact_parts = set()  # ... and where an artifact does
            if b["layout"] != q["layout"]:
                problems.append(where + ": layout differs")
            if b["terrain"] != q["terrain"]:
                problems.append(where + ": terrain differs")
            for part in ("engr", "stairs", "rooms"):
                if b[part] != q[part]:
                    problems.append(where + ": %s differ: %s vs %s" %
                                    (part, b[part][:2], q[part][:2]))
            if b["draws"] != q["draws"]:
                problems.append(where + ": layout %s vs %s" %
                                (b["draws"], q["draws"]))

            # monsters (by their own species, for shapeshifters)
            bm = collections.Counter(monkey(m) for m in b["monsters"])
            qm = collections.Counter(monkey(m) for m in q["monsters"])
            if bm != qm:
                differs.add("monsters")
                if any(has_artifact(k) for k in (bm - qm) + (qm - bm)):
                    artifact_parts.add("monsters")
            mdiff = monster_diff(b["monsters"], q["monsters"])
            gonesquares = set()
            if pname in ("genocide", "uniques") and gone:
                # only monsters of the gone species may be missing; another
                # may turn up on a square one of them had (e.g. a throne),
                # and others may be placed on other squares nearby
                # (a shapeshifter whose form is gone takes another form; a
                # gone monster's square, and its long worm tail, may be
                # taken by another monster nearby)
                goners = [m for m in b["monsters"]
                          if m["own"] in gone or m["species"] in gone]
                gonesquares = near({(m["x"], m["y"]) for m in goners})
                mdiff = [d for d in mdiff
                         if not ((d[1] in gone or d[0][0] in gone) and d[2])]
                # (a shapeshifter, e.g. a vampire whose bat form is gone,
                # is made differently when some of its forms are gone)
                if pname == "genocide":
                    mdiff = [d for d in mdiff if not d[3]]
                unplaced = lambda k: (k[0],) + k[3:]
                inb = collections.Counter(unplaced(d[0]) for d in mdiff
                                          if d[2])
                inq = collections.Counter(unplaced(d[0]) for d in mdiff
                                          if not d[2])
                moved = inb & inq
                # a monster placed elsewhere has one of its two squares near
                # a gone monster, or one that another monster placed
                # elsewhere left or took (each turned away from, or let
                # onto, a square in turn); it frees its square for another
                # one that would have been turned away, e.g. a shop mimic
                movers = {k for k, n in moved.items() if n}
                spots = {k: {(d[0][1], d[0][2]) for d in mdiff
                             if unplaced(d[0]) == k} for k in movers}
                vacated = set(gonesquares)
                excused = set()
                grew = True
                while grew:
                    grew = False
                    for k in movers - excused:
                        if spots[k] & vacated:
                            excused.add(k)
                            vacated |= spots[k]
                            grew = True
                # (a group with a member near a gone monster may be placed
                # differently as a whole, each member next to the one
                # before, and fit in another number of members)
                grouped = {d[0][0] for d in mdiff
                           if (d[0][1], d[0][2]) in gonesquares}
                groupsquares = near({(m["x"], m["y"]) for m in goners},
                                    GROUP_REACH)
                bad = [d[0] for d in mdiff
                       if unplaced(d[0]) not in excused
                       and (d[0][1], d[0][2]) not in vacated
                       and not (d[0][0] in grouped
                                and (d[0][1], d[0][2]) in groupsquares)]
                if bad:
                    problems.append(where + ": monsters differ beyond the "
                                    "missing species: %s" % bad[:2])
            elif pname in TOUGHER_PASSES:
                bad = tougher_monsters_diff(b["monsters"], q["monsters"])
                if bad:
                    problems.append(where + ": monsters differ beyond "
                                    "tougher ones: %s" % bad[:2])
            elif pname == "gear":
                bad = gear_monsters_diff(b["monsters"], q["monsters"])
                if bad:
                    problems.append(where + ": monsters differ beyond "
                                    "their inventory: %s" % bad[:2])
            elif mdiff:
                problems.append(where + ": %d monster(s) differ: %s" %
                                (len(mdiff), [d[0] for d in mdiff][:2]))
            # squares where a trap may follow the monster (a giant
            # spider's web): in the passes where which monsters there are
            # may change, any monster's square, and near a gone monster
            if pname in MONSTER_PASSES - {"gear"}:
                monsquares = {(m["x"], m["y"]) for m in b["monsters"]
                              + q["monsters"]} | gonesquares
            else:
                monsquares = set()

            # traps: in the visit-order passes, the Fort Ludios portal in
            # a vault (outside the layout) may differ, and so may where the
            # one on the Fort Ludios level (in it) leads
            btraps = collections.Counter(b["traps"])
            qtraps = collections.Counter(q["traps"])
            if btraps != qtraps:
                differs.add("traps")
                only_b, only_q = btraps - qtraps, qtraps - btraps
                if pname in ORDER_PASSES:
                    nodest = lambda t: t[:4] + t[6:]
                    for c in (only_b, only_q):
                        for t in list(c):
                            if t[0] == MAGIC_PORTAL and t[6] == "-":
                                del c[t]
                    bk = collections.Counter(nodest(t) for t in
                                             only_b.elements()
                                             if t[0] == MAGIC_PORTAL)
                    qk = collections.Counter(nodest(t) for t in
                                             only_q.elements()
                                             if t[0] == MAGIC_PORTAL)
                    if bk == qk:
                        for c in (only_b, only_q):
                            for t in list(c):
                                if t[0] == MAGIC_PORTAL:
                                    del c[t]
                bad = [t for t in (only_b + only_q).elements()
                       if not (t[0] == WEB and t[6] == "-"
                               and (t[1], t[2]) in monsquares)]
                if bad:
                    problems.append(where + ": traps differ %s" % bad)

            # objects; in the passes where monsters may change, so may
            # what was made as part of one (a hider's object to hide under,
            # left behind if the monster was moved); which artifact a random
            # object becomes depends on which already exist
            bo = collections.Counter(b["objects"])
            qo = collections.Counter(q["objects"])
            if bo != qo:
                differs.add("objects")
                diff = [(o, True) for o in (bo - qo).elements()] \
                    + [(o, False) for o in (qo - bo).elements()]
                if any(o[9] for o, _ in diff):
                    artifact_parts.add("objects")
                diff = [d for d in diff
                        if not (pname in MONSTER_PASSES and d[0][11] == "m")]
                excused = artifact_excused(diff)
                diff = [d[0] for i, d in enumerate(diff) if i not in excused]
                if diff:
                    problems.append(where + ": %d object(s) differ: %s" %
                                    (len(diff), diff[:3]))

            # the fingerprint: a part may differ only where the level's
            # items of that part do differ, and the pass may change them
            # (or an artifact does, which any pass may change)
            if b["fp"] is None or q["fp"] is None:
                problems.append(where + ": fingerprint missing")
            else:
                for part in PARTS:
                    if b["fp"].get(part) != q["fp"].get(part) and not (
                            part in differs
                            and (part in MAY_CHANGE.get(pname, ())
                                 or part in artifact_parts)):
                        problems.append(where + ": fingerprint's %s differ"
                                        " (%s vs %s)"
                                        % (part, b["fp"].get(part),
                                           q["fp"].get(part)))

            # the wandering monster timeline: the same, but for gone
            # species (genocide), and tougher ones (aggravate, amulet)
            if pname not in TOUGHER_PASSES:
                bs = [w for w in b["spawns"] if w[1] not in gone]
                if bs != q["spawns"]:
                    problems.append(where + ": wandering monsters differ: "
                                    "%s vs %s" % (bs[:3], q["spawns"][:3]))
    return problems


def main():
    global MAGIC_PORTAL, WEB
    ap = argparse.ArgumentParser(description=(__doc__ or "").split("\n")[0])
    ap.add_argument("playground")
    ap.add_argument("-n", type=int, default=100, help="number of seeds")
    ap.add_argument("-j", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--start", type=int, default=0)
    ap.add_argument("--keep", action="store_true",
                    help="keep the per-seed output files")
    ap.add_argument("--magic-portal", type=int, default=MAGIC_PORTAL,
                    help="trap type number of MAGIC_PORTAL")
    ap.add_argument("--web", type=int, default=WEB,
                    help="trap type number of WEB")
    args = ap.parse_args()
    MAGIC_PORTAL, WEB = args.magic_portal, args.web
    playground = os.path.abspath(args.playground)
    workdir = tempfile.mkdtemp(prefix="seedfuzz-")
    seeds = ["fuzz%d" % i for i in range(args.start, args.start + args.n)]

    failures = collections.Counter()
    bad_seeds = 0
    with concurrent.futures.ThreadPoolExecutor(args.j) as ex:
        futs = {ex.submit(run_seed, playground, s, workdir): s
                for s in seeds}
        for fut in concurrent.futures.as_completed(futs):
            seed = futs[fut]
            try:
                path = fut.result()
                problems = check_seed(path)
            except Exception as e:  # timeout and the like
                problems = ["run failed: %s" % e]
            if problems:
                bad_seeds += 1
                print("%s: %d problem(s)" % (seed, len(problems)))
                for pr in problems[:20]:
                    print("    " + pr)
                for pr in problems:
                    failures[pr.split(":")[0].split()[0]] += 1
            if not args.keep:
                path = os.path.join(workdir, "%s.txt" % seed)
                if os.path.exists(path):
                    os.remove(path)
    print("\n%d seeds, %d with problems" % (len(seeds), bad_seeds))
    for pname, n in failures.most_common():
        print("  %-14s %d" % (pname, n))
    if args.keep:
        print("output kept in", workdir)
    return 1 if bad_seeds else 0


if __name__ == "__main__":
    sys.exit(main())
