/* NetHack 5.0	wizcmds.c	$NHDT-Date: 1781973074 2026/06/20 16:31:14 $  $NHDT-Branch: NetHack-5.0 $:$NHDT-Revision: 1.36 $ */
/*-Copyright (c) Robert Patrick Rankin, 2024. */
/* NetHack may be freely redistributed.  See license for details. */

#include "hack.h"
#include "func_tab.h"

extern const char unavailcmd[];                  /* cmd.c [27] */
extern const char *levltyp[MAX_TYPE + 2];          /* cmd.c */

staticfn int size_monst(struct monst *, boolean);
staticfn int size_obj(struct obj *);
staticfn void count_obj(struct obj *, long *, long *, boolean, boolean);
staticfn void obj_chain(winid, const char *, struct obj *, boolean, long *,
                      long *);
staticfn void mon_invent_chain(winid, const char *, struct monst *, long *,
                             long *);
staticfn void mon_chain(winid, const char *, struct monst *, boolean, long *,
                      long *);
staticfn void contained_stats(winid, const char *, long *, long *);
staticfn void misc_stats(winid, long *, long *);
staticfn void you_sanity_check(void);
staticfn void levl_sanity_check(void);
staticfn void makemap_unmakemon(struct monst *, boolean);
staticfn int QSORTCALLBACK migrsort_cmp(const genericptr, const genericptr);
staticfn void list_migrating_mons(d_level *);

DISABLE_WARNING_FORMAT_NONLITERAL

/* #wizwish command - wish for something */
int
wiz_wish(void) /* Unlimited wishes for debug mode by Paul Polderman */
{
    if (wizard) {
        boolean save_verbose = flags.verbose;

        flags.verbose = FALSE;
        makewish();
        flags.verbose = save_verbose;
        encumber_msg();
    } else
        pline(unavailcmd, ecname_from_fn(wiz_wish));
    return ECMD_OK;
}

DISABLE_WARNING_FORMAT_NONLITERAL

/* #wizidentify command - reveal and optionally identify hero's inventory */
int
wiz_identify(void)
{
    if (wizard) {
        iflags.override_ID = (int) cmd_from_func(wiz_identify);
        /* command remapping might leave #wizidentify as the only way
           to invoke us, in which case cmd_from_func() will yield NUL;
           it won't matter to display_inventory()/display_pickinv()
           if ^I invokes some other command--what matters is that
           display_pickinv() and xname() see override_ID as nonzero */
        if (!iflags.override_ID)
            iflags.override_ID = C('I');
        (void) display_inventory((char *) 0, FALSE);
        iflags.override_ID = 0;
    } else
        pline(unavailcmd, ecname_from_fn(wiz_identify));
    return ECMD_OK;
}

RESTORE_WARNING_FORMAT_NONLITERAL

/* used when wiz_makemap() gets rid of monsters for the old incarnation of
   a level before creating a new incarnation of it */
staticfn void
makemap_unmakemon(struct monst *mtmp, boolean migratory)
{
    int ndx = monsndx(mtmp->data);

    /* uncreate any unique monster so that it is eligible to be remade
       on the new incarnation of the level; ignores DEADMONSTER() [why?] */
    if (mtmp->data->geno & G_UNIQ)
        svm.mvitals[ndx].mvflags &= ~G_EXTINCT;
    if (svm.mvitals[ndx].born)
        svm.mvitals[ndx].born--;

    /* vault is going away; get rid of guard who might be in play or
       be parked at <0,0>; for the latter, might already be flagged as
       dead but is being kept around because of the 'isgd' flag */
    if (mtmp->isgd) {
        mtmp->isgd = 0; /* after this, fall through to mongone() */
    } else if (DEADMONSTER(mtmp)) {
        return; /* already set to be discarded */
    } else if (mtmp->isshk && on_level(&u.uz, &ESHK(mtmp)->shoplevel)) {
        setpaid(mtmp);
    }
    if (migratory) {
        /* caller has removed 'mtmp' from migrating_mons; put it onto fmon
           so that dmonsfree() bookkeeping for number of dead or removed
           monsters won't get out of sync; it is not on the map but
           mongone() -> m_detach() -> mon_leaving_level() copes with that */
        mtmp->mstate |= MON_OFFMAP;
        mtmp->mstate &= ~(MON_MIGRATING | MON_LIMBO | MON_ENDGAME_MIGR
                          | TERRAIN_FALLOUT_MASK);
        /* FIXME: will post-5.0.0 MON_PARKED need to be dealt with here? */
        mtmp->nmon = fmon;
        fmon = mtmp;
    }
    mongone(mtmp);
}

/* get rid of the all the monsters on--or intimately involved with--current
   level; used when #wizmakemap destroys the level before replacing it */
void
makemap_remove_mons(void)
{
    struct monst *mtmp, **mprev;

    /* keep steed and other adjacent pets after releasing them
       from traps, stopping eating, &c as if hero were ascending */
    keepdogs(TRUE); /* (pets-only; normally we'd be using 'FALSE') */
    /* get rid of all the monsters that didn't make it to 'mydogs' */
    for (mtmp = fmon; mtmp; mtmp = mtmp->nmon) {
        /* if already dead, dmonsfree(below) will get rid of it */
        if (DEADMONSTER(mtmp))
            continue;
        makemap_unmakemon(mtmp, FALSE);
    }
    /* some monsters retain details of this level in mon->mextra; that
       data becomes invalid when the level is replaced by a new one;
       get rid of them now if migrating or already arrived elsewhere;
       [when on their 'home' level, the previous loop got rid of them;
       if they aren't actually migrating but have been placed on some
       'away' level, such monsters are treated like the Wizard:  kept
       on migrating monsters list, scheduled to migrate back to their
       present location instead of being saved with whatever level they
       happen to be on; see keepdogs() and keep_mon_accessible(dog.c)] */
    for (mprev = &gm.migrating_mons; (mtmp = *mprev) != 0; ) {
        if (mtmp->mextra
            && ((mtmp->isshk && on_level(&u.uz, &ESHK(mtmp)->shoplevel))
                || (mtmp->ispriest && on_level(&u.uz, &EPRI(mtmp)->shrlevel))
                || (mtmp->isgd && on_level(&u.uz, &EGD(mtmp)->gdlevel)))) {
            *mprev = mtmp->nmon;
            makemap_unmakemon(mtmp, TRUE);
        } else {
            mprev = &mtmp->nmon;
        }
    }
    /* release dead and 'unmade' monsters */
    dmonsfree();
    if (fmon) {
        impossible("makemap_remove_mons: 'fmon' did not get emptied?");
    }
    return;
}

DISABLE_WARNING_FORMAT_NONLITERAL

/* #levelhash - show fingerprints of the current level as generated and
   as it is now; everyone playing the same seeded game should see the same
   "generated" values for the same level; available in seeded games so
   that racers can check they have the same level, but only wizard mode
   shows the current values since those reflect unseen changes */
int
do_levelhash(void)
{
    static const char *const partnames[NUM_LEVELHASH] = {
        "terrain", "traps", "objects", "monsters"
    };
    uint64 parts[NUM_LEVELHASH];
    char buf[BUFSZ];
    int i, ledger = ledger_no(&u.uz);

    if (!wizard && !nh_seeded()) {
        pline(unavailcmd, ecname_from_fn(do_levelhash));
        return ECMD_OK;
    }
    /* (the ledger number is only shown in wizard mode: on a branch level
       it gives away how long the main dungeon is) */
    if (nh_seeded() && !wizard)
        pline("Seed %s (generator version %d), level %d.",
              nh_seed_display(TRUE), nh_game_seedver(), depth(&u.uz));
    else if (nh_seeded())
        pline("Seed %s (generator version %d), level %d (ledger %d).",
              nh_seed_display(TRUE), nh_game_seedver(), depth(&u.uz), ledger);
    else
        pline("Not a seeded game; level %d (ledger %d).", depth(&u.uz),
              ledger);
    if (level_fingerprint_at_creation(ledger, parts)) {
        Strcpy(buf, "Generated:");
        for (i = 0; i < NUM_LEVELHASH; i++)
            Sprintf(eos(buf), " %s=%08lx", partnames[i],
                    (unsigned long) (parts[i] & 0xffffffffUL));
        pline("%s", buf);
    } else {
        pline("No fingerprint was taken when this level was made.");
    }
    if (!wizard)
        return ECMD_OK;
    level_fingerprint(parts);
    Strcpy(buf, "Now:");
    for (i = 0; i < NUM_LEVELHASH; i++)
        Sprintf(eos(buf), " %s=%08lx", partnames[i],
                (unsigned long) (parts[i] & 0xffffffffUL));
    pline("%s", buf);
    return ECMD_OK;
}

/*
 * #wizseedfuzz - check that a seeded game's levels don't depend on the
 * hero's history.  Every level (apart from the tutorial and the endgame's
 * placeholder level) is made once as a baseline, then again under each of
 * a series of perturbations: other visit orders, genocides, extinctions,
 * birth counts, artifacts that already exist, aggravate monster, the
 * Amulet, the hero's state, the fruit name, uniques already killed, how
 * far the hero has got, hallucination, the hero's gear, the turn, how many
 * monsters and objects have been made, and the hero's name.  The Astral
 * Plane's player-monsters are made too, as on arrival.  What each level
 * contains is written to a file for test/seedfuzz.py to compare.  The
 * game's global state is put back between passes, and the levels are made
 * as in a normal game even when this is run in wizard mode; the level the
 * hero was on is made again afresh at the end.
 */

enum seedfuzz_pass {
    SF_BASE = 0, SF_ORDER_DOWN, SF_ORDER_SHUFFLE, SF_GENOCIDE, SF_EXTINCT,
    SF_BORN, SF_ARTIFACTS, SF_AGGRAVATE, SF_AMULET, SF_HERO, SF_FRUIT,
    SF_UNIQUES, SF_PROGRESS, SF_HALLU, SF_GEAR, SF_TURN, SF_IDS, SF_NAME,
    NUM_SEEDFUZZ
};

static const char *const seedfuzz_names[NUM_SEEDFUZZ] = {
    "base", "order-down", "order-shuffle", "genocide", "extinct", "born",
    "artifacts", "aggravate", "amulet", "hero", "fruit", "uniques",
    "progress", "hallu", "gear", "turn", "ids", "name"
};

/* turns of each level's wandering monster timeline to write */
#define SEEDFUZZ_SPAWN_FROM 1000L
#define SEEDFUZZ_SPAWN_TURNS 500L

static const int seedfuzz_uniques[] = {
    PM_MEDUSA, PM_ORACLE, PM_CROESUS, PM_JUIBLEX, PM_ORCUS,
    PM_VLAD_THE_IMPALER, PM_WIZARD_OF_YENDOR, PM_CYCLOPS, PM_ASMODEUS,
    PM_BAALZEBUB, PM_YEENOGHU, PM_DISPATER, PM_GERYON
};

struct seedfuzz_state {
    struct mvitals mvitals[NUMMONS];
    struct context_info context;
    struct you you;
    struct q_score quest_status;
    d_level knox_source;
    xint16 ureached[MAXDUNGEON];
    uint64 levelhash[MAXLINFO][NUM_LEVELHASH];
    boolean levelhash_have[MAXLINFO];
    char plname[PL_NSIZ];
    genericptr_t arti;
};

/* ids of the objects made as part of a monster on the level being made
   (they follow the monster: a tougher one, or a missing one, has different
   ones), and of the monsters whose species was picked by the level's
   difficulty (a tougher one can be picked instead) */
struct seedfuzz_ids {
    unsigned *id;
    int n, max;
};
static struct seedfuzz_ids seedfuzz_monobjs, seedfuzz_picked;

staticfn void seedfuzz_save(struct seedfuzz_state *, boolean);
staticfn void seedfuzz_note(struct seedfuzz_ids *, unsigned);
staticfn boolean seedfuzz_noted(struct seedfuzz_ids *, unsigned);
staticfn void seedfuzz_astral(void);
staticfn unsigned long seedfuzz_invhash(struct obj *);
staticfn void seedfuzz_dumpobjs(FILE *, struct obj *, char);
staticfn void seedfuzz_dump(FILE *, int, int);
staticfn int seedfuzz_levels(int *);
staticfn void seedfuzz_perturb(FILE *, int);
staticfn int seedfuzz_randmon(boolean);

/* save (restore == FALSE) or put back the state that making levels, or a
   perturbation, changes */
staticfn void
seedfuzz_save(struct seedfuzz_state *st, boolean restore)
{
    branch *br = dungeon_branch("Fort Ludios");
    int i;

    if (!restore) {
        (void) memcpy((genericptr_t) st->mvitals, (genericptr_t) svm.mvitals,
                      sizeof st->mvitals);
        st->context = svc.context;
        st->you = u;
        st->quest_status = svq.quest_status;
        st->knox_source = br->end1;
        for (i = 0; i < MAXDUNGEON; i++)
            st->ureached[i] = svd.dungeons[i].dunlev_ureached;
        (void) memcpy((genericptr_t) st->levelhash,
                      (genericptr_t) gseed.levelhash, sizeof st->levelhash);
        (void) memcpy((genericptr_t) st->levelhash_have,
                      (genericptr_t) gseed.levelhash_have,
                      sizeof st->levelhash_have);
        Strcpy(st->plname, svp.plname);
        artifact_state(st->arti, FALSE);
    } else {
        (void) memcpy((genericptr_t) svm.mvitals, (genericptr_t) st->mvitals,
                      sizeof st->mvitals);
        svc.context = st->context;
        u = st->you;
        svq.quest_status = st->quest_status;
        br->end1 = st->knox_source;
        for (i = 0; i < MAXDUNGEON; i++)
            svd.dungeons[i].dunlev_ureached = st->ureached[i];
        (void) memcpy((genericptr_t) gseed.levelhash,
                      (genericptr_t) st->levelhash, sizeof st->levelhash);
        (void) memcpy((genericptr_t) gseed.levelhash_have,
                      (genericptr_t) st->levelhash_have,
                      sizeof st->levelhash_have);
        Strcpy(svp.plname, st->plname);
        artifact_state(st->arti, TRUE);
    }
}

staticfn void
seedfuzz_note(struct seedfuzz_ids *ids, unsigned id)
{
    if (!ids->max)
        return; /* not running */
    if (ids->n == ids->max) {
        unsigned *bigger = (unsigned *) alloc(2 * ids->max
                                              * sizeof (unsigned));

        (void) memcpy((genericptr_t) bigger, (genericptr_t) ids->id,
                      ids->max * sizeof (unsigned));
        free((genericptr_t) ids->id);
        ids->id = bigger;
        ids->max *= 2;
    }
    ids->id[ids->n++] = id;
}

staticfn boolean
seedfuzz_noted(struct seedfuzz_ids *ids, unsigned id)
{
    int i;

    for (i = 0; i < ids->n; i++)
        if (ids->id[i] == id)
            return TRUE;
    return FALSE;
}

/* note an object made as part of a monster (see mksobj()) */
void
seedfuzz_note_obj(struct obj *otmp)
{
    seedfuzz_note(&seedfuzz_monobjs, otmp->o_id);
}

/* note a monster whose species was picked by the level's difficulty (see
   makemon()) */
void
seedfuzz_note_mon(struct monst *mtmp)
{
    seedfuzz_note(&seedfuzz_picked, mtmp->m_id);
}

staticfn unsigned long
seedfuzz_invhash(struct obj *list)
{
    unsigned long h = 5381UL;
    struct obj *o;

    for (o = list; o; o = o->nobj)
        h = (h * 33UL) ^ (unsigned long) (o->otyp * 131 + o->quan * 7
                                           + o->spe * 3 + o->oartifact);
    return h & 0xffffffffUL;
}

staticfn void
seedfuzz_dumpobjs(FILE *fp, struct obj *list, char tag)
{
    struct obj *o;

    for (o = list; o; o = o->nobj) {
        char name[BUFSZ], *p;
        /* (corpsenm only means a species for these, or a novel's title;
           for other objects it holds other things, such as a monster's
           id; see level_fingerprint()) */
        boolean species = (o->otyp == CORPSE || o->otyp == STATUE
                           || o->otyp == FIGURINE || o->otyp == EGG
                           || o->otyp == TIN || o->otyp == SPE_NOVEL);

        Strcpy(name, OBJ_NAME(objects[o->otyp]) ? OBJ_NAME(objects[o->otyp])
                                                 : "?");
        for (p = name; *p; p++)
            if (*p == ' ')
                *p = '_';

        /* (m: made as part of a monster) */
        fprintf(fp, "%c %d %d %d %ld %d %d %d %d %d %s %c %d\n", tag,
                o->otyp, o->ox, o->oy, o->quan,
                (o->otyp == SLIME_MOLD) ? 0 : o->spe, o->blessed, o->cursed,
                species ? o->corpsenm : NON_PM, o->oartifact, name,
                seedfuzz_noted(&seedfuzz_monobjs, o->o_id) ? 'm' : '-',
                o->oerodeproof);
        if (o->cobj) /* contents: of a monster's container, or not */
            seedfuzz_dumpobjs(fp, o->cobj, (tag == 'I' || tag == 'J') ? 'J'
                                                                     : 'C');
    }
}

/* write what the level just made contains */
staticfn void
seedfuzz_dump(FILE *fp, int pass, int ledger)
{
    uint64 parts[NUM_LEVELHASH];
    char buf[BUFSZ];
    struct trap *t;
    struct monst *m;

    /* (the fingerprint taken as it was made, as the game shows it; the
       Astral Plane's player-monsters, made later, aren't part of it) */
    if (!level_fingerprint_at_creation(ledger, parts))
        (void) memset((genericptr_t) parts, 0, sizeof parts);
    fprintf(fp, "L %d %d %d draws=%ld terrain=%08lx\n", pass, ledger,
            depth(&u.uz), gseed.layout_draws,
            (unsigned long) (parts[0] & 0xffffffffUL));
    /* the fingerprint as #levelhash shows it, in the dumplog's words */
    level_fingerprint_text(buf, sizeof buf, &u.uz, parts);
    fprintf(fp, "F%s\n", buf);
    for (t = gf.ftrap; t; t = t->ntrap)
        fprintf(fp, "R %d %d %d\n", t->ttyp, t->tx, t->ty);
    seedfuzz_dumpobjs(fp, fobj, 'O');
    seedfuzz_dumpobjs(fp, svl.level.buriedobjlist, 'B');
    for (m = fmon; m; m = m->nmon) {
        char name[BUFSZ], *p;

        if (DEADMONSTER(m))
            continue;
        Strcpy(name, m->data->pmnames[NEUTRAL]);
        for (p = name; *p; p++)
            if (*p == ' ')
                *p = '_';
        /* (p: its species was picked by the level's difficulty) */
        fprintf(fp, "M %d %d %d %d %d %08lx %s %d %d %c %d\n",
                monsndx(m->data), m->mx, m->my, m->m_lev, m->mpeaceful,
                seedfuzz_invhash(m->minvent), name,
                (m->cham != NON_PM) ? m->cham : monsndx(m->data),
                (m->cham != NON_PM) ? 1 : 0,
                seedfuzz_noted(&seedfuzz_picked, m->m_id) ? 'p' : '-',
                m->mhpmax);
        seedfuzz_dumpobjs(fp, m->minvent, 'I');
    }
    /* the level's wandering monster timeline (see seeded_spawn_pick()),
       leaving out being a demigod, which is meant to change it */
    if (svl.level.flags.rndmongen) {
        unsigned demigod = u.uevent.udemigod;
        struct permonst *ptr;
        long turn;

        u.uevent.udemigod = 0;
        for (turn = SEEDFUZZ_SPAWN_FROM;
             turn < SEEDFUZZ_SPAWN_FROM + SEEDFUZZ_SPAWN_TURNS; turn++)
            if ((ptr = seeded_spawn_pick(turn)) != 0)
                fprintf(fp, "W %ld %d\n", turn, monsndx(ptr));
        u.uevent.udemigod = demigod;
    }
}

/* the levels to make, as ledger numbers */
staticfn int
seedfuzz_levels(int *ledgers)
{
    int n = 0, ledger;
    d_level lev;
    s_level *dummy = find_level("dummy"); /* endgame placeholder */

    for (ledger = 1; ledger <= maxledgerno(); ledger++) {
        lev.dnum = ledger_to_dnum((xint16) ledger);
        lev.dlevel = ledger_to_dlev((xint16) ledger);
        if (In_tutorial(&lev)
            || (dummy && on_level(&lev, &dummy->dlevel)))
            continue;
        ledgers[n++] = ledger;
    }
    return n;
}

/* a random species that can be generated (optionally a unique) */
staticfn int
seedfuzz_randmon(boolean ok_uniq)
{
    int mndx, tries = 0;

    do {
        mndx = rn1(SPECIAL_PM - LOW_PM, LOW_PM);
    } while (++tries < 1000
             && ((mons[mndx].geno & G_NOGEN)
                 || (!ok_uniq && (mons[mndx].geno & G_UNIQ))));
    return mndx;
}

/* the Astral Plane's player-monsters, made as on the hero's arrival
   through the portal from the Plane of Water */
staticfn void
seedfuzz_astral(void)
{
    struct trap *t;
    coordxy ux = u.ux, uy = u.uy;
    dest_area *arrive = (svu.updest.lx > 0) ? &svu.updest
                        : (svd.dndest.lx > 0) ? &svd.dndest
                          : (dest_area *) 0;

    /* where the hero stands (no player-monster is placed there): the
       portal from the Plane of Water puts the hero in the level's arrival
       region; its first square, for a fixed answer.  It must be a real
       square: dressing a monster recalculates vision around the hero */
    u.ux = arrive ? arrive->lx : (coordxy) (COLNO / 2);
    u.uy = arrive ? arrive->ly : (coordxy) (ROWNO / 2);
    for (t = gf.ftrap; t; t = t->ntrap)
        if (t->ttyp == MAGIC_PORTAL)
            u.ux = t->tx, u.uy = t->ty;
    astral_mplayers();
    u.ux = ux, u.uy = uy;
}

/* apply a perturbation, noting in the file what was changed */
staticfn void
seedfuzz_perturb(FILE *fp, int pass)
{
    int i, mndx;

    switch (pass) {
    case SF_GENOCIDE:
        for (i = 0; i < 3; i++) {
            mndx = seedfuzz_randmon(FALSE);
            svm.mvitals[mndx].mvflags |= (G_GENOD | G_NOCORPSE);
            fprintf(fp, "G %d\n", mndx);
        }
        break;
    case SF_EXTINCT:
        for (i = 0; i < 5; i++)
            svm.mvitals[seedfuzz_randmon(FALSE)].mvflags |= G_EXTINCT;
        break;
    case SF_BORN:
        for (i = LOW_PM; i < NUMMONS; i++)
            if (!(mons[i].geno & G_UNIQ))
                svm.mvitals[i].born = (uchar) rn2(100);
        break;
    case SF_ARTIFACTS:
        for (i = 0; i < 6; i++)
            artifact_mark_exists(rnd(NROFARTIFACTS));
        break;
    case SF_AGGRAVATE:
        u.uprops[AGGRAVATE_MONSTER].extrinsic |= W_RINGL;
        break;
    case SF_AMULET:
        u.uhave.amulet = 1;
        break;
    case SF_HERO:
        u.ulevel = 14;
        u.uluck = LUCKMAX;
        u.moreluck = LUCKADD;
        u.ualign.record = -50;
        u.ualign.abuse = 20;
        u.ualign.type = (u.ualign.type == A_LAWFUL) ? A_CHAOTIC : A_LAWFUL;
        u.uprops[PROT_FROM_SHAPE_CHANGERS].extrinsic |= W_RINGR;
        u.ux = (coordxy) rn1(COLNO - 3, 2);
        u.uy = (coordxy) rn2(ROWNO);
        u.uevent.udemigod = 1;
        u.uevent.qcompleted = 1;
        break;
    case SF_FRUIT: {
        char fruit[] = "durian";

        svc.context.current_fruit = fruitadd(fruit, (struct fruit *) 0);
        break;
    }
    case SF_UNIQUES:
        for (i = 0; i < SIZE(seedfuzz_uniques); i++) {
            mndx = seedfuzz_uniques[i];
            svm.mvitals[mndx].mvflags |= G_EXTINCT;
            svm.mvitals[mndx].born = 1;
            fprintf(fp, "U %d\n", mndx);
        }
        break;
    case SF_PROGRESS: /* been to the bottom of everything, and invoked */
        for (i = 0; i < svn.n_dgns; i++)
            svd.dungeons[i].dunlev_ureached = svd.dungeons[i].num_dunlevs;
        u.uevent.invoked = 1;
        u.uevent.gehennom_entered = 1;
        break;
    case SF_HALLU:
        set_itimeout(&HHallucination, 1000L);
        break;
    case SF_GEAR: /* affects monsters only */
        u.uprops[SEE_INVIS].intrinsic |= FROMOUTSIDE;
        u.ualignbase[A_CURRENT] = (u.ualignbase[A_CURRENT] == A_LAWFUL)
                                      ? A_CHAOTIC : A_LAWFUL;
        break;
    case SF_IDS: /* as if many monsters and objects had been made */
        svc.context.ident += 1000 + (unsigned) rn2(1000000);
        break;
    case SF_NAME:
        Strcpy(svp.plname, "Fuzzy Wuzzy");
        break;
    default:
        break;
    }
}

/* run the checks, writing to outfile; the hero ends up on a remade copy
   of the level they were on */
void
seedfuzz_run(const char *outfile)
{
    struct seedfuzz_state *st;
    int *ledgers, *order, nlev, pass, i, j, tmp;
    long savemoves = svm.moves, *turns;
    d_level here, inmem;
    boolean was_wizard = wizard;
    FILE *fp;

    if (!nh_seeded()) {
        pline("#wizseedfuzz needs a seeded game.");
        return;
    }
    if (u.ustuck || u.usteed) {
        pline("#wizseedfuzz can't be used while %s.",
              u.usteed ? "riding" : "stuck to a monster");
        return;
    }
    /* (outside debug mode, only make a new file: the game may be running
       with permissions the player doesn't have) */
    if (!(fp = fopen(outfile, wizard ? "w" : "wx"))) {
        pline("#wizseedfuzz can't write \"%s\".", outfile);
        return;
    }
    st = (struct seedfuzz_state *) alloc(sizeof *st);
    st->arti = alloc(artifact_state_size());
    ledgers = (int *) alloc(sizeof (int) * (MAXLINFO + 1));
    order = (int *) alloc(sizeof (int) * (MAXLINFO + 1));
    turns = (long *) alloc(sizeof (long) * (MAXLINFO + 1));
    nlev = seedfuzz_levels(ledgers);
    seedfuzz_monobjs.max = seedfuzz_picked.max = 256;
    seedfuzz_monobjs.id = (unsigned *) alloc(256 * sizeof (unsigned));
    seedfuzz_picked.id = (unsigned *) alloc(256 * sizeof (unsigned));
    assign_level(&here, &u.uz);
    assign_level(&inmem, &u.uz); /* the level in memory */

    fprintf(fp, "S %s %d\n", nh_seed_str(), nh_game_seedver());
    flags.debug = FALSE; /* make the levels as in a normal game */
    seedfuzz_save(st, FALSE);
    for (pass = 0; pass < NUM_SEEDFUZZ; pass++) {
        fprintf(fp, "P %d %s\n", pass, seedfuzz_names[pass]);
        /* choices made by the perturbations and the shuffle don't touch
           the game's own random numbers */
        rng_stream_begin("seedfuzz", (long) pass, 0L);
        for (i = 0; i < nlev; i++)
            order[i] = (pass == SF_ORDER_DOWN) ? ledgers[nlev - 1 - i]
                                               : ledgers[i];
        if (pass == SF_ORDER_SHUFFLE)
            for (i = nlev - 1; i > 0; i--) {
                j = rn2(i + 1);
                tmp = order[i], order[i] = order[j], order[j] = tmp;
            }
        /* as in a game, only the first level is made on the first turn
           (the turn pass makes every level on a later one) */
        for (i = 0; i < nlev; i++)
            turns[i] = (pass == SF_TURN) ? 3L + (long) rn2(100000)
                       : (ledger_to_dnum((xint16) order[i]) == 0
                          && ledger_to_dlev((xint16) order[i]) == 1) ? 1L
                       : 2L;
        seedfuzz_perturb(fp, pass);
        rng_stream_end();

        for (i = 0; i < nlev; i++) {
            assign_level(&u.uz, &inmem);
            makemap_prepost(TRUE, FALSE); /* discard the level in memory */
            u.uz.dnum = ledger_to_dnum((xint16) order[i]);
            u.uz.dlevel = ledger_to_dlev((xint16) order[i]);
            assign_level(&inmem, &u.uz);
            seedfuzz_monobjs.n = seedfuzz_picked.n = 0;
            svm.moves = turns[i];
            /* each dungeon's themed-room Lua state lives for the whole
               run (in a game, for the dungeon's visits); its memory limit
               counts garbage not yet collected, and running out inside a
               finalizer leaves "[lua] error in __gc" in the paniclog */
            for (j = 0; j < svn.n_dgns; j++)
                if (gl.luathemes[j])
                    lua_gc((lua_State *) gl.luathemes[j], LUA_GCCOLLECT);
            mklev();
            if (Is_astralevel(&u.uz))
                seedfuzz_astral();
            seedfuzz_dump(fp, pass, order[i]);
        }
        seedfuzz_save(st, TRUE); /* also puts back u.uz */
    }
    svm.moves = savemoves;
    flags.debug = was_wizard;
    free((genericptr_t) seedfuzz_monobjs.id);
    free((genericptr_t) seedfuzz_picked.id);
    (void) memset((genericptr_t) &seedfuzz_monobjs, 0,
                  sizeof seedfuzz_monobjs);
    (void) memset((genericptr_t) &seedfuzz_picked, 0,
                  sizeof seedfuzz_picked);
    fprintf(fp, "E\n");
    (void) fclose(fp);

    /* put the hero back on (a remade copy of) the level they were on; the
       game's state, including the level fingerprints, is as it was */
    assign_level(&u.uz, &inmem);
    makemap_prepost(TRUE, FALSE);
    assign_level(&u.uz, &here);
    mklev();
    seedfuzz_save(st, TRUE); /* the remade level's fingerprint too */
    svc.context.polearm.hitmon = (struct monst *) 0;
    makemap_prepost(FALSE, FALSE);
    free((genericptr_t) turns);
    free((genericptr_t) order);
    free((genericptr_t) ledgers);
    free(st->arti);
    free((genericptr_t) st);
    pline("#wizseedfuzz: %d levels, %d passes, written to \"%s\".", nlev,
          NUM_SEEDFUZZ, outfile);
}

int
wiz_seedfuzz(void)
{
    if (!wizard) {
        pline(unavailcmd, ecname_from_fn(wiz_seedfuzz));
        return ECMD_OK;
    }
    seedfuzz_run("seedfuzz.txt");
    return ECMD_OK;
}

/* #wizmakemap - discard current dungeon level and replace with a new one */
int
wiz_makemap(void)
{
    if (wizard) {
        boolean was_in_W_tower = In_W_tower(u.ux, u.uy, &u.uz);

        makemap_prepost(TRUE, was_in_W_tower);
        /* create a new level; various things like bestowing a guardian
           angel on Astral or setting off alarm on Ft.Ludios are handled
           by goto_level(do.c) so won't occur for replacement levels */
        mklev();
        makemap_prepost(FALSE, was_in_W_tower);
    } else {
        pline(unavailcmd, ecname_from_fn(wiz_makemap));
    }
    return ECMD_OK;
}

/* the #wizmap command - reveal the level map
   and any traps or engravings on it */
int
wiz_map(void)
{
    if (wizard) {
        struct trap *t;
        struct engr *ep;
        long save_Hconf = HConfusion, save_Hhallu = HHallucination;

        notice_mon_off();
        HConfusion = HHallucination = 0L;
        for (t = gf.ftrap; t != 0; t = t->ntrap) {
            t->tseen = 1;
            map_trap(t, TRUE);
        }
        for (ep = head_engr; ep != 0; ep = ep->nxt_engr) {
            map_engraving(ep, TRUE);
        }
        do_mapping();
        notice_mon_on();
        HConfusion = save_Hconf;
        HHallucination = save_Hhallu;
    } else
        pline(unavailcmd, ecname_from_fn(wiz_map));
    return ECMD_OK;
}

/* #wizgenesis - generate monster(s); a count prefix will be honored */
int
wiz_genesis(void)
{
    if (wizard) {
        boolean mongen_saved = iflags.debug_mongen;

        iflags.debug_mongen = FALSE;
        (void) create_particular();
        iflags.debug_mongen = mongen_saved;
    } else
        pline(unavailcmd, ecname_from_fn(wiz_genesis));
    return ECMD_OK;
}

/* #wizwhere command - display dungeon layout */
int
wiz_where(void)
{
    if (wizard)
        (void) print_dungeon(FALSE, (schar *) 0, (xint16 *) 0);
    else
        pline(unavailcmd, ecname_from_fn(wiz_where));
    return ECMD_OK;
}

/* the #wizdetect command - detect secret doors, traps, hidden monsters */
int
wiz_detect(void)
{
    if (wizard)
        (void) findit();
    else
        pline(unavailcmd, ecname_from_fn(wiz_detect));
    return ECMD_OK;
}

RESTORE_WARNING_FORMAT_NONLITERAL

/* the #wizkill command - pick targets and reduce them to 0HP;
   by default, the hero is credited/blamed; use 'm' prefix to avoid that */
int
wiz_kill(void)
{
    struct monst *mtmp;
    coord cc;
    int ans;
    char c, qbuf[QBUFSZ];
    const char *prompt = "Pick first monster to slay";
    boolean save_verbose = flags.verbose,
            save_autodescribe = iflags.autodescribe;
    d_level uarehere = u.uz;

    cc.x = u.ux, cc.y = u.uy;
    for (;;) {
        pline("%s:", prompt);
        prompt = "Next monster";

        flags.verbose = FALSE;
        iflags.autodescribe = TRUE;
        ans = getpos(&cc, TRUE, "a monster");
        flags.verbose = save_verbose;
        iflags.autodescribe = save_autodescribe;
        if (ans < 0 || cc.x < 1)
            break;

        mtmp = 0;
        if (u_at(cc.x, cc.y)) {
            if (u.usteed) {
                Sprintf(qbuf, "Kill %.110s?", mon_nam(u.usteed));
                if ((c = ynq(qbuf)) == 'q')
                    break;
                if (c == 'y')
                    mtmp = u.usteed;
            }
            if (!mtmp) {
                Sprintf(qbuf, "%s?", Role_if(PM_SAMURAI) ? "Perform seppuku"
                                                         : "Commit suicide");
                if (paranoid_query(TRUE, qbuf)) {
                    Sprintf(svk.killer.name, "%s own player", uhis());
                    svk.killer.format = KILLED_BY;
                    done(DIED);
                }
                break;
            }
        } else if (u.uswallow) {
            mtmp = next2u(cc.x, cc.y) ? u.ustuck : 0;
        } else {
            mtmp = m_at(cc.x, cc.y);
        }

        /* whether there's an unseen monster here or not, player will know
           that there's no monster here after the kill or failed attempt;
           let hero know too */
        (void) unmap_invisible(cc.x, cc.y);

        if (mtmp) {
            /* we don't require that the monster be seen or sensed so
               we issue our own message in order to name it in case it
               isn't; note that if it triggers other kills, those might
               be referred to as "it" */
            int tame = !!mtmp->mtame,
                seen = (canspotmon(mtmp) || (u.uswallow && mtmp == u.ustuck)),
                flgs = (SUPPRESS_IT | SUPPRESS_HALLUCINATION
                        | ((tame && has_mgivenname(mtmp)) ? SUPPRESS_SADDLE
                           : 0)),
                articl = tame ? ARTICLE_YOUR : seen ? ARTICLE_THE : ARTICLE_A;
            const char *adjs = tame ? (!seen ? "poor, unseen" : "poor")
                                    : (!seen ? "unseen" : (const char *) 0);
            char *Mn = x_monnam(mtmp, articl, adjs, flgs, FALSE);

            if (!iflags.menu_requested) {
                /* normal case: hero is credited/blamed */
                You("%s %s!", nonliving(mtmp->data) ? "destroy" : "kill", Mn);
                xkilled(mtmp, XKILL_NOMSG);
            } else { /* 'm'-prefix */
                /* we know that monsters aren't moving because player has
                   just issued this #wizkill command, but if 'mtmp' is a
                   gas spore whose explosion kills any other monsters we
                   need to have the mon_moving flag be True in order to
                   avoid blaming or crediting hero for their deaths */
                svc.context.mon_moving = TRUE;
                pline("%s is %s.", upstart(Mn),
                      nonliving(mtmp->data) ? "destroyed" : "killed");
                /* Null second arg suppresses the usual message */
                monkilled(mtmp, (char *) 0, AD_PHYS);
                svc.context.mon_moving = FALSE;
            }
            /* end targetting loop if an engulfer dropped hero onto a level-
               changing trap */
            if (u.utotype || !on_level(&u.uz, &uarehere))
                break;
        } else {
            There("is no monster there.");
            break;
        }
    }
    /* since #wizkill takes no game time, it is possible to kill something
       in the main dungeon and immediately level teleport into the endgame
       which will delete the main dungeon's level files; avoid triggering
       impossible "dmonsfree: 0 removed doesn't match N pending" by forcing
       dead monster cleanup; we don't track whether anything was actually
       killed above--if nothing was, this will be benign */
    dmonsfree();
    /* distinction between ECMD_CANCEL and ECMD_OK is unimportant here */
    return ECMD_OK; /* no time elapses */
}

DISABLE_WARNING_FORMAT_NONLITERAL

/* the #wizloadlua command - load an arbitrary lua file */
int
wiz_load_lua(void)
{
    if (wizard) {
        char buf[BUFSZ];
            /* Large but not unlimited memory and CPU so random bits of
             * code can be tested by wizards. */
        nhl_sandbox_info sbi = {NHL_SB_SAFE | NHL_SB_DEBUGGING,
                16*1024*1024, 0, 16*1024*1024};

        buf[0] = '\0';
        getlin("Load which lua file?", buf);
        if (buf[0] == '\033' || buf[0] == '\0')
            return ECMD_CANCEL;
        if (!strchr(buf, '.'))
            strcat(buf, ".lua");
        (void) load_lua(buf, &sbi);
    } else
        pline(unavailcmd, ecname_from_fn(wiz_load_lua));
    return ECMD_OK;
}

/* the #wizloaddes command - load a special level lua file */
int
wiz_load_splua(void)
{
    if (wizard) {
        char buf[BUFSZ];

        buf[0] = '\0';
        getlin("Load which des lua file?", buf);
        if (buf[0] == '\033' || buf[0] == '\0')
            return ECMD_CANCEL;
        if (!strchr(buf, '.'))
            strcat(buf, ".lua");

        lspo_reset_level(NULL);
        (void) load_special(buf);
        lspo_finalize_level(NULL);

    } else
        pline(unavailcmd, ecname_from_fn(wiz_load_splua));
    return ECMD_OK;
}

/* the #wizlevelport command - level teleport */
int
wiz_level_tele(void)
{
    if (wizard)
        level_tele();
    else
        pline(unavailcmd, ecname_from_fn(wiz_level_tele));
    return ECMD_OK;
}

RESTORE_WARNING_FORMAT_NONLITERAL

/* #wizfliplevel - transpose the current level */
int
wiz_flip_level(void)
{
    static const char choices[] = "0123",
        prmpt[] = "Flip 0=randomly, 1=vertically, 2=horizontally, 3=both:";

    /*
     * Does not handle
     *   levregions,
     *   monster mtrack,
     *   migrating monsters aimed at returning to specific coordinates
     *     on this level
     * as flipping is normally done only during level creation.
     */
    if (wizard) {
        char c = yn_function(prmpt, choices, '\0', TRUE);

        if (c && strchr(choices, c)) {
            c -= '0';

            if (!c)
                flip_level_rnd(3, TRUE);
            else
                flip_level((int) c, TRUE);

            docrt();
        } else {
            pline("%s", Never_mind);
        }
    }
    return ECMD_OK;
}

/* #levelchange command - adjust hero's experience level */
int
wiz_level_change(void)
{
    char buf[BUFSZ], dummy = '\0';
    int newlevel = 0;
    int ret;

    buf[0] = '\0'; /* in case EDIT_GETLIN is enabled */
    getlin("To what experience level do you want to be set?", buf);
    (void) mungspaces(buf);
    if (buf[0] == '\033' || buf[0] == '\0')
        ret = 0;
    else
        ret = sscanf(buf, "%d%c", &newlevel, &dummy);

    if (ret != 1) {
        pline1(Never_mind);
        return ECMD_OK;
    }
    if (newlevel == u.ulevel) {
        You("are already that experienced.");
    } else if (newlevel < u.ulevel) {
        if (u.ulevel == 1) {
            You("are already as inexperienced as you can get.");
            return ECMD_OK;
        }
        if (newlevel < 1)
            newlevel = 1;
        while (u.ulevel > newlevel)
            losexp("#levelchange");
    } else {
        if (u.ulevel >= MAXULEV) {
            You("are already as experienced as you can get.");
            return ECMD_OK;
        }
        if (newlevel > MAXULEV)
            newlevel = MAXULEV;
        while (u.ulevel < newlevel)
            pluslvl(FALSE);
    }
    /* blessed full healing or restore ability won't fix any lost levels */
    u.ulevelmax = u.ulevel;
    return ECMD_OK;
}

DISABLE_WARNING_CONDEXPR_IS_CONSTANT

/* #wiztelekinesis */
int
wiz_telekinesis(void)
{
    int ans = 0;
    coord cc;
    struct monst *mtmp = (struct monst *) 0;

    cc.x = u.ux;
    cc.y = u.uy;

    pline("Pick a monster to hurtle.");
    do {
        ans = getpos(&cc, TRUE, "a monster");
        if (ans < 0 || cc.x < 1)
            return ECMD_CANCEL;

        if ((((mtmp = m_at(cc.x, cc.y)) != 0) && canspotmon(mtmp))
            || u_at(cc.x, cc.y)) {
            if (!getdir("which direction?"))
                return ECMD_CANCEL;

            if (mtmp) {
                mhurtle(mtmp, u.dx, u.dy, 6);
                if (!DEADMONSTER(mtmp) && canspotmon(mtmp)) {
                    cc.x = mtmp->mx;
                    cc.y = mtmp->my;
                }
            } else {
                hurtle(u.dx, u.dy, 6, FALSE);
                cc.x = u.ux, cc.y = u.uy;
            }
        }

    } while (u.utotype == UTOTYPE_NONE);
    return ECMD_OK;
}

RESTORE_WARNING_CONDEXPR_IS_CONSTANT

/* #panic command - test program's panic handling */
int
wiz_panic(void)
{
    if (iflags.debug_fuzzer) {
        u.uhp = u.uhpmax = 1000;
        u.uen = u.uenmax = 1000;
        return ECMD_OK;
    }
    if (paranoid_query(TRUE,
                       "Do you want to call panic() and end your game?"))
        panic("Crash test (#panic).");
    return ECMD_OK;
}

/* #debugfuzzer command - fuzztest the program */
int
wiz_fuzzer(void)
{
    if (flags.suppress_alert < FEATURE_NOTICE_VER(3,7,0)) {
        pline("The fuzz tester will make NetHack execute random keypresses.");
        There("is no conventional way out of this mode.");
    }
    if (paranoid_query(TRUE, "Do you want to start fuzz testing?")) {
        /* Thoth, take the reins */
        if (y_n("Do you want to call panic() after impossible()?") == 'n') {
            iflags.debug_fuzzer = fuzzer_impossible_continue;
        } else {
            iflags.debug_fuzzer = fuzzer_impossible_panic;
        }
    }
    return ECMD_OK;
}

/* #polyself command - change hero's form */
int
wiz_polyself(void)
{
    polyself(POLY_CONTROLLED);
    return ECMD_OK;
}

/* #seenv command */
int
wiz_show_seenv(void)
{
    winid win;
    coordxy x, y, startx, stopx, curx;
    int v;
    char row[COLNO + 1];

    win = create_nhwindow(NHW_TEXT);
    /*
     * Each seenv description takes up 2 characters, so center
     * the seenv display around the hero.
     */
    startx = max(1, u.ux - (COLNO / 4));
    stopx = min(startx + (COLNO / 2), COLNO);
    /* can't have a line exactly 80 chars long */
    if (stopx - startx == COLNO / 2)
        startx++;

    for (y = 0; y < ROWNO; y++) {
        for (x = startx, curx = 0; x < stopx; x++, curx += 2) {
            if (u_at(x, y)) {
                row[curx] = row[curx + 1] = '@';
            } else {
                v = levl[x][y].seenv & 0xff;
                if (v == 0)
                    row[curx] = row[curx + 1] = ' ';
                else
                    Sprintf(&row[curx], "%02x", v);
            }
        }
        /* remove trailing spaces */
        for (x = curx - 1; x >= 0; x--)
            if (row[x] != ' ')
                break;
        row[x + 1] = '\0';

        putstr(win, 0, row);
    }
    display_nhwindow(win, TRUE);
    destroy_nhwindow(win);
    return ECMD_OK;
}

/* #vision command */
int
wiz_show_vision(void)
{
    winid win;
    coordxy x, y;
    int v;
    char row[COLNO + 1];

    win = create_nhwindow(NHW_TEXT);
    Sprintf(row, "Flags: 0x%x could see, 0x%x in sight, 0x%x temp lit",
            COULD_SEE, IN_SIGHT, TEMP_LIT);
    putstr(win, 0, row);
    putstr(win, 0, "");
    for (y = 0; y < ROWNO; y++) {
        for (x = 1; x < COLNO; x++) {
            if (u_at(x, y)) {
                row[x] = '@';
            } else {
                v = gv.viz_array[y][x]; /* data access should be hidden */
                row[x] = (v == 0) ? ' ' : ('0' + v);
            }
        }
        /* remove trailing spaces */
        for (x = COLNO - 1; x >= 1; x--)
            if (row[x] != ' ')
                break;
        row[x + 1] = '\0';

        putstr(win, 0, &row[1]);
    }
    display_nhwindow(win, TRUE);
    destroy_nhwindow(win);
    return ECMD_OK;
}

/* #wmode command */
int
wiz_show_wmodes(void)
{
    winid win;
    coordxy x, y;
    char row[COLNO + 1];
    struct rm *lev;
    boolean istty = WINDOWPORT(tty);

    win = create_nhwindow(NHW_TEXT);
    if (istty)
        putstr(win, 0, ""); /* tty only: blank top line */
    for (y = 0; y < ROWNO; y++) {
        for (x = 0; x < COLNO; x++) {
            lev = &levl[x][y];
            if (u_at(x, y))
                row[x] = '@';
            else if (IS_WALL(lev->typ) || lev->typ == SDOOR)
                row[x] = '0' + (lev->wall_info & WM_MASK);
            else if (lev->typ == CORR)
                row[x] = '#';
            else if (IS_ROOM(lev->typ) || IS_DOOR(lev->typ))
                row[x] = '.';
            else
                row[x] = 'x';
        }
        row[COLNO] = '\0';
        /* map column 0, levl[0][], is off the left edge of the screen */
        putstr(win, 0, &row[1]);
    }
    display_nhwindow(win, TRUE);
    destroy_nhwindow(win);
    return ECMD_OK;
}

/* wizard mode variant of #terrain; internal levl[][].typ values in base-36 */
void
wiz_map_levltyp(void)
{
    winid win;
    coordxy x, y;
    int terrain;
    char row[COLNO + 1];
    boolean istty = !strcmp(windowprocs.name, "tty");

    win = create_nhwindow(NHW_TEXT);
    /* map row 0, levl[][0], is drawn on the second line of tty screen */
    if (istty)
        putstr(win, 0, ""); /* tty only: blank top line */
    for (y = 0; y < ROWNO; y++) {
        /* map column 0, levl[0][], is off the left edge of the screen;
           it should always have terrain type "undiggable stone" */
        for (x = 1; x < COLNO; x++) {
            terrain = levl[x][y].typ;
            /* assumes there aren't more than 10+26+26 terrain types */
            row[x - 1] = (char) ((terrain == STONE && !may_dig(x, y))
                                    ? '*'
                                    : (terrain < 10)
                                       ? '0' + terrain
                                       : (terrain < 36)
                                          ? 'a' + terrain - 10
                                          : 'A' + terrain - 36);
        }
        x--;
        if (levl[0][y].typ != STONE || may_dig(0, y))
            row[x++] = '!';
        row[x] = '\0';
        putstr(win, 0, row);
    }

    {
        char dsc[COLBUFSZ];
        s_level *slev = Is_special(&u.uz);

        Sprintf(dsc, "D:%d,L:%d", u.uz.dnum, u.uz.dlevel);
        /* [dungeon branch features currently omitted] */
        /* special level features */
        if (slev) {
            Sprintf(eos(dsc), " \"%s\"", slev->proto);
            /* special level flags (note: dungeon.def doesn't set `maze'
               or `hell' for any specific levels so those never show up) */
            if (slev->flags.maze_like)
                Strcat(dsc, " mazelike");
            if (slev->flags.hellish)
                Strcat(dsc, " hellish");
            if (slev->flags.town)
                Strcat(dsc, " town");
            if (slev->flags.rogue_like)
                Strcat(dsc, " roguelike");
            /* alignment currently omitted to save space */
        }
        /* level features */
        if (svl.level.flags.nfountains)
            Sprintf(eos(dsc), " %c:%d", defsyms[S_fountain].sym,
                    (int) svl.level.flags.nfountains);
        if (svl.level.flags.nsinks)
            Sprintf(eos(dsc), " %c:%d", defsyms[S_sink].sym,
                    (int) svl.level.flags.nsinks);
        if (svl.level.flags.has_vault)
            Strcat(dsc, " vault");
        if (svl.level.flags.has_shop)
            Strcat(dsc, " shop");
        if (svl.level.flags.has_temple)
            Strcat(dsc, " temple");
        if (svl.level.flags.has_court)
            Strcat(dsc, " throne");
        if (svl.level.flags.has_zoo)
            Strcat(dsc, " zoo");
        if (svl.level.flags.has_morgue)
            Strcat(dsc, " morgue");
        if (svl.level.flags.has_barracks)
            Strcat(dsc, " barracks");
        if (svl.level.flags.has_beehive)
            Strcat(dsc, " hive");
        if (svl.level.flags.has_swamp)
            Strcat(dsc, " swamp");
        /* level flags */
        if (svl.level.flags.noteleport)
            Strcat(dsc, " noTport");
        if (svl.level.flags.hardfloor)
            Strcat(dsc, " noDig");
        if (svl.level.flags.nommap)
            Strcat(dsc, " noMMap");
        if (!svl.level.flags.hero_memory)
            Strcat(dsc, " noMem");
        if (svl.level.flags.shortsighted)
            Strcat(dsc, " shortsight");
        if (svl.level.flags.graveyard)
            Strcat(dsc, " graveyard");
        if (svl.level.flags.is_maze_lev)
            Strcat(dsc, " maze");
        if (svl.level.flags.is_cavernous_lev)
            Strcat(dsc, " cave");
        if (svl.level.flags.arboreal)
            Strcat(dsc, " tree");
        if (Sokoban)
            Strcat(dsc, " sokoban-rules");
        /* non-flag info; probably should include dungeon branching
           checks (extra stairs and magic portals) here */
        if (Invocation_lev(&u.uz))
            Strcat(dsc, " invoke");
        if (On_W_tower_level(&u.uz))
            Strcat(dsc, " tower");
        /* append a branch identifier for completeness' sake */
        if (u.uz.dnum == 0)
            Strcat(dsc, " dungeon");
        else if (u.uz.dnum == mines_dnum)
            Strcat(dsc, " mines");
        else if (In_sokoban(&u.uz))
            Strcat(dsc, " sokoban");
        else if (u.uz.dnum == quest_dnum)
            Strcat(dsc, " quest");
        else if (Is_knox(&u.uz))
            Strcat(dsc, " ludios");
        else if (u.uz.dnum == 1)
            Strcat(dsc, " gehennom");
        else if (u.uz.dnum == tower_dnum)
            Strcat(dsc, " vlad");
        else if (In_endgame(&u.uz))
            Strcat(dsc, " endgame");
        else {
            /* somebody's added a dungeon branch we're not expecting */
            const char *brname = svd.dungeons[u.uz.dnum].dname;

            if (!brname || !*brname)
                brname = "unknown";
            if (!strncmpi(brname, "the ", 4))
                brname += 4;
            Sprintf(eos(dsc), " %s", brname);
        }
        /* limit the line length to map width */
        if (strlen(dsc) >= COLNO)
            dsc[COLNO - 1] = '\0'; /* truncate */
        putstr(win, 0, dsc);
    }

    display_nhwindow(win, TRUE);
    destroy_nhwindow(win);
    return;
}

DISABLE_WARNING_FORMAT_NONLITERAL

/* explanation of base-36 output from wiz_map_levltyp() */
void
wiz_levltyp_legend(void)
{
    winid win;
    int i, j, last, c;
    const char *dsc, *fmt;
    char buf[BUFSZ];

    win = create_nhwindow(NHW_TEXT);
    putstr(win, 0, "#terrain encodings:");
    putstr(win, 0, "");
    fmt = " %c - %-28s"; /* TODO: include tab-separated variant for win32 */
    *buf = '\0';
    /* output in pairs, left hand column holds [0],[1],...,[N/2-1]
       and right hand column holds [N/2],[N/2+1],...,[N-1];
       N ('last') will always be even, and may or may not include
       the empty string entry to pad out the final pair, depending
       upon how many other entries are present in levltyp[] */
    last = SIZE(levltyp) & ~1;
    for (i = 0; i < last / 2; ++i)
        for (j = i; j < last; j += last / 2) {
            dsc = levltyp[j];
            c = !*dsc ? ' '
                   : !strncmp(dsc, "unreachable", 11) ? '*'
                      /* same int-to-char conversion as wiz_map_levltyp() */
                      : (j < 10) ? '0' + j
                         : (j < 36) ? 'a' + j - 10
                            : 'A' + j - 36;
            Sprintf(eos(buf), fmt, c, dsc);
            if (j > i) {
                putstr(win, 0, buf);
                *buf = '\0';
            }
        }
    display_nhwindow(win, TRUE);
    destroy_nhwindow(win);
    return;
}

RESTORE_WARNING_FORMAT_NONLITERAL

DISABLE_WARNING_CONDEXPR_IS_CONSTANT

/* #wizsmell command - test usmellmon(). */
int
wiz_smell(void)
{
    struct monst *mtmp; /* monster being smelled */
    struct permonst *mptr;
    int ans, glyph;
    coord cc; /* screen pos to sniff */
    boolean is_you;

    cc.x = u.ux;
    cc.y = u.uy;
    if (!olfaction(gy.youmonst.data)) {
        You("are incapable of detecting odors in your present form.");
        return ECMD_OK;
    }

    You("can move the cursor to a monster that you want to smell.");
    do {
        pline("Pick a monster to smell.");
        ans = getpos(&cc, TRUE, "a monster");
        if (ans < 0 || cc.x < 0) {
            return ECMD_CANCEL; /* done */
        }
        is_you = FALSE;
        if (u_at(cc.x, cc.y)) {
            if (u.usteed) {
                mptr = u.usteed->data;
            } else {
                mptr = gy.youmonst.data;
                is_you = TRUE;
            }
        } else if ((mtmp = m_at(cc.x, cc.y)) != (struct monst *) 0) {
            mptr = mtmp->data;
        } else {
            mptr = (struct permonst *) 0;
        }
        /* Buglet: mapping or unmapping "remembered, unseen monster" should
           cause time to elapse; since we're in wizmode, don't bother */
        glyph = glyph_at(cc.x, cc.y);
        /* Is it a monster? */
        if (mptr) {
            if (is_you)
                You("surreptitiously sniff under your %s.", body_part(ARM));
            if (!usmellmon(mptr))
                pline("%s to not give off any smell.",
                      is_you ? "You seem" : "That monster seems");
            if (!glyph_is_monster(glyph))
                map_invisible(cc.x, cc.y);
        } else {
            You("don't smell any monster there.");
            if (glyph_is_invisible(glyph))
                unmap_invisible(cc.x, cc.y);
        }
    } while (TRUE);
    return ECMD_OK;
}

RESTORE_WARNING_CONDEXPR_IS_CONSTANT

DISABLE_WARNING_FORMAT_NONLITERAL

#define DEFAULT_TIMEOUT_INCR 30

/* #wizinstrinsic command to set some intrinsics for testing */
int
wiz_intrinsic(void)
{
    if (wizard) {
        static const char wizintrinsic[] = "#wizintrinsic";
        static const char fmt[] = "You are%s %s.";
        winid win;
        anything any;
        char buf[BUFSZ];
        int i, j, n, amt, typ, p = 0;
        long oldtimeout, newtimeout;
        const char *propname;
        menu_item *pick_list = (menu_item *) 0;
        int clr = NO_COLOR;

        any = cg.zeroany;
        win = create_nhwindow(NHW_MENU);
        start_menu(win, MENU_BEHAVE_STANDARD);
        if (iflags.cmdassist) {
            /* start menu with a subtitle */
            Sprintf(buf,
        "[Precede any selection with a count to increment by other than %d.]",
                    DEFAULT_TIMEOUT_INCR);
            add_menu_str(win, buf);
        }
        for (i = 0; (propname = property_by_index(i, &p)) != 0; ++i) {
            if (p == HALLUC_RES) {
                /* Grayswandir vs hallucination; ought to be redone to
                   use u.uprops[HALLUC].blocked instead of being treated
                   as a separate property; letting in be manually toggled
                   even only in wizard mode would be asking for trouble... */
                continue;
            }
            if (p == FIRE_RES) {
                /* FIRE_RES and properties beyond it (in the propertynames[]
                   ordering, not their numerical PROP values), can only be
                   set to timed values here so show a separator */
                add_menu_str(win, "--");
            }
            any.a_int = i + 1; /* +1: avoid 0 */
            oldtimeout = u.uprops[p].intrinsic & TIMEOUT;
            if (oldtimeout)
                Sprintf(buf, "%-27s [%li]", propname, oldtimeout);
            else
                Sprintf(buf, "%s", propname);
            add_menu(win, &nul_glyphinfo, &any, 0, 0, ATR_NONE, clr, buf,
                     MENU_ITEMFLAGS_NONE);
        }
        end_menu(win, "Which intrinsics?");
        n = select_menu(win, PICK_ANY, &pick_list);
        destroy_nhwindow(win);

        for (j = 0; j < n; ++j) {
            i = pick_list[j].item.a_int - 1; /* -1: reverse +1 above */
            propname = property_by_index(i, &p);
            oldtimeout = u.uprops[p].intrinsic & TIMEOUT;
            amt = (pick_list[j].count == -1L) ? DEFAULT_TIMEOUT_INCR
                                              : (int) pick_list[j].count;
            if (amt <= 0) /* paranoia */
                continue;
            newtimeout = oldtimeout + (long) amt;

            switch (p) {
            case SICK:
            case SLIMED:
            case STONED:
                if (oldtimeout > 0L && newtimeout > oldtimeout)
                    newtimeout = oldtimeout;
                break;
            }

            switch (p) {
            case BLINDED:
                make_blinded(newtimeout, TRUE);
                break;
#if 0       /* make_confused() only gives feedback when confusion is
             * ending so use the 'default' case for it instead */
            case CONFUSION:
                make_confused(newtimeout, TRUE);
                break;
#endif /*0*/
            case DEAF:
                make_deaf(newtimeout, TRUE);
                break;
            case HALLUC:
                make_hallucinated(newtimeout, TRUE, 0L);
                break;
            case SICK:
                typ = !rn2(2) ? SICK_VOMITABLE : SICK_NONVOMITABLE;
                make_sick(newtimeout, wizintrinsic, TRUE, typ);
                break;
            case SLIMED:
                Sprintf(buf, fmt,
                        !Slimed ? "" : " still", "turning into slime");
                make_slimed(newtimeout, buf);
                break;
            case STONED:
                Sprintf(buf, fmt,
                        !Stoned ? "" : " still", "turning into stone");
                make_stoned(newtimeout, buf, KILLED_BY, wizintrinsic);
                break;
            case STUNNED:
                make_stunned(newtimeout, TRUE);
                break;
            case VOMITING:
                Sprintf(buf, fmt, !Vomiting ? "" : " still", "vomiting");
                make_vomiting(newtimeout, FALSE);
                pline1(buf);
                break;
            case WARN_OF_MON:
                if (!Warn_of_mon) {
                    svc.context.warntype.speciesidx = PM_GRID_BUG;
                    svc.context.warntype.species
                                     = &mons[svc.context.warntype.speciesidx];
                }
                goto def_feedback;
            case GLIB:
                /* slippery fingers might need a persistent inventory update
                   so needs more than simple incr_itimeout() but we want
                   the pline() issued with that */
                make_glib((int) newtimeout);
                FALLTHROUGH;
                /*FALLTHRU*/
            default:
 def_feedback:
                if (p != GLIB)
                    incr_itimeout(&u.uprops[p].intrinsic, amt);
                disp.botl = TRUE; /* have pline() do a status update */
                pline("Timeout for %s %s %d.", propname,
                      oldtimeout ? "increased by" : "set to", amt);
                break;
            }
            /* this has to be after incr_itimeout() */
            if (p == LEVITATION || p == FLYING)
                float_vs_flight();
            else if (p == PROT_FROM_SHAPE_CHANGERS)
                rescham();
            if (p == WWALKING || p == LEVITATION || p == FLYING) {
                if (u.uinwater)
                    (void) pooleffects(FALSE);
            }
        }
        if (n >= 1)
            free((genericptr_t) pick_list);
        docrt();
    } else
        pline(unavailcmd, ecname_from_fn(wiz_intrinsic));
    return ECMD_OK;
}

RESTORE_WARNING_FORMAT_NONLITERAL

/* #wizrumorcheck command - verify each rumor access */
int
wiz_rumor_check(void)
{
    rumor_check();
    return ECMD_OK;
}

/*
 * wizard mode sanity_check code
 */

static const char template[] = "%-27s  %4ld  %6ld";
static const char stats_hdr[] = "                             count  bytes";
static const char stats_sep[] = "---------------------------  ----- -------";

staticfn int
size_obj(struct obj *otmp)
{
    int sz = (int) sizeof (struct obj);

    if (otmp->oextra) {
        sz += (int) sizeof (struct oextra);
        if (ONAME(otmp))
            sz += (int) strlen(ONAME(otmp)) + 1;
        if (OMONST(otmp))
            sz += size_monst(OMONST(otmp), FALSE);
        if (OMAILCMD(otmp))
            sz += (int) strlen(OMAILCMD(otmp)) + 1;
        /* sz += (int) sizeof (unsigned); -- now part of oextra itself */
    }
    return sz;
}

staticfn void
count_obj(struct obj *chain, long *total_count, long *total_size,
          boolean top, boolean recurse)
{
    long count, size;
    struct obj *obj;

    for (count = size = 0, obj = chain; obj; obj = obj->nobj) {
        if (top) {
            count++;
            size += size_obj(obj);
        }
        if (recurse && obj->cobj)
            count_obj(obj->cobj, total_count, total_size, TRUE, TRUE);
    }
    *total_count += count;
    *total_size += size;
}

DISABLE_WARNING_FORMAT_NONLITERAL  /* RESTORE_WARNING follows wiz_show_stats */

staticfn void
obj_chain(
    winid win,
    const char *src,
    struct obj *chain,
    boolean force,
    long *total_count, long *total_size)
{
    char buf[BUFSZ];
    long count = 0L, size = 0L;

    count_obj(chain, &count, &size, TRUE, FALSE);

    if (count || size || force) {
        *total_count += count;
        *total_size += size;
        Sprintf(buf, template, src, count, size);
        putstr(win, 0, buf);
    }
}

staticfn void
mon_invent_chain(
    winid win,
    const char *src,
    struct monst *chain,
    long *total_count, long *total_size)
{
    char buf[BUFSZ];
    long count = 0, size = 0;
    struct monst *mon;

    for (mon = chain; mon; mon = mon->nmon)
        count_obj(mon->minvent, &count, &size, TRUE, FALSE);

    if (count || size) {
        *total_count += count;
        *total_size += size;
        Sprintf(buf, template, src, count, size);
        putstr(win, 0, buf);
    }
}

staticfn void
contained_stats(
    winid win,
    const char *src,
    long *total_count, long *total_size)
{
    char buf[BUFSZ];
    long count = 0, size = 0;
    struct monst *mon;

    count_obj(gi.invent, &count, &size, FALSE, TRUE);
    count_obj(fobj, &count, &size, FALSE, TRUE);
    count_obj(svl.level.buriedobjlist, &count, &size, FALSE, TRUE);
    count_obj(gm.migrating_objs, &count, &size, FALSE, TRUE);
    /* DEADMONSTER check not required in this loop since they have no
     * inventory */
    for (mon = fmon; mon; mon = mon->nmon)
        count_obj(mon->minvent, &count, &size, FALSE, TRUE);
    for (mon = gm.migrating_mons; mon; mon = mon->nmon)
        count_obj(mon->minvent, &count, &size, FALSE, TRUE);

    if (count || size) {
        *total_count += count;
        *total_size += size;
        Sprintf(buf, template, src, count, size);
        putstr(win, 0, buf);
    }
}

staticfn int
size_monst(struct monst *mtmp, boolean incl_wsegs)
{
    int sz = (int) sizeof (struct monst);

    if (mtmp->wormno && incl_wsegs)
        sz += size_wseg(mtmp);

    if (mtmp->mextra) {
        sz += (int) sizeof (struct mextra);
        if (MGIVENNAME(mtmp))
            sz += (int) strlen(MGIVENNAME(mtmp)) + 1;
        if (EGD(mtmp))
            sz += (int) sizeof (struct egd);
        if (EPRI(mtmp))
            sz += (int) sizeof (struct epri);
        if (ESHK(mtmp))
            sz += (int) sizeof (struct eshk);
        if (EMIN(mtmp))
            sz += (int) sizeof (struct emin);
        if (EDOG(mtmp))
            sz += (int) sizeof (struct edog);
        if (EBONES(mtmp))
            sz += (int) sizeof (struct ebones);
        /* mextra->mcorpsenm doesn't point to more memory */
    }
    return sz;
}

staticfn void
mon_chain(
    winid win,
    const char *src,
    struct monst *chain,
    boolean force,
    long *total_count, long *total_size)
{
    char buf[BUFSZ];
    long count, size;
    struct monst *mon;
    /* mon->wormno means something different for migrating_mons and mydogs */
    boolean incl_wsegs = !strcmpi(src, "fmon");

    count = size = 0L;
    for (mon = chain; mon; mon = mon->nmon) {
        count++;
        size += size_monst(mon, incl_wsegs);
    }
    if (count || size || force) {
        *total_count += count;
        *total_size += size;
        Sprintf(buf, template, src, count, size);
        putstr(win, 0, buf);
    }
}

staticfn void
misc_stats(
    winid win,
    long *total_count, long *total_size)
{
    char buf[BUFSZ], hdrbuf[QBUFSZ];
    long count, size;
    int idx;
    struct trap *tt;
    struct damage *sd; /* shop damage */
    struct kinfo *k; /* delayed killer */
    struct cemetery *bi; /* bones info */

    /* traps and engravings are output unconditionally;
     * others only if nonzero
     */
    count = size = 0L;
    for (tt = gf.ftrap; tt; tt = tt->ntrap) {
        ++count;
        size += (long) sizeof *tt;
    }
    *total_count += count;
    *total_size += size;
    Sprintf(hdrbuf, "traps, size %ld", (long) sizeof (struct trap));
    Sprintf(buf, template, hdrbuf, count, size);
    putstr(win, 0, buf);

    count = size = 0L;
    engr_stats("engravings, size %ld+text", hdrbuf, &count, &size);
    *total_count += count;
    *total_size += size;
    Sprintf(buf, template, hdrbuf, count, size);
    putstr(win, 0, buf);

    count = size = 0L;
    light_stats("light sources, size %ld", hdrbuf, &count, &size);
    if (count || size) {
        *total_count += count;
        *total_size += size;
        Sprintf(buf, template, hdrbuf, count, size);
        putstr(win, 0, buf);
    }

    count = size = 0L;
    timer_stats("timers, size %ld", hdrbuf, &count, &size);
    if (count || size) {
        *total_count += count;
        *total_size += size;
        Sprintf(buf, template, hdrbuf, count, size);
        putstr(win, 0, buf);
    }

    count = size = 0L;
    for (sd = svl.level.damagelist; sd; sd = sd->next) {
        ++count;
        size += (long) sizeof *sd;
    }
    if (count || size) {
        *total_count += count;
        *total_size += size;
        Sprintf(hdrbuf, "shop damage, size %ld",
                (long) sizeof (struct damage));
        Sprintf(buf, template, hdrbuf, count, size);
        putstr(win, 0, buf);
    }

    count = size = 0L;
    region_stats("regions, size %ld+%ld*rect+N", hdrbuf, &count, &size);
    if (count || size) {
        *total_count += count;
        *total_size += size;
        Sprintf(buf, template, hdrbuf, count, size);
        putstr(win, 0, buf);
    }

    count = size = 0L;
    for (k = svk.killer.next; k; k = k->next) {
        ++count;
        size += (long) sizeof *k;
    }
    if (count || size) {
        *total_count += count;
        *total_size += size;
        Sprintf(hdrbuf, "delayed killer%s, size %ld",
                plur(count), (long) sizeof (struct kinfo));
        Sprintf(buf, template, hdrbuf, count, size);
        putstr(win, 0, buf);
    }

    count = size = 0L;
    for (bi = svl.level.bonesinfo; bi; bi = bi->next) {
        ++count;
        size += (long) sizeof *bi;
    }
    if (count || size) {
        *total_count += count;
        *total_size += size;
        Sprintf(hdrbuf, "bones history, size %ld",
                (long) sizeof (struct cemetery));
        Sprintf(buf, template, hdrbuf, count, size);
        putstr(win, 0, buf);
    }

    count = size = 0L;
    for (idx = 0; idx < NUM_OBJECTS; ++idx)
        if (objects[idx].oc_uname) {
            ++count;
            size += (long) (strlen(objects[idx].oc_uname) + 1);
        }
    if (count || size) {
        *total_count += count;
        *total_size += size;
        Strcpy(hdrbuf, "object type names, text");
        Sprintf(buf, template, hdrbuf, count, size);
        putstr(win, 0, buf);
    }
}

staticfn void
you_sanity_check(void)
{
    struct monst *mtmp;

    if (u.uswallow && !u.ustuck) {
        /* this probably ought to be panic() */
        impossible("sanity_check: swallowed by nothing?");
        display_nhwindow(WIN_MESSAGE, TRUE);
        /* try to recover from whatever the problem is */
        u.uswallow = 0;
        u.uswldtim = 0;
        docrt();
    }
    if ((mtmp = m_at(u.ux, u.uy)) != 0) {
        /* u.usteed isn't on the map */
        if (u.ustuck != mtmp)
            impossible("sanity_check: you over monster");
    }
    /* [should we also check for (u.uhp < 1), (Upolyd && u.mh < 1),
       and (u.uen < 0) here?] */
    if (u.uhp > u.uhpmax) {
        impossible("current hero health (%d) better than maximum? (%d)",
                   u.uhp, u.uhpmax);
        u.uhp = u.uhpmax;
    }
    if (Upolyd && u.mh > u.mhmax) {
        impossible(
              "current hero health as monster (%d) better than maximum? (%d)",
                   u.mh, u.mhmax);
        u.mh = u.mhmax;
    }
    if (u.uen > u.uenmax) {
        impossible("current hero energy (%d) better than maximum? (%d)",
                   u.uen, u.uenmax);
        u.uen = u.uenmax;
    }

    check_wornmask_slots();
    (void) check_invent_gold("invent");
}

staticfn void
levl_sanity_check(void)
{
    coordxy x, y;

    if (Underwater)
        return; /* Underwater uses different vision */

    for (y = 0; y < ROWNO; y++) {
        for (x = 1; x < COLNO; x++) {
            if ((does_block(x, y, &levl[x][y]) ? 1 : 0) != get_viz_clear(x, y))
                impossible("levl[%i][%i] vision blocking", x, y);
        }
    }
}

void
sanity_check(void)
{
    if (iflags.sanity_no_check) {
        /* in case a recurring sanity_check warning occurs, we mustn't
           re-trigger it when ^P is used, otherwise msg_window:Single
           and msg_window:Combination will always repeat the most recent
           instance, never able to go back to any earlier messages */
        iflags.sanity_no_check = FALSE;
        return;
    }
    program_state.in_sanity_check++;
    you_sanity_check();
    obj_sanity_check();
    timer_sanity_check();
    mon_sanity_check();
    light_sources_sanity_check();
    bc_sanity_check();
    trap_sanity_check();
    engraving_sanity_check();
    levl_sanity_check();
    program_state.in_sanity_check--;
}

/* qsort() comparison routine for use in list_migrating_mons() */
staticfn int QSORTCALLBACK
migrsort_cmp(const genericptr vptr1, const genericptr vptr2)
{
    const struct monst *m1 = *(const struct monst **) vptr1,
                       *m2 = *(const struct monst **) vptr2;
    int d1 = (int) m1->mux, l1 = (int) m1->muy,
        d2 = (int) m2->mux, l2 = (int) m2->muy;

    /* if different branches, sort by dungeon number */
    if (d1 != d2)
        return d1 - d2;
    /* within same branch, sort by level number */
    if (l1 != l2)
        return l1 - l2;
    /* same destination level:  use a tie-breaker to force stable sort;
       monst->m_id is unsigned so we need more than just simple subtraction */
    return (m1->m_id < m2->m_id) ? -1 : (m1->m_id > m2->m_id);
}

/* called by #migratemons; displays count of migrating monsters, optionally
   displays them as well */
staticfn void
list_migrating_mons(
    d_level *nextlevl) /* default destination for wiz_migrate_mons() */
{
    winid win = WIN_ERR;
    boolean showit = FALSE;
    unsigned n;
    int xyloc;
    coordxy x, y;
    char c, prmpt[10], xtra[10], buf[BUFSZ];
    struct monst *mtmp, **marray;
    int here = 0, nxtlv = 0, other = 0;

    for (mtmp = gm.migrating_mons; mtmp; mtmp = mtmp->nmon) {
        if (mtmp->mux == u.uz.dnum && mtmp->muy == u.uz.dlevel)
            ++here;
        else if (mtmp->mux == nextlevl->dnum && mtmp->muy == nextlevl->dlevel)
            ++nxtlv;
        else
            ++other;
    }
    if (here + nxtlv + other == 0) {
        pline("No monsters currently migrating.");
    } else {
        pline(
      "%d mon%s pending for current level, %d for next level, %d for others.",
              here, plur(here), nxtlv, other);
        prmpt[0] = xtra[0] = '\0';
        (void) strkitten(here ? prmpt : xtra, 'c');
        (void) strkitten(nxtlv ? prmpt : xtra, 'n');
        (void) strkitten(other ? prmpt : xtra, 'o');
        Strcat(prmpt, "a q");
        if (*xtra)
            Sprintf(eos(prmpt), "%c%s", '\033', xtra);
        c = yn_function("List which?", prmpt, 'q', TRUE);
        n = (c == 'c') ? here
            : (c == 'n') ? nxtlv
              : (c == 'o') ? other
                : (c == 'a') ? here + nxtlv + other
                  : 0;
        if (n > 0) {
            win = create_nhwindow(NHW_TEXT);
            switch (c) {
            case 'c':
            case 'n':
            case 'o':
                Sprintf(buf, "Monster%s migrating to %s:", plur(n),
                        (c == 'c') ? "current level"
                        : (c == 'n') ? "next level"
                          : "'other' levels");
                break;
            default:
                Strcpy(buf, "All migrating monsters:");
                break;
            }
            putstr(win, 0, buf);
            putstr(win, 0, "");
            /* collect the migrating monsters into an array; for 'o' and 'a'
               where multiple destination levels might be present, sort by
               the destination; 'c' and 'n' don't need to be sorted but we
               do that anyway to get the same tie-breaker as 'o' and 'a' */
            marray = (struct monst **) alloc((n + 1) * sizeof *marray);
            n = 0;
            for (mtmp = gm.migrating_mons; mtmp; mtmp = mtmp->nmon) {
                if (c == 'a')
                    showit = TRUE;
                else if (mtmp->mux == u.uz.dnum && mtmp->muy == u.uz.dlevel)
                    showit = (c == 'c');
                else if (mtmp->mux == nextlevl->dnum
                         && mtmp->muy == nextlevl->dlevel)
                    showit = (c == 'n');
                else
                    showit = (c == 'o');

                if (showit)
                    marray[n++] = mtmp;
            }
            marray[n] = (struct monst *) 0; /* mark end for traversal loop */
            if (n > 1)
                qsort((genericptr_t) marray, (size_t) n, sizeof *marray,
                      migrsort_cmp); /* sort elements [0] through [n-1] */
            for (n = 0; (mtmp = marray[n]) != 0; ++n) {
                Sprintf(buf, "  %s", minimal_monnam(mtmp, FALSE));
                /* minimal_monnam() appends map coordinates; strip that */
                (void) strsubst(buf, " <0,0>", "");
                if (has_mgivenname(mtmp)) /* if mtmp is named, include that */
                    Sprintf(eos(buf), " named %s", MGIVENNAME(mtmp));
                if (c == 'o' || c == 'a')
                    Sprintf(eos(buf), " to %d:%d", mtmp->mux, mtmp->muy);
                xyloc = mtmp->mtrack[0].x; /* (for legibility) */
                if (xyloc == MIGR_EXACT_XY) {
                    x = mtmp->mtrack[1].x;
                    y = mtmp->mtrack[1].y;
                    Sprintf(eos(buf), " at <%d,%d>", (int) x, (int) y);
                }
                putstr(win, 0, buf);
            }
            free((genericptr_t) marray);
            display_nhwindow(win, FALSE);
            destroy_nhwindow(win);
        } else if (c != 'q') {
            pline("None.");
        }

    }
}

/* the #stats command
 * Display memory usage of all monsters and objects on the level.
 */
int
wiz_show_stats(void)
{
    char buf[BUFSZ];
    winid win;
    long total_obj_size, total_obj_count,
         total_mon_size, total_mon_count,
         total_ovr_size, total_ovr_count,
         total_misc_size, total_misc_count;

    win = create_nhwindow(NHW_TEXT);
    putstr(win, 0, "Current memory statistics:");

    total_obj_count = total_obj_size = 0L;
    putstr(win, 0, stats_hdr);
    Sprintf(buf, "  Objects, base size %ld", (long) sizeof (struct obj));
    putstr(win, 0, buf);
    obj_chain(win, "invent", gi.invent, TRUE,
              &total_obj_count, &total_obj_size);
    obj_chain(win, "fobj", fobj, TRUE, &total_obj_count, &total_obj_size);
    obj_chain(win, "buried", svl.level.buriedobjlist, FALSE,
              &total_obj_count, &total_obj_size);
    obj_chain(win, "migrating obj", gm.migrating_objs, FALSE,
              &total_obj_count, &total_obj_size);
    obj_chain(win, "billobjs", gb.billobjs, FALSE,
              &total_obj_count, &total_obj_size);
    mon_invent_chain(win, "minvent", fmon, &total_obj_count, &total_obj_size);
    mon_invent_chain(win, "migrating minvent", gm.migrating_mons,
                     &total_obj_count, &total_obj_size);
    contained_stats(win, "contained", &total_obj_count, &total_obj_size);
    putstr(win, 0, stats_sep);
    Sprintf(buf, template, "  Obj total", total_obj_count, total_obj_size);
    putstr(win, 0, buf);

    total_mon_count = total_mon_size = 0L;
    putstr(win, 0, "");
    Sprintf(buf, "  Monsters, base size %ld", (long) sizeof (struct monst));
    putstr(win, 0, buf);
    mon_chain(win, "fmon", fmon, TRUE, &total_mon_count, &total_mon_size);
    mon_chain(win, "migrating", gm.migrating_mons, FALSE,
              &total_mon_count, &total_mon_size);
    /* 'gm.mydogs' is only valid during level change or end of game disclosure,
       but conceivably we've been called from within debugger at such time */
    if (gm.mydogs) /* monsters accompanying hero */
        mon_chain(win, "mydogs", gm.mydogs, FALSE,
                  &total_mon_count, &total_mon_size);
    putstr(win, 0, stats_sep);
    Sprintf(buf, template, "  Mon total", total_mon_count, total_mon_size);
    putstr(win, 0, buf);

    total_ovr_count = total_ovr_size = 0L;
    putstr(win, 0, "");
    putstr(win, 0, "  Overview");
    overview_stats(win, template, &total_ovr_count, &total_ovr_size);
    putstr(win, 0, stats_sep);
    Sprintf(buf, template, "  Over total", total_ovr_count, total_ovr_size);
    putstr(win, 0, buf);

    total_misc_count = total_misc_size = 0L;
    putstr(win, 0, "");
    putstr(win, 0, "  Miscellaneous");
    misc_stats(win, &total_misc_count, &total_misc_size);
    putstr(win, 0, stats_sep);
    Sprintf(buf, template, "  Misc total", total_misc_count, total_misc_size);
    putstr(win, 0, buf);

    putstr(win, 0, "");
    putstr(win, 0, stats_sep);
    Sprintf(buf, template, "  Grand total",
            (total_obj_count + total_mon_count
             + total_ovr_count + total_misc_count),
            (total_obj_size + total_mon_size
             + total_ovr_size + total_misc_size));
    putstr(win, 0, buf);

#if defined(__BORLANDC__) && !defined(_WIN32)
    show_borlandc_stats(win);
#endif

    display_nhwindow(win, FALSE);
    destroy_nhwindow(win);
    return ECMD_OK;
}

RESTORE_WARNING_FORMAT_NONLITERAL

#if (NH_DEVEL_STATUS != NH_STATUS_RELEASED) || defined(DEBUG)
/* the #wizdispmacros command
 * Verify that some display macros are returning sane values */
int
wiz_display_macros(void)
{
    static const char display_issues[] = "Display macro issues:";
    char buf[BUFSZ];
    winid win;
    int glyph, test, trouble = 0, no_glyph = NO_GLYPH, max_glyph = MAX_GLYPH;

    win = create_nhwindow(NHW_TEXT);

    for (glyph = 0; glyph < MAX_GLYPH; ++glyph) {
        /* glyph_is_cmap / glyph_to_cmap() */
        if (glyph_is_cmap(glyph)) {
            test = glyph_to_cmap(glyph);
            /* check for MAX_GLYPH return */
            if (test == no_glyph) {
                if (!trouble++)
                    putstr(win, 0, display_issues);
                Sprintf(buf, "glyph_is_cmap() / glyph_to_cmap(glyph=%d)"
                             " sync failure, returned NO_GLYPH (%d)",
                        glyph, test);
                 putstr(win, 0, buf);
            }
            if (glyph_is_cmap_zap(glyph)
                && !(test >= S_vbeam && test <= S_rslant)) {
                if (!trouble++)
                    putstr(win, 0, display_issues);
                Sprintf(buf,
                        "glyph_is_cmap_zap(glyph=%d) returned non-zap cmap %d",
                        glyph, test);
                 putstr(win, 0, buf);
            }
            /* check against defsyms array subscripts */
            if (!IndexOk(test, defsyms)) {
                if (!trouble++)
                    putstr(win, 0, display_issues);
                Sprintf(buf, "glyph_to_cmap(glyph=%d) returns %d"
                             " exceeds defsyms[%d] bounds (MAX_GLYPH = %d)",
                        glyph, test, SIZE(defsyms), max_glyph);
                putstr(win, 0, buf);
            }
        }
        /* glyph_is_monster / glyph_to_mon */
        if (glyph_is_monster(glyph)) {
            test = glyph_to_mon(glyph);
            /* check against mons array subscripts */
            if (test < 0 || test >= NUMMONS) {
                if (!trouble++)
                    putstr(win, 0, display_issues);
                Sprintf(buf, "glyph_to_mon(glyph=%d) returns %d"
                             " exceeds mons[%d] bounds",
                        glyph, test, NUMMONS);
                putstr(win, 0, buf);
            }
        }
        /* glyph_is_object / glyph_to_obj */
        if (glyph_is_object(glyph)) {
            test = glyph_to_obj(glyph);
            /* check against objects array subscripts */
            if (test < 0 || test > NUM_OBJECTS) {
                if (!trouble++)
                    putstr(win, 0, display_issues);
                Sprintf(buf, "glyph_to_obj(glyph=%d) returns %d"
                             " exceeds objects[%d] bounds",
                        glyph, test, NUM_OBJECTS);
                putstr(win, 0, buf);
            }
        }
    }
    if (!trouble)
        putstr(win, 0, "No display macro issues detected.");
    display_nhwindow(win, FALSE);
    destroy_nhwindow(win);
    return ECMD_OK;
}

/* the #wizshownhuuid command */
int
wiz_show_nhuuid(void)
{
    pline("The NHUUID for this game is { %s }.", svn.nhuuid);
    return ECMD_OK;
}

/* the #wizmondiff command */
int
wiz_mon_diff(void)
{
    static const char window_title[] = "Review of monster difficulty ratings"
                                       " [index:level]:";
    char buf[BUFSZ];
    winid win;
    int mhardcoded = 0, mcalculated = 0, trouble = 0, cnt = 0, mdiff = 0;
    int mlev;
    struct permonst *ptr;

    /*
     * Possible extension:  choose between showing discrepancies,
     * showing all monsters, or monsters within a particular class.
     */

    win = create_nhwindow(NHW_TEXT);
    for (ptr = &mons[0]; ptr->mlet; ptr++, cnt++) {
        mcalculated = mstrength(ptr);
        mhardcoded = (int) ptr->difficulty;
        mdiff = mhardcoded - mcalculated;
        if (mdiff) {
            if (!trouble++)
                putstr(win, 0, window_title);
            mlev = (int) ptr->mlevel;
            if (mlev > 50) /* hack for named demons */
                mlev = 50;
            Snprintf(buf, sizeof buf,
                     "%-18s [%3d:%2d]: calculated: %2d, hardcoded: %2d (%+d)",
                     ptr->pmnames[NEUTRAL], cnt, mlev,
                     mcalculated, mhardcoded, mdiff);
            putstr(win, 0, buf);
        }
    }
    if (!trouble)
        putstr(win, 0, "No monster difficulty discrepancies were detected.");
    display_nhwindow(win, FALSE);
    destroy_nhwindow(win);
    return ECMD_OK;
}

/* the #wizobjprobs command */
int
wiz_objprobs(void)
{
    int win;
    char buf[BUFSZ];
    int probsum[MAXOCLASSES];
    int otyp;
    int oclass = objects[FIRST_OBJECT].oc_class;
    memset(probsum, 0, sizeof probsum);

    for (otyp = FIRST_OBJECT; otyp < NUM_OBJECTS; otyp++) {
        probsum[(int) objects[otyp].oc_class] += objects[otyp].oc_prob;
    }

    win = create_nhwindow(NHW_TEXT);
    for (otyp = FIRST_OBJECT; otyp < NUM_OBJECTS; otyp++) {
        /* placeholders for extra descriptions aren't generatable objects */
        if (!OBJ_NAME(objects[otyp]))
            continue;

        if ((int) objects[otyp].oc_class != oclass) {
            putstr(win, 0, "");
        }
        oclass = objects[otyp].oc_class;

        Snprintf(buf, sizeof buf, "%4d / %4d (%6.2f%%): %s",
                 objects[otyp].oc_prob,
                 probsum[oclass],
                 (float) objects[otyp].oc_prob * 100.f /
                 (float) probsum[oclass],
                 OBJ_NAME(objects[otyp]));
        putstr(win, 0, buf);
    }
    display_nhwindow(win, FALSE);
    destroy_nhwindow(win);

    return ECMD_OK;
}
#endif /* (NH_DEVEL_STATUS != NH_STATUS_RELEASED) || defined(DEBUG) */

/* #migratemons command */
int
wiz_migrate_mons(void)
{
#ifdef DEBUG_MIGRATING_MONS
    int mcount;
    char inbuf[BUFSZ];
    struct permonst *ptr;
    struct monst *mtmp;
    boolean use_random_mon = TRUE;
    boolean mongen_saved = iflags.debug_mongen;
#endif
    d_level tolevel;

    if (Is_stronghold(&u.uz))
        assign_level(&tolevel, &valley_level);
    else if (!Is_botlevel(&u.uz))
        get_level(&tolevel, depth(&u.uz) + 1);
    else
        tolevel.dnum = 0, tolevel.dlevel = 0;

    list_migrating_mons(&tolevel);

#ifdef DEBUG_MIGRATING_MONS
    inbuf[0] = inbuf[1] = '\0';
    if (tolevel.dnum || tolevel.dlevel)
        getlin("How many random monsters to migrate to next level? [0]",
               inbuf);
    else
        pline("Can't get there from here.");
    if (*inbuf == '\033' || *inbuf == '\0')
        return ECMD_OK;

    mcount = atoi(inbuf);
    if (mcount < 0) {
        use_random_mon = FALSE;
        mcount *= -1;
    }
    if (mcount < 1)
        mcount = 0;
    else if (mcount > ((COLNO - 1) * ROWNO))
        mcount = (COLNO - 1) * ROWNO;

    iflags.debug_mongen = FALSE;
    while (mcount > 0) {
        if (use_random_mon) {
            ptr = rndmonst();
            mtmp = makemon(ptr, 0, 0, MM_NOMSG);
        } else {
            mtmp = fmon;
        }
        if (mtmp)
            migrate_to_level(mtmp, ledger_no(&tolevel), MIGR_RANDOM,
                                 (coord *) 0);
        mcount--;
    }
    iflags.debug_mongen = mongen_saved;
#endif /* DEBUG_MIGRATING_MONS */
    return ECMD_OK;
}

/* #wizcustom command to see glyphmap customizations */
int
wiz_custom(void)
{
    extern const char *const known_handling[];     /* symbols.c */

    if (wizard) {
        static const char wizcustom[] = "#wizcustom";
        winid win;
        char buf[BUFSZ], bufa[BUFSZ];
        int n;
#if 0
        int j, glyph;
#endif
        menu_item *pick_list = (menu_item *) 0;

        if (!glyphname_hash_indices_loaded())
            populate_glyphname_hash_indices();

        win = create_nhwindow(NHW_MENU);
        start_menu(win, MENU_BEHAVE_STANDARD);
        const char *heading = iflags.menu_tab_sep
                        ? "glyph\tglyph identifier\tsym\tclr\tcustomcolor\tunicode\tutf8"
                        : "    glyph  glyph identifier                        "
                          "     sym   clr customcolor unicode utf8";
        add_menu_heading(win, heading);
        Sprintf(bufa, "%s: colorcount=%ld %s", wizcustom,
                (long) iflags.colorcount,
                gs.symset[PRIMARYSET].name ? gs.symset[PRIMARYSET].name
                                          : "default");
        if (gc.currentgraphics == PRIMARYSET && gs.symset[PRIMARYSET].name)
            Strcat(bufa, ", active");
        if (gs.symset[PRIMARYSET].handling) {
            Sprintf(eos(bufa), ", handler=%s",
                    known_handling[gs.symset[PRIMARYSET].handling]);
        }
        Sprintf(buf, "%s", bufa);
        wizcustom_glyphnames(win);
        end_menu(win, bufa);
        n = select_menu(win, PICK_NONE, &pick_list);
        destroy_nhwindow(win);
#if 0
        for (j = 0; j < n; ++j) {
            glyph = pick_list[j].item.a_int - 1; /* -1: reverse +1 above */
        }
#endif
        if (n >= 1)
            free((genericptr_t) pick_list);
        if (glyphname_hash_indices_loaded())
            empty_glyphname_hash_indices();
        docrt();
    } else
        pline(unavailcmd, ecname_from_fn(wiz_custom));
    return ECMD_OK;
}

void
wizcustom_callback(winid win, int glyphnum, char *id)
{
    extern glyph_map glyphmap[MAX_GLYPH];
    glyph_map *cgm;
    int clr = NO_COLOR;
    char buf[BUFSZ], bufa[BUFSZ], bufb[BUFSZ], bufc[BUFSZ], bufu[BUFSZ];
    anything any;

    if (win && id) {
        cgm = &glyphmap[glyphnum];
        if (
#ifdef ENHANCED_SYMBOLS
            cgm->u ||
#endif
            cgm->customcolor != 0) {
            if (iflags.menu_tab_sep) {
                Sprintf(bufa, "[%04d]\t%s\t", glyphnum, id);
                Sprintf(bufb, "'\\%03d'\t%02d\t",
                        gs.showsyms[cgm->sym.symidx], cgm->sym.color);
                Sprintf(bufc, "%011lx\t", (unsigned long) cgm->customcolor);
            } else {
                Sprintf(bufa, "[%04d] %-45s", glyphnum, id);
                Sprintf(bufb, "'\\%03d' %02d ",
                        gs.showsyms[cgm->sym.symidx], cgm->sym.color);
                Sprintf(bufc, "%011lx ", (unsigned long) cgm->customcolor);
            }
            bufu[0] = '\0';
#ifdef ENHANCED_SYMBOLS
            if (cgm->u && cgm->u->utf8str) {
                uint8 *cp;
                char sep = iflags.menu_tab_sep ? '\t' : ' ';
                Sprintf(bufu, "U+%04lx", (unsigned long) cgm->u->utf32ch);
                cp = cgm->u->utf8str;
                while (*cp) {
                    char bufd[BUFSZ];
                    Sprintf(bufd, "%c<%d>", sep, (int) *cp);
                    Strcat(bufu, bufd);
                    cp++;
                    sep = ' ';
                }
            }
#endif
            any.a_int = glyphnum + 1; /* avoid 0 */
            Snprintf(buf, sizeof buf, "%s%s%s%s", bufa, bufb, bufc, bufu);
            add_menu(win, &nul_glyphinfo, &any, 0, 0, ATR_NONE, clr, buf,
                     MENU_ITEMFLAGS_NONE);
        }
    }
    return;
}

/*wizcmds.c*/
