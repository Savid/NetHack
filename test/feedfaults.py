#!/usr/bin/env python3
"""Linux/gdb regression fixtures for feed error recovery and rendering.

Only disposable games are modified. Requires a debug build and WIZARDS=*.
Run: python3 test/feedfaults.py playground
"""
import argparse
import fcntl
import json
import os
import re
import shutil
import select
import signal
import struct
import sys
import tempfile
import termios
import threading
import time
import feedgame as nhgame


class Trial:
    def __init__(self, options="", setup=None, race="human"):
        self.work = tempfile.mkdtemp(prefix="feedfaults-")
        self.pg = os.path.join(self.work, "pg")
        nhgame.copy_playground(self.pg)
        if setup:
            setup(self.pg)
        self.g = nhgame.Game(
            self.pg,
            "feedfaults",
            "",
            mode="wizard",
            debuggable=True,
            extra_env={
                "NETHACKOPTIONS": "role:Valkyrie,race:"
                + race
                + ",gender:female,align:lawful,!legacy,!news,"
                "!splash_screen,!tutorial,!autopickup"
                + ("," + options if options else "")
            },
        )
        self.g.read_feed()
        if not self.g.first_command():
            raise RuntimeError("the game never asked for a command")

    def data(self):
        good = []
        bad = []
        for ln in bytes(self.g.feed).split(b"\n")[:-1]:
            try:
                good.append(json.loads(ln))
            except Exception:
                bad.append(ln)
        return good, bad

    def wait_data(self, done, secs=5):
        """the feed's lines once done(lines) holds, or secs have gone"""
        end = time.time() + secs
        while True:
            lines = self.data()[0]
            if done(lines) or time.time() > end:
                return lines
            time.sleep(0.02)

    def close(self):
        if self.g.alive:
            self.g.finish(secs=5)
        if self.g.alive:
            os.kill(self.g.pid, signal.SIGKILL)
        self.g.close()
        self.g.feed_thread.join(2)
        shutil.rmtree(self.work)


def drive_debug(x, commands, seconds=20):
    box = []

    def work():
        try:
            box.append(x.g.gdb(commands, timeout=seconds + 5))
        except Exception as e:
            box.append(e)

    th = threading.Thread(target=work)
    th.start()
    end = time.time() + seconds
    while th.is_alive() and time.time() < end:
        x.g.drain(0.05)
        if "--More--" in x.g.tail:
            x.g.tail = ""
            x.g.send(" ", settle=0.03)
        if "Report now?" in x.g.tail:
            x.g.tail = ""
            x.g.send("n", settle=0.03)
    th.join(6)
    if not box:
        raise RuntimeError("debugger did not complete")
    if isinstance(box[0], Exception):
        raise box[0]
    return box[0]


def check(name, test):
    x = Trial(options="showrace,color,pettype:cat", race="dwarf")
    try:
        test(x)
        assert not x.data()[1], "malformed JSON"
        print(name, "PASS", flush=True)
    finally:
        x.close()


def nested(x):
    r = x.g.gdb(
        [
            'call (void) fb_begin("outer")',
            'call (void) fb_str("before", "one")',
            "set $n1 = (int) fb_nest()",
            'call (void) fb_begin("middle")',
            "set $n2 = (int) fb_nest()",
            "call (void) feed_key(32)",
            "call (void) fb_unnest($n2)",
            'call (void) fb_str("value", "two")',
            "call (void) fb_end()",
            "call (void) fb_unnest($n1)",
            'call (void) fb_str("after", "three")',
            "call (void) fb_end()",
            "call (void) feed_flush()",
        ]
    )
    assert not r.stderr, r.stderr
    d = x.wait_data(lambda d: {"outer", "middle"} <= {v["k"] for v in d})
    assert any(
        v.get("k") == "outer"
        and v.get("before") == "one"
        and v.get("after") == "three"
        for v in d
    )
    assert any(v.get("k") == "middle" and v.get("value") == "two" for v in d)


def overrides(x):
    before = len(x.data()[0])
    r = x.g.gdb(
        [
            "set $sx = sizeof(gs.showsyms)/sizeof(gs.showsyms[0])-6",
            "set sysopt.accessibility = 1",
            "set go.ov_primary_syms[$sx+4] = 33",
            "set go.ov_primary_syms[$sx+5] = 64",
            "set gs.showsyms[$sx+4] = 33",
            "set gs.showsyms[$sx+5] = 64",
            "call (void) reset_glyphmap(gm_accessibility_change)",
            "call (void) feed_boundary()",
            "set feed_signalled = 1",
            "call (void) feed_boundary()",
        ]
    )
    assert not r.stderr, r.stderr
    d = x.wait_data(lambda d: any(v["k"] == "kf" for v in d[before:]))
    kf = [v for v in d[before:] if v["k"] == "kf"][-1]
    assert kf["hero"]["screen"][1] == "h", kf["hero"]["screen"]
    pets = [v for v in kf["level"]["sym"] if v[3] == "kitten"]
    assert pets and all(v[1] == "f" for v in pets)


def transition(x):
    before = len(x.data()[0])
    x.g.tail = ""
    x.g.send("\026valley\r")
    end = time.time() + 10
    while "--More--" not in x.g.tail and x.g.alive and time.time() < end:
        x.g.drain(0.05)
    assert "--More--" in x.g.tail
    os.kill(x.g.pid, signal.SIGUSR1)
    x.g.drain(1.3)
    assert not any(v["k"] == "kf" for v in x.data()[0][before:])
    for _ in range(12):
        if "--More--" not in x.g.tail:
            break
        x.g.tail = ""
        x.g.send(" ")
        x.g.drain(0.1)
    d = x.data()[0][before:]
    lev = [i for i, v in enumerate(d) if v.get("ev") == "level"]
    kf = [i for i, v in enumerate(d) if v["k"] == "kf"]
    assert len(lev) == 1 and len(kf) == 1 and lev[0] < kf[0], (lev, kf)


def naming_fault(x):
    before = len(x.data()[0])
    result = drive_debug(
        x,
        [
            "set $fruit = (struct obj *) mksobj(SLIME_MOLD, 0, 0)",
            "set $fruit = (struct obj *) addinv($fruit)",
            "set $spe = $fruit->spe",
            "set $fruit->spe = -127",
            "call (void) feed_boundary()",
            "set $fruit->spe = $spe",
        ],
    )
    assert not result.stderr, result.stderr
    assert x.g.alive
    events = x.data()[0][before:]
    assert any("Bad fruit" in v.get("text", "") for v in events)
    assert any(v.get("k") == "key" for v in events)


def feed_objects(lines):
    """the last record written of each object in lines, by id, containers'
    contents and monsters' inventories included"""
    found = {}

    def walk(objs):
        for o in objs:
            found[o["id"]] = o
            walk(o.get("contents", []))

    for v in lines:
        if v["k"] == "kf":
            walk(v["inv"]["items"])
            walk(v["level"]["objects"])
            for m in v["level"]["monsters"]:
                walk(m.get("inv", []))
        elif v["k"] == "inv":
            walk(v["items"])
        elif v["k"] == "obj":
            walk(v["upd"])
        elif v["k"] == "mon":
            for m in v.get("upd", []):
                walk(m.get("inv", []))
    return found


def gdb_values(out, tag):
    """the name=number pairs of gdb's 'tag ...' line"""
    for line in out.splitlines():
        if line.startswith(tag + " "):
            return {k: int(v) for k, v in
                    (w.split("=") for w in line.split()[1:])}
    raise AssertionError("gdb wrote no %s line: %s" % (tag, out))


def shop_bill(x):
    """Observing an unpaid item must not apply a pending anger surcharge,
    but its price is the one the game shows once it does."""
    before = len(x.data()[0])
    result = drive_debug(x, [
        "set $room = &svr.rooms[0]",
        "set $room->rtype = 14",
        "set $ok = (int) shkinit(&shtypes[0], $room)",
        "set $shk = $room->resident",
        "set $eshk = $shk->mextra->eshk",
        "set u.ushops[0] = $eshk->shoproom",
        "set u.ushops[1] = 0",
        "set $obj = gi.invent",
        "set $obj->unpaid = 1",
        "set $eshk->billct = 1",
        "set $eshk->bill_p = &$eshk->bill[0]",
        "set $eshk->bill[0].bo_id = $obj->o_id",
        "set $eshk->bill[0].bquan = $obj->quan",
        "set $eshk->bill[0].price = 30",
        "call (void) setmangry($shk, 0)",
        "call (void) feed_boundary()",
        'printf "bill=%ld surcharge=%d suppress=%d\\n", '
        "$eshk->bill[0].price, $eshk->surcharge, iflags.suppress_price",
        # what the game shows, riling the shopkeeper as it looks
        'printf "shown id=%u quan=%ld price=%ld\\n", $obj->o_id, '
        "$obj->quan, unpaid_cost($obj, 0)",
    ])
    assert not result.stderr, result.stderr
    assert "bill=30 surcharge=0 suppress=0\n" in result.stdout, result.stdout
    shown = gdb_values(result.stdout, "shown")
    assert shown["price"] == 40 * shown["quan"], shown
    d = x.wait_data(lambda d: shown["id"] in feed_objects(d[before:]))
    obj = feed_objects(d[before:])[shown["id"]]
    assert obj.get("price") == shown["price"], (obj, shown)


def shop_prices(x):
    """Objects carry the prices the game shows, found without changing
    anything: unpaid ones in the inventory (a container's contents too),
    the shop's own on its floor while the hero is in the shop (as looking
    at them quotes, having seen them up close), none outside a shop, an
    angry shopkeeper's surcharge before it has been applied, and the bill
    of a shopkeeper who is away."""
    pre = drive_debug(x, [
        'printf "where rno=%d\\n", svl.level.locations[u.ux][u.uy].roomno',
    ])
    assert not pre.stderr, pre.stderr
    assert gdb_values(pre.stdout, "where")["rno"] >= 3, "hero not in a room"
    before = len(x.data()[0])
    state_line = (
        'printf "TAG ok=%d fx=%d fy=%d sx=%d sy=%d held=%d core=%lu '
        'disp=%lu billct=%d bill0=%ld bill1=%ld bill2=%ld surcharge=%d '
        'peaceful=%d quote=%lu unseen=%d\\n", $ok, $fx, $fy, '
        "$eshk->shk.x, $eshk->shk.y, $held[0], nh_rng_draws[0], "
        "nh_rng_draws[1], $eshk->billct, $eshk->bill[0].price, "
        "$eshk->bill[1].price, $eshk->bill[2].price, $eshk->surcharge, "
        "$shk->mpeaceful, " + " + ".join(
            "objects[%s].oc_buy_maxseen" % t for t in (
                "SCR_ENCHANT_ARMOR", "SACK", "POT_HEALING", "DAGGER",
                "LARGE_BOX", "WAN_STRIKING", "RIN_PROTECTION",
                "WORTHLESS_BLUE_GLASS")) + ", !$gem->dknown")
    result = drive_debug(x, [
        "set feed_signalled = 1",
        "call (void) feed_boundary()",
        # the hero's room becomes a general store
        "set $rno = svl.level.locations[u.ux][u.uy].roomno",
        "set $room = &svr.rooms[$rno - 3]",
        "set $room->rtype = 14",
        "set $ok = (int) shkinit(&shtypes[0], $room)",
        "set $shk = $room->resident",
        "set $eshk = $shk->mextra->eshk",
        "set u.ushops[0] = $rno",
        "set u.ushops[1] = 0",
        # picked up: two scrolls, and a sack with a potion in it
        "set $inv = (struct obj *) mksobj(SCR_ENCHANT_ARMOR, 0, 0)",
        "set $inv->quan = 2",
        "set $inv->owt = (int) weight($inv)",
        "set $inv = (struct obj *) addinv($inv)",
        "call (void) addtobill($inv, 1, 0, 1)",
        "set $bag = (struct obj *) mksobj(SACK, 0, 0)",
        "set $in1 = (struct obj *) mksobj(POT_HEALING, 0, 0)",
        "set $in1 = (struct obj *) add_to_container($bag, $in1)",
        "set $bag = (struct obj *) addinv($bag)",
        "call (void) addtobill($bag, 1, 0, 1)",
        # for sale on the floor of the shop proper, away from the hero and
        # the shopkeeper's own spot: three daggers, a large box with a wand
        # and a ring in it (seen, as looting shows them), and a piece of
        # glass of an identified kind not yet seen up close, which the
        # shopkeeper prices as a gem until it is
        "set $fx = 0",
        "set $x = $room->lx",
        "while $x <= $room->hx && !$fx",
        "set $y = $room->ly",
        "while $y <= $room->hy && !$fx",
        "if inside_shop($x, $y) == $rno "
        "&& ($x != $eshk->shk.x || $y != $eshk->shk.y) "
        "&& ($x != u.ux || $y != u.uy)",
        "set $fx = $x",
        "set $fy = $y",
        "end",
        "set $y = $y + 1",
        "end",
        "set $x = $x + 1",
        "end",
        "set $floor = (struct obj *) mksobj(DAGGER, 0, 0)",
        "set $floor->quan = 3",
        "set $floor->owt = (int) weight($floor)",
        "call (void) place_object($floor, $fx, $fy)",
        "set $box = (struct obj *) mksobj(LARGE_BOX, 0, 0)",
        "set $in2 = (struct obj *) mksobj(WAN_STRIKING, 0, 0)",
        "set $in2->dknown = 1",
        "set $in2 = (struct obj *) add_to_container($box, $in2)",
        "set $in3 = (struct obj *) mksobj(RIN_PROTECTION, 0, 0)",
        "set $in3->dknown = 1",
        "set $in3 = (struct obj *) add_to_container($box, $in3)",
        "call (void) place_object($box, $fx, $fy)",
        "set $gem = (struct obj *) mksobj(WORTHLESS_BLUE_GLASS, 0, 0)",
        "set $gem->quan = 1",
        "set $gem->owt = (int) weight($gem)",
        "set $gem->dknown = 0",
        "set objects[WORTHLESS_BLUE_GLASS].oc_name_known = 1",
        "call (void) place_object($gem, $fx, $fy)",
        # what the feed must leave alone: in_rooms()'s buffer (a caller
        # may hold it across a wait), the RNGs, the bill, the shopkeeper's
        # temper, the remembered price quotes and what has been seen
        "set $held = in_rooms(u.ux, u.uy, 0)",
        "set $held[0] = 99",
        state_line.replace("TAG", "state"),
        "set feed_signalled = 1",
        "call (void) feed_boundary()",
        state_line.replace("TAG", "after"),
        # what the game shows: the itemized bill (one object) and the name
        # (with its contents), and what looking at the floor quotes
        "set $nc = (int *) alloc(sizeof (int))",
        'printf "inv id=%u own=%ld all=%ld\\n", $inv->o_id, '
        "unpaid_cost($inv, 0), unpaid_cost($inv, 1)",
        'printf "bag id=%u own=%ld all=%ld\\n", $bag->o_id, '
        "unpaid_cost($bag, 0), unpaid_cost($bag, 1)",
        'printf "in1 id=%u own=%ld\\n", $in1->o_id, unpaid_cost($in1, 0)',
        'printf "gem id=%u unseen=%ld\\n", $gem->o_id, '
        "get_cost_of_shop_item($gem, $nc)",
        'printf "floor id=%u\\n", $floor->o_id',
        'printf "box id=%u\\n", $box->o_id',
        'printf "in2 id=%u\\n", $in2->o_id',
        'printf "in3 id=%u\\n", $in3->o_id',
    ] + ['printf "look %%u %%s\\n", $%s->o_id, doname_with_price($%s)'
         % (n, n) for n in ("floor", "box", "in2", "in3", "gem")] + [
        # the hero out of the shop: nothing is quoted on its floor
        "set u.ushops[0] = 0",
        "set feed_signalled = 1",
        "call (void) feed_boundary()",
        # back in, with the shopkeeper angry but not yet riled
        "set u.ushops[0] = $rno",
        "set $shk->mpeaceful = 0",
        "set feed_signalled = 1",
        "call (void) feed_boundary()",
        'printf "angry surcharge=%d\\n", $eshk->surcharge',
        'printf "riled floor=%ld box=%ld in2=%ld gem=%ld bag=%ld '
        'in1=%ld\\n", get_cost_of_shop_item($floor, $nc), '
        "get_cost_of_shop_item($box, $nc), get_cost_of_shop_item($in2, $nc), "
        "get_cost_of_shop_item($gem, $nc), unpaid_cost($bag, 1), "
        "unpaid_cost($in1, 0)",
        # the shopkeeper away, with bill_p as u_entered_shop() leaves it
        "set $eshk->bill_p = (struct bill_x *) -1000",
        "set feed_signalled = 1",
        "call (void) feed_boundary()",
        "set $eshk->bill_p = &$eshk->bill[0]",
    ], seconds=30)
    assert not result.stderr, result.stderr
    out = result.stdout
    state, after = gdb_values(out, "state"), gdb_values(out, "after")
    assert state["ok"] >= 0, "no shop door for the shopkeeper"
    assert state["fx"], "no shop square for the goods: %s" % state
    assert state["held"] == 99 and state["billct"] == 3, state
    assert state["unseen"] == 1, state
    assert state == after, "the feed changed the game: %s, %s" % (
        state, after)
    names = ("inv", "bag", "in1", "gem", "floor", "box", "in2", "in3")
    shown = {k: gdb_values(out, k) for k in names}
    ids = {n: shown[n]["id"] for n in names}
    look = {}
    for line in out.splitlines():
        if line.startswith("look "):
            _, oid, text = line.split(" ", 2)
            m = re.search(r"\(for sale, (\d+) zorkmids?\)", text)
            assert m, line
            look[int(oid)] = int(m.group(1))
    assert len(look) == 5, out
    d = x.wait_data(lambda d: sum(v["k"] == "kf" for v in d[before:]) >= 5)
    kfs = [i for i, v in enumerate(d) if i >= before and v["k"] == "kf"]
    assert len(kfs) == 5, "expected five keyframes, got %d" % len(kfs)
    inside = feed_objects(d[kfs[0] + 1:kfs[1] + 1])

    def got(name, objs=inside):
        o = objs[ids[name]]
        return o.get("price", 0), o.get("contents_price", 0)

    for name in ("inv", "bag", "in1"):
        assert inside[ids[name]].get("unpaid") == 1, name
    assert got("inv") == (shown["inv"]["own"], 0), (got("inv"), shown)
    assert shown["inv"]["own"] == shown["inv"]["all"] > 0, shown
    own, contents = got("bag")
    assert own == shown["bag"]["own"] > 0, (got("bag"), shown)
    assert own + contents == shown["bag"]["all"], (got("bag"), shown)
    assert got("in1") == (shown["in1"]["own"], 0) and contents > 0, shown
    for name in ("floor", "in2", "in3", "gem"):
        assert got(name) == (look[ids[name]], 0), (name, got(name), look)
    own, contents = got("box")
    assert own > 0 and own + contents == look[ids["box"]], (own, look)
    assert contents == got("in2")[0] + got("in3")[0], (got("box"), look)
    # priced as a gem, unseen; as glass once seen, as the feed has it
    assert shown["gem"]["unseen"] > 10 * got("gem")[0], (shown, got("gem"))
    left = feed_objects(d[kfs[2]:kfs[2] + 1])
    for name in names:
        assert got(name)[0] and "price" not in left[ids[name]], name
        assert "contents_price" not in left[ids[name]], name
    assert gdb_values(out, "angry")["surcharge"] == 0, out
    riled = gdb_values(out, "riled")
    angry = feed_objects(d[kfs[3]:kfs[3] + 1])
    for name in ("floor", "in2", "gem", "in1"):
        assert got(name, angry)[0] == riled[name] > got(name)[0], (
            name, got(name, angry), riled)
    for name in ("box", "bag"):
        assert sum(got(name, angry)) == riled[name] > sum(got(name)), (
            name, got(name, angry), riled)
    away = feed_objects(d[kfs[4]:kfs[4] + 1])
    for name in ("inv", "bag", "in1"):
        assert got(name, away) == got(name, angry), (
            name, got(name, away), got(name, angry))


def price_quotes(x):
    """Keep remembered quotes in feed names, but not on unpaid items."""
    before = len(x.data()[0])
    result = drive_debug(x, [
        "set $quote = (struct obj *) mksobj(POT_HEALING, 0, 0)",
        "set $quote = (struct obj *) addinv($quote)",
        "set $quote->dknown = 1",
        "set iflags.pricequotes = 1",
        "set objects[POT_HEALING].oc_name_known = 0",
        "set objects[POT_HEALING].oc_buy_minseen = 60",
        "set objects[POT_HEALING].oc_buy_maxseen = 60",
        "set objects[POT_HEALING].oc_sell_minseen = 30",
        "set objects[POT_HEALING].oc_sell_maxseen = 30",
        'printf "quote_id=%u\\n", $quote->o_id',
        "set feed_signalled = 1",
        "call (void) feed_boundary()",
        "set $quote->unpaid = 1",
        "set feed_signalled = 1",
        "call (void) feed_boundary()",
        "set $quote->unpaid = 0",
    ])
    assert not result.stderr, result.stderr
    oid = int(result.stdout.split("quote_id=")[1].split()[0])
    d = x.wait_data(lambda d: sum(e["k"] == "kf" for e in d[before:]) >= 2)
    items = [o for e in d[before:] if e["k"] == "kf"
             for o in e["inv"]["items"] if o["id"] == oid]
    assert len(items) == 2, "missing quote snapshots"
    assert "{buy 60 sell 30}" in items[0]["name"], "remembered quote lost"
    assert items[1].get("unpaid") == 1
    assert "{buy" not in items[1]["name"], "unpaid item gained a quote"
    assert "zorkmid" not in items[1]["name"], "unpaid price lookup reached"


def idle_fault(x):
    result = x.g.gdb(
        [
            'call (void) fb_begin("outer")',
            'call (void) fb_str("before", "one")',
            "set feed_signalled = 1",
            "set feed.waiting = 1",
            "call (void) feed_idle()",
            'call (void) fb_str("after", "two")',
            "call (void) fb_end()",
            "call (void) feed_flush()",
        ]
    )
    assert not result.stderr, result.stderr
    d = x.wait_data(lambda d: any(v["k"] == "outer" for v in d))
    assert any(
        v.get("k") == "outer"
        and v.get("before") == "one"
        and v.get("after") == "two"
        for v in d
    )


def dump_rng(x):
    """No configured dumplog: the feed must not cause one to be generated."""
    result = x.g.gdb([
        "set $dump = sysopt.dumplogfile",
        "set sysopt.dumplogfile = 0",
        "set $gold = (struct obj *) mksobj(GOLD_PIECE, 0, 0)",
        "set $gold->quan = 100",
        "call (struct obj *) addinv($gold)",
        "set $hallu = u.uprops[HALLUC].intrinsic",
        "set u.uprops[HALLUC].intrinsic = 100",
        "set $over = program_state.gameover",
        "set program_state.gameover = 1",
        "set $before = nh_rng_draws[0]",
        "call (void) dump_open_log(0)",
        "call (void) dump_everything(0, 0)",
        'printf "dump_draws=%lu\\n", nh_rng_draws[0] - $before',
        "call (void) dump_close_log()",
        "set program_state.gameover = $over",
        "set u.uprops[HALLUC].intrinsic = $hallu",
        "set sysopt.dumplogfile = $dump",
    ])
    assert not result.stderr, result.stderr
    assert "dump_draws=0\n" in result.stdout, result.stdout


def interrupt_output(pipe_size=4096):
    """An unrecorded game's quit prompt must not resend a partial line."""
    work = tempfile.mkdtemp(prefix="feedfaults-interrupt-")
    pg = os.path.join(work, "pg")
    nhgame.copy_playground(pg)
    g = nhgame.Game(pg, "feedfaults", "", mode="wizard", extra_env={
        "NETHACKOPTIONS": "role:Valkyrie,race:human,gender:female,"
        "align:lawful,!legacy,!news,!splash_screen,!tutorial,!autopickup",
    })
    raw = bytearray()
    padding_left = 0
    passed = False

    def drain(seconds):
        nonlocal padding_left
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            g.drain(0)
            if select.select([g.feed_fd], [], [], 0.02)[0]:
                data = os.read(g.feed_fd, 65536)
                if not data:
                    break
                if padding_left:
                    n = min(padding_left, len(data))
                    assert data[:n] == b"\n" * n, "interleaved pipe padding"
                    data = data[n:]
                    padding_left -= n
                raw.extend(data)

    def events():
        return [json.loads(line) for line in raw.split(b"\n")[:-1]]

    def until(done, what):
        end = time.monotonic() + 10
        while g.alive and time.monotonic() < end and not done():
            drain(0.02)
        assert done(), "timed out waiting for " + what

    try:
        end = time.monotonic() + 20
        while (g.alive and time.monotonic() < end
               and not nhgame.asked_for_command(raw)):
            if "--More--" in g.tail:
                g.tail = ""
                g.send(" ")
            drain(0.1)
        assert nhgame.asked_for_command(raw), "no first command"
        action = max(e["a"] for e in events() if e["k"] == "hero")
        g.tail = ""
        # Search can find a monster and leave --More-- pending.  Escape
        # reaches another command boundary without taking a turn; seeing
        # it also means the startup keyframe has been read in full.
        g.send("\033", settle=0.0)
        until(lambda: any(e["k"] == "hero" and e["a"] > action
                          for e in events()), "the next command")
        action = max(e["a"] for e in events() if e["k"] == "hero")
        before = raw.rfind(b"\n") + 1
        size = fcntl.fcntl(g.feed_fd, fcntl.F_SETPIPE_SZ, pipe_size)
        # Linux rounds capacity up to at least a page (possibly 64 KiB).
        # Occupy the excess without changing the game or its keyframe.
        # The reader verifies and removes only this known padding.
        queued = struct.unpack("i", fcntl.ioctl(
            g.feed_fd, termios.FIONREAD, struct.pack("i", 0)))[0]
        assert queued == 0, "feed was not drained before blocking its writer"
        padding_left = size - min(size, 4096)
        if padding_left:
            fd = os.open("/proc/self/fd/%d" % g.feed_fd,
                         os.O_WRONLY | os.O_NONBLOCK)
            try:
                assert os.write(fd, b"\n" * padding_left) == padding_left, \
                    "could not fill excess pipe capacity"
            finally:
                os.close(fd)
        os.kill(g.pid, signal.SIGUSR1)
        # Leave the keyframe blocked in write() beyond the free capacity.
        end = time.monotonic() + 10
        while True:
            queued = struct.unpack("i", fcntl.ioctl(
                g.feed_fd, termios.FIONREAD, struct.pack("i", 0)))[0]
            if queued >= size or time.monotonic() >= end:
                break
            g.drain(0.02)
        assert queued >= size, "keyframe did not fill the pipe"
        os.kill(g.pid, signal.SIGINT)
        until(lambda: "Really quit" in g.tail, "the interrupt prompt")
        g.send("n\033", settle=0.0)
        until(lambda: any(e["k"] == "hero" and e["a"] > action
                          for e in events()), "a command after the interrupt")
        later = [json.loads(line)
                 for line in raw[before:].split(b"\n")[:-1]]
        assert sum(e["k"] == "kf" for e in later) == 1, \
            "interrupt duplicated or lost the blocked keyframe"
        assert g.alive, "game exited after the interrupt was declined"
        passed = True
        print("interrupt during output PASS", flush=True)
    finally:
        if g.alive:
            os.kill(g.pid, signal.SIGKILL)
        g.drain(0.2)
        g.close()
        os.close(g.feed_fd)
        if passed:
            shutil.rmtree(work)
        else:
            with open(os.path.join(work, "feed.jsonl"), "wb") as f:
                f.write(raw)
            with open(os.path.join(work, "terminal.txt"), "w") as f:
                f.write(g.tail)
            print("scratch files kept in", work, flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("playground")
    args = parser.parse_args()
    if not sys.platform.startswith("linux") or not shutil.which("gdb"):
        parser.error("these fixtures require Linux and gdb")
    nhgame.PLAYGROUND = os.path.abspath(args.playground)
    for name, test in [
        ("naming error prompt", naming_fault),
        ("shop bill unchanged", shop_bill),
        ("shop prices", shop_prices),
        ("remembered price quotes", price_quotes),
        ("idle during a line", idle_fault),
        ("double nesting", nested),
        ("accessibility characters", overrides),
        ("level transition signal", transition),
        ("feed-only dump RNG", dump_rng),
    ]:
        check(name, test)
    interrupt_output()


if __name__ == "__main__":
    main()
