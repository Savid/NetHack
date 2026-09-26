/* NetHack 5.0	rnd.c	$NHDT-Date: 1781973065 2026/06/20 16:31:05 $  $NHDT-Branch: NetHack-5.0 $:$NHDT-Revision: 1.41 $ */
/*      Copyright (c) 2004 by Robert Patrick Rankin               */
/* NetHack may be freely redistributed.  See license for details. */

#include "hack.h"

staticfn uint64 fnv1a64(const char *);
staticfn uint64 splitmix64(uint64);
staticfn uint64 keyed_mix(uint64, const char *, long, long);
staticfn uint64 seed_chain_end(void);
staticfn boolean seed_canonical(const char *, char *);
staticfn const char *seed_value(const char *, boolean, uint64 *, char *);
staticfn void seed_use(boolean, boolean, uint64, const char *);

#ifdef USE_ISAAC64
#include "isaac64.h"

staticfn int whichrng(int (*fn)(int));
staticfn int RND(int);
staticfn void set_random(unsigned long, int (*)(int));
staticfn void seed_isaac64_ctx(isaac64_ctx *, uint64);
staticfn int rng_entity_kind(void);
staticfn void rng_entity_substream(const char *, int);

/* seeded games (see below): the CORE states saved by nested streams; the
   rest of the seeded-game state is in gseed (decl.h) */
#define RNG_STREAM_MAX 32
static isaac64_ctx rng_stream_saved[RNG_STREAM_MAX];
static isaac64_ctx level_layout_saved;

#if 0
static isaac64_ctx rng_state;
#endif

struct rnglist_t {
    int (*fn)(int);
    boolean init;
    isaac64_ctx rng_state;
};

enum { CORE = 0, DISP = 1 };

static struct rnglist_t rnglist[] = {
    { rn2, FALSE, { 0 } },                      /* CORE */
    { rn2_on_display_rng, FALSE, { 0 } },       /* DISP */
};

staticfn int
whichrng(int (*fn)(int))
{
    int i;

    for (i = 0; i < SIZE(rnglist); ++i)
        if (rnglist[i].fn == fn)
            return i;
    return -1;
}

void
init_isaac64(unsigned long seed, int (*fn)(int))
{
    unsigned char new_rng_state[sizeof seed];
    unsigned i;
    int rngindx = whichrng(fn);

    if (rngindx < 0)
        panic("Bad rng function passed to init_isaac64().");

    for (i = 0; i < sizeof seed; i++) {
        new_rng_state[i] = (unsigned char) (seed & 0xFF);
        seed >>= 8;
    }
    isaac64_init(&rnglist[rngindx].rng_state, new_rng_state,
                 (int) sizeof seed);
}

staticfn int
RND(int x)
{
    /* count draws from a seeded level's layout stream; monsters and
       objects made during it are keyed by this count */
    if (gseed.level_active && !gseed.content_depth
        && gseed.stream_depth == gseed.layout_depth)
        ++gseed.layout_draws;
    return (isaac64_next_uint64(&rnglist[CORE].rng_state) % x);
}

/* 0 <= rn2(x) < x, but on a different sequence from the "main" rn2;
   used in cases where the answer doesn't affect gameplay and we don't
   want to give users easy control over the main RNG sequence. */
int
rn2_on_display_rng(int x)
{
    return (isaac64_next_uint64(&rnglist[DISP].rng_state) % x);
}

#else   /* USE_ISAAC64 */

/* "Rand()"s definition is determined by [OS]conf.h */
#if defined(UNIX) || defined(RANDOM)
#define RND(x) ((int) (Rand() % (long) (x)))
#else
/* Good luck: the bottom order bits are cyclic. */
#define RND(x) ((int) ((Rand() >> 3) % (x)))
#endif
int
rn2_on_display_rng(int x)
{
    static unsigned seed = 1;
    seed *= 2739110765;
    return (int) ((seed >> 16) % (unsigned) x);
}
#endif  /* USE_ISAAC64 */

/* 0 <= rn2(x) < x */
int
rn2(int x)
{
#if (NH_DEVEL_STATUS != NH_STATUS_RELEASED)
    if (x <= 0) {
        impossible("rn2(%d) attempted", x);
        return 0;
    }
    x = RND(x);
    return x;
#else
    return RND(x);
#endif
}

/* 0 <= rnl(x) < x; sometimes subtracting Luck;
   good luck approaches 0, bad luck approaches (x-1) */
int
rnl(int x)
{
    int i, adjustment;

#if (NH_DEVEL_STATUS != NH_STATUS_RELEASED)
    if (x <= 0) {
        impossible("rnl(%d) attempted", x);
        return 0;
    }
#endif

    adjustment = Luck;
    if (x <= 15) {
        /* for small ranges, use Luck/3 (rounded away from 0);
           also guard against architecture-specific differences
           of integer division involving negative values */
        adjustment = (abs(adjustment) + 1) / 3 * sgn(adjustment);
        /*
         *       11..13 ->  4
         *        8..10 ->  3
         *        5.. 7 ->  2
         *        2.. 4 ->  1
         *       -1,0,1 ->  0 (no adjustment)
         *       -4..-2 -> -1
         *       -7..-5 -> -2
         *      -10..-8 -> -3
         *      -13..-11-> -4
         */
    }

    i = RND(x);
    if (adjustment && rn2(37 + abs(adjustment))) {
        i -= adjustment;
        if (i < 0)
            i = 0;
        else if (i >= x)
            i = x - 1;
    }
    return i;
}

/* 1 <= rnd(x) <= x */
int
rnd(int x)
{
#if (NH_DEVEL_STATUS != NH_STATUS_RELEASED)
    if (x <= 0) {
        impossible("rnd(%d) attempted", x);
        return 1;
    }
#endif
    x = RND(x) + 1;
    return x;
}

int
rnd_on_display_rng(int x)
{
    return rn2_on_display_rng(x) + 1;
}

/* d(N,X) == NdX == dX+dX+...+dX N times; n <= d(n,x) <= (n*x) */
int
d(int n, int x)
{
    int tmp = n;

#if (NH_DEVEL_STATUS != NH_STATUS_RELEASED)
    if (x < 0 || n < 0 || (x == 0 && n != 0)) {
        impossible("d(%d,%d) attempted", n, x);
        return 1;
    }
#endif
    while (n--)
        tmp += RND(x);
    return tmp; /* Alea iacta est. -- J.C. */
}

/* 1 <= rne(x) <= max(u.ulevel/3,5) */
int
rne(int x)
{
    int tmp, utmp;

    utmp = (u.ulevel < 15) ? 5 : u.ulevel / 3;
    tmp = 1;
    while (tmp < utmp && !rn2(x))
        tmp++;
    return tmp;

    /* was:
     *  tmp = 1;
     *  while (!rn2(x))
     *    tmp++;
     *  return min(tmp, (u.ulevel < 15) ? 5 : u.ulevel / 3);
     * which is clearer but less efficient and stands a vanishingly
     * small chance of overflowing tmp
     */
}

/* rnz: everyone's favorite! */
int
rnz(int i)
{
    long x = (long) i;
    long tmp = 1000L;

    tmp += rn2(1000);
    tmp *= rne(4);
    if (rn2(2)) {
        x *= tmp;
        x /= 1000;
    } else {
        x *= 1000;
        x /= tmp;
    }
    return (int) x;
}

/* Sets the seed for the random number generator */
#ifdef USE_ISAAC64

staticfn void
set_random(unsigned long seed,
           int (*fn)(int))
{
    init_isaac64(seed, fn);
}

#else /* USE_ISAAC64 */

/*ARGSUSED*/
staticfn void
set_random(unsigned long seed,
           int (*fn)(int) UNUSED)
{
    /*
     * The types are different enough here that sweeping the different
     * routine names into one via #defines is even more confusing.
     */
# ifdef RANDOM /* srandom() from sys/share/random.c */
    srandom((unsigned int) seed);
# else
#  if defined(__APPLE__) || defined(BSD) || defined(LINUX) \
    || defined(ULTRIX) || defined(CYGWIN32) /* system srandom() */
#   if defined(BSD) && !defined(POSIX_TYPES) && defined(SUNOS4)
    (void)
#   endif
        srandom((int) seed);
#  else
#   ifdef UNIX /* system srand48() */
    srand48((long) seed);
#   else       /* poor quality system routine */
    srand((int) seed);
#   endif
#  endif
# endif
}
#endif /* USE_ISAAC64 */

/* An appropriate version of this must always be provided in
   port-specific code somewhere. It returns a number suitable
   as seed for the random number generator */
extern unsigned long sys_random_seed(void);

/*
 * Seeded games (OPTIONS=seed:<number or text>).
 *
 * When a seed is set, every game started with it gets the same dungeon:
 * the same character, starting inventory, object identities, dungeon
 * structure, and the same map, monsters and objects on every level,
 * regardless of what the player did before arriving or the order in
 * which levels are visited.  Play itself (combat and so on) still uses
 * the normal, unpredictable rng.
 *
 * Each piece of generation gets its own random stream derived from the
 * seed.  A stream temporarily replaces the CORE rng's state, so
 * everything that calls rn2() et al (including Lua's nh.random) uses it
 * without further changes, and the gameplay state is put back
 * afterwards.
 *
 * While a level is being made, its layout comes from a stream for that
 * level, and every monster and object made along the way (makemon(),
 * mksobj() and friends, each with everything made as part of it) gets
 * a stream of its own, keyed by how far the layout had got when it was
 * made.  So anything that makes one monster or object come out
 * differently for one player (a genocided species, an artifact that
 * already exists) can't change any other monster or object, or the map.
 * A stand-in hero (see stand_in_hero_begin()) is used meanwhile so that
 * the hero's progress doesn't influence what gets generated.
 */

staticfn uint64
fnv1a64(const char *str)
{
    uint64 h = 0xcbf29ce484222325ULL;

    while (*str) {
        h ^= (uint64) (unsigned char) *str++;
        h *= 0x100000001b3ULL;
    }
    return h;
}

staticfn uint64
splitmix64(uint64 x)
{
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

/* a value derived from key and the name (tag, n1, n2) that doesn't give
   the key back: the key goes into the mix again after it, so undoing the
   mixing leaves the key on both sides of the equation, and finding it
   means guessing it */
staticfn uint64
keyed_mix(uint64 key, const char *tag, long n1, long n2)
{
    uint64 h = fnv1a64(tag);

    h = splitmix64(h ^ splitmix64((uint64) n1));
    h = splitmix64(h ^ splitmix64((uint64) n2 + 0x5eedULL));
    return splitmix64(splitmix64(key ^ h) ^ key);
}

/* derive an independent sub-seed for one stream of a seeded game */
uint64
nh_seed_for(const char *tag, long n1, long n2)
{
    return keyed_mix(gseed.seed, tag, n1, n2);
}

boolean
nh_seeded(void)
{
    return gseed.active;
}

const char *
nh_seed_str(void)
{
    return gseed.text;
}

/* rounds of the seed's key-stretching chain: enough that it takes a
   noticeable fraction of a second, so that checking a guess of the seed
   against what's derived from its end is slow (each round needs the
   previous one and the seed, so it can't be shortened or run backwards) */
#define SEED_CHAIN_ROUNDS (1L << 25)
/* what is derived from the end of the chain, each with its own constant:
   the digest shown for a hidden seed, and the key of the fingerprints */
#define SEED_CHAIN_DIGEST 0x6469676573742023ULL
#define SEED_CHAIN_LEVELHASH 0x6c766c6861736823ULL

/* the end of the seed's key-stretching chain, made once per game */
staticfn uint64
seed_chain_end(void)
{
    if (!gseed.chain_made) {
        uint64 h = nh_seed_for("chain", 0L, 0L);
        long i;

        for (i = 0; i < SEED_CHAIN_ROUNDS; i++)
            h = splitmix64(h ^ gseed.seed);
        gseed.chain = h;
        gseed.chain_made = TRUE;
    }
    return gseed.chain;
}

/* the seed as shown to players: the seed itself, or when the server set it,
   a short digest of it so that racers can tell they have the same seed
   without learning it */
const char *
nh_seed_display(boolean quoted)
{
    static char buf[SEEDSZ + 20];

    if (!gseed.hidden)
        Snprintf(buf, sizeof buf, quoted ? "\"%s\"" : "%s", gseed.text);
    else
        Sprintf(buf, "hidden#%08lx",
                (unsigned long) (splitmix64(seed_chain_end()
                                            ^ SEED_CHAIN_DIGEST)
                                 & 0xffffffffUL));
    return buf;
}

/* the value a level's fingerprint (#levelhash) starts from; keyed by the
   end of the seed's chain, so that the fingerprints shown can't be traced
   back to the seed any faster than the digest can (an unseeded game's
   fingerprints, shown only in debug mode, have no key) */
uint64
nh_levelhash_salt(int ledger)
{
    uint64 key = gseed.active ? splitmix64(seed_chain_end()
                                           ^ SEED_CHAIN_LEVELHASH)
                              : 0ULL;

    return keyed_mix(key, "levelhash", (long) ledger, 0L);
}

/* TRUE if the server set the seed (sysconf SEED) */
boolean
nh_seed_hidden(void)
{
    return gseed.hidden;
}

#ifdef USE_ISAAC64

/* seed an isaac64 context from exactly 8 bytes, independent of the
   platform's sizeof (long), so every build derives the same sequence */
staticfn void
seed_isaac64_ctx(isaac64_ctx *ctx, uint64 seed)
{
    unsigned char bytes[8];
    int i;

    for (i = 0; i < 8; i++) {
        bytes[i] = (unsigned char) (seed & 0xFF);
        seed >>= 8;
    }
    isaac64_init(ctx, bytes, 8);
}

/* switch CORE to the stream named by tag, n1, n2 */
void
rng_stream_begin(const char *tag, long n1, long n2)
{
    if (!gseed.active)
        return;
    if (gseed.stream_depth >= RNG_STREAM_MAX)
        panic("rng_stream_begin: too many nested streams (%s)", tag);
    rng_stream_saved[gseed.stream_depth++] = rnglist[CORE].rng_state;
    seed_isaac64_ctx(&rnglist[CORE].rng_state, nh_seed_for(tag, n1, n2));
}

/* put CORE back to what it was before the matching rng_stream_begin() */
void
rng_stream_end(void)
{
    if (!gseed.active)
        return;
    if (gseed.stream_depth <= 0) {
        impossible("rng_stream_end: no stream to end");
        return;
    }
    rnglist[CORE].rng_state = rng_stream_saved[--gseed.stream_depth];
}

/* start making a level: CORE becomes that level's layout stream */
void
rng_level_begin(int ledger)
{
    if (!gseed.active)
        return;
    if (gseed.level_active) {
        impossible("rng_level_begin: already making a level");
        return;
    }
    rng_stream_begin("layout", (long) ledger, 0L);
    seeded_fresh_species(TRUE);
    gseed.level_active = TRUE;
    gseed.level_ledger = ledger;
    gseed.layout_depth = gseed.stream_depth;
    gseed.layout_draws = 0L;
    gseed.picked_draws = -1L;
    gseed.content_depth = 0;
    gseed.key_draws = 0L;
    gseed.key_sub[LVL_RNG_MONSTERS] = gseed.key_sub[LVL_RNG_OBJECTS] = 0L;
}

void
rng_level_end(void)
{
    if (!gseed.level_active)
        return;
    if (gseed.content_depth) {
        impossible("rng_level_end: monster or object still being made");
        gseed.content_depth = 0;
    }
    gseed.level_active = FALSE;
    seeded_fresh_species(FALSE);
    rng_stream_end();
}

/* called on entry to makemon(), mksobj() and friends; while a seeded
   level is being made, the monster or object (with everything made as
   part of it, such as a monster's inventory) gets a stream of its own */
void
rng_content_enter(int which)
{
    if (!gseed.level_active)
        return;
    if (gseed.content_depth++ > 0) {
        /* nested (a monster's inventory, a box's contents, a figurine's
           species): same stream, but made as what it is, so an object
           that is part of a monster is still made as an object */
        if (gseed.content_depth <= SIZE(gseed.which_stack))
            gseed.which_stack[gseed.content_depth - 1] = gseed.content_which;
        if (which != LVL_RNG_INHERIT)
            gseed.content_which = which;
        return;
    }
    /* (entered to pick a species, e.g. rndmonst_adj(), for the monster
       made next) */
    gseed.pick_only = (which == LVL_RNG_INHERIT);
    if (which == LVL_RNG_INHERIT)
        which = LVL_RNG_MONSTERS;
    /* several monsters or objects can be made at the same point of the
       layout; number them from there (so a stream that is only sometimes
       used, e.g. depending on whether a monster is in the way, must be
       entered every time) */
    if (gseed.key_draws != gseed.layout_draws) {
        gseed.key_draws = gseed.layout_draws;
        gseed.key_sub[LVL_RNG_MONSTERS] = 0L;
        gseed.key_sub[LVL_RNG_OBJECTS] = 0L;
    }
    gseed.content_which = which;
    /* (a species picked just before, at this point, is this monster's) */
    if (gseed.picked_draws != gseed.layout_draws)
        gseed.entity_picked = FALSE;
    level_layout_saved = rnglist[CORE].rng_state;
    gseed.entity_n1 = (long) gseed.level_ledger
                      + 4096L * gseed.key_sub[which]++;
    gseed.entity_n2 = gseed.layout_draws;
    gseed.place_count = 0;
    seed_isaac64_ctx(&rnglist[CORE].rng_state,
                     nh_seed_for((which == LVL_RNG_MONSTERS) ? "monsters"
                                                             : "objects",
                                 gseed.entity_n1, gseed.entity_n2));
}

/* the kind (LVL_RNG_MONSTERS or LVL_RNG_OBJECTS) of the outermost monster
   or object being made on a seeded level */
staticfn int
rng_entity_kind(void)
{
    return (gseed.content_depth == 1) ? gseed.content_which
                                      : gseed.which_stack[1];
}

/* sub-stream number n of the monster or object being made; the kind is
   part of the name, since a monster and an object made at the same point
   of the layout share their numbers */
staticfn void
rng_entity_substream(const char *tag, int n)
{
    char buf[BUFSZ];

    Snprintf(buf, sizeof buf, "%s:%s:%d", tag,
             (rng_entity_kind() == LVL_RNG_MONSTERS) ? "monster" : "object",
             n);
    rng_stream_begin(buf, gseed.entity_n1, gseed.entity_n2);
}

/* while a seeded level is being made, placing a monster or object (which
   can take a varying number of tries, depending on what's already there)
   uses a random stream of its own, so that where it lands can't change
   anything else about it */
boolean
rng_placement_begin(void)
{
    if (!gseed.level_active || !gseed.content_depth)
        return FALSE;
    rng_entity_substream("placement", gseed.place_count++);
    return TRUE;
}

void
rng_placement_end(boolean begun)
{
    if (begun)
        rng_stream_end();
}

/* while a seeded level is being made, part number n of the monster or
   object being made (e.g. a member of a monster's group) gets a stream of
   its own, so that whether an earlier part could be made doesn't change
   it; end it with rng_placement_end() */
boolean
rng_part_begin(const char *tag, int n)
{
    if (!gseed.level_active || !gseed.content_depth)
        return FALSE;
    rng_entity_substream(tag, n);
    return TRUE;
}

void
rng_content_leave(void)
{
    if (!gseed.level_active)
        return;
    if (gseed.content_depth <= 0) {
        impossible("rng_content_leave: not making a monster or object");
        return;
    }
    if (--gseed.content_depth > 0) {
        if (gseed.content_depth < SIZE(gseed.which_stack))
            gseed.content_which = gseed.which_stack[gseed.content_depth];
        return;
    }
    rnglist[CORE].rng_state = level_layout_saved;
    /* a species picked for this monster isn't the next one's */
    if (!gseed.pick_only)
        gseed.picked_draws = -1L;
}

/* a species has just been picked by how difficult the level is (so it can
   come out otherwise for a hero wearing a ring of aggravate monster or
   carrying the Amulet); noted for the monster being made on a seeded level,
   for the history fuzzer (see seedfuzz_note_mon()) */
void
rng_species_picked(void)
{
    if (gseed.level_active && gseed.content_depth > 0
        && gseed.content_which == LVL_RNG_MONSTERS) {
        gseed.entity_picked = TRUE;
        gseed.picked_draws = gseed.layout_draws;
    }
}

/* TRUE while a monster made on a seeded level had its species (or its
   leader's) picked by how difficult the level is, as part of making it or
   just before */
boolean
rng_species_was_picked(void)
{
    return gseed.level_active && gseed.entity_picked;
}

/* TRUE while a seeded level's layout or objects (including objects that
   are part of a monster) are being made; its monsters aren't included
   (they get tougher from aggravate monster and carrying the Amulet, as
   usual, without changing the rest of the level) */
boolean
rng_making_level_layout(void)
{
    return (gseed.level_active
            && !(gseed.content_depth > 0
                 && gseed.content_which == LVL_RNG_MONSTERS));
}

/* TRUE while any part of a seeded level is being made */
boolean
rng_making_level(void)
{
    return gseed.level_active;
}

/* TRUE while a monster (not one that is part of an object, such as a
   statue's) is being made on a seeded level, including what is made as
   part of it (its inventory, a hider's object to hide under) */
boolean
rng_making_monster_part(void)
{
    if (!gseed.level_active || !gseed.content_depth)
        return FALSE;
    return rng_entity_kind() == LVL_RNG_MONSTERS;
}

#else /* !USE_ISAAC64 */

void
rng_stream_begin(const char *tag UNUSED, long n1 UNUSED, long n2 UNUSED)
{
}

void
rng_stream_end(void)
{
}

void
rng_level_begin(int ledger UNUSED)
{
}

void
rng_level_end(void)
{
}

void
rng_content_enter(int which UNUSED)
{
}

void
rng_content_leave(void)
{
}

boolean
rng_making_level_layout(void)
{
    return FALSE;
}

boolean
rng_making_level(void)
{
    return FALSE;
}

boolean
rng_making_monster_part(void)
{
    return FALSE;
}

boolean
rng_placement_begin(void)
{
    return FALSE;
}

void
rng_placement_end(boolean begun UNUSED)
{
}

boolean
rng_part_begin(const char *tag UNUSED, int n UNUSED)
{
    return FALSE;
}

void
rng_species_picked(void)
{
}

boolean
rng_species_was_picked(void)
{
    return FALSE;
}

#endif /* ?USE_ISAAC64 */

/* seed text in its canonical form, whichever way it was given: spaces and
   tabs around it dropped, and each run of them inside it made one space
   (the options file and sysconf do this to a whole line, NETHACKOPTIONS
   doesn't); FALSE if that is longer than SEEDSZ - 1 characters */
staticfn boolean
seed_canonical(const char *val, char *out)
{
    int n = 0;
    boolean space = FALSE;

    for (; *val; val++) {
        if (*val == ' ' || *val == '\t') {
            space = (n > 0);
            continue;
        }
        if (n + (space ? 2 : 1) > SEEDSZ - 1)
            return FALSE;
        if (space)
            out[n++] = ' ';
        space = FALSE;
        out[n++] = *val;
    }
    out[n] = '\0';
    return TRUE;
}

/* check (non-empty, canonical) seed text and work out the seed it stands
   for and the text kept for it (a number without its leading zeros, so
   that "0042" is "42"); server: it's the server's (sysconf SEED); returns
   Null if it's usable, otherwise why not */
staticfn const char *
seed_value(const char *text, boolean server, uint64 *seedp, char *out)
{
    const char *p;
    boolean numeric = TRUE;

    for (p = text; *p; p++) {
        /* keep the value safe to write into xlogfile, and to give as an
           option, which a comma would end */
        if ((unsigned char) *p < ' ' || *p == '\177' || *p == '='
            || *p == ',')
            return "value has an invalid character (a control character,"
                   " '=' or ',')";
        if (!digit(*p))
            numeric = FALSE;
    }
    /* what's shown for a server's hidden seed; a player's own seed shown
       like that would pass for one */
    if (!server && !strncmpi(text, "hidden#", 7))
        return "value can't start with \"hidden#\"";
    if (numeric) {
        while (text[0] == '0' && text[1])
            text++;
        numeric = (strlen(text) <= 19);
    }
    if (numeric) {
        *seedp = 0;
        for (p = text; *p; p++)
            *seedp = *seedp * 10 + (uint64) (*p - '0');
    } else {
        *seedp = fnv1a64(text);
    }
    Strcpy(out, text);
    return (const char *) 0;
}

/* make the current game seeded with seed and text, or unseeded */
staticfn void
seed_use(boolean seeded, boolean hidden, uint64 seed, const char *text)
{
    gseed.active = seeded;
    gseed.hidden = (seeded && hidden);
    gseed.seed = seeded ? seed : 0ULL;
    Strcpy(gseed.text, seeded ? text : "");
    gseed.chain_made = FALSE;
}

/* the seed option (OPTIONS=seed:<value>), the player's own seed: an
   all-digit value is used as a number, anything else is hashed, and an
   empty value ("seed:" or "!seed") turns seeding off; when sysconf sets
   the seed, the option is checked, then ignored */
boolean
nh_seed_option(const char *val)
{
    char canon[SEEDSZ], text[SEEDSZ];
    const char *why = (const char *) 0;
    uint64 seed = 0;

    text[0] = '\0';
    if (!seed_canonical(val, canon)) {
        config_error_add("seed: value is longer than %d characters",
                         SEEDSZ - 1);
        return FALSE;
    }
    if (*canon)
        why = seed_value(canon, FALSE, &seed, text);
#ifndef USE_ISAAC64
    if (!why && *canon)
        why = "seeded games need a build with USE_ISAAC64";
#endif
    if (why) {
        config_error_add("seed: %s", why);
        return FALSE;
    }
    Strcpy(gseed.option_value, text);
    if (gseed.server_seed) {
        gseed.option_ignored = (*text != '\0');
        return TRUE;
    }
    seed_use(*text != '\0', FALSE, seed, text);
    return TRUE;
}

/* sysconf SEED: the server's seed for every new game, hidden from the
   players; one that isn't valid is neither shown (every player would see
   it) nor replaced by the players' own seeds: no new game can be started
   (see nh_seed_new_game()) */
void
nh_set_server_seed(const char *val)
{
    char canon[SEEDSZ], text[SEEDSZ];
    uint64 seed = 0;

    gseed.server_seed = TRUE;
    gseed.server_seed_bad = (!seed_canonical(val, canon) || !*canon
                             || seed_value(canon, TRUE, &seed, text) != 0);
#ifndef USE_ISAAC64
    gseed.server_seed_bad = TRUE;
#endif
    if (gseed.server_seed_bad)
        seed_use(FALSE, FALSE, 0ULL, "");
    else
        seed_use(TRUE, TRUE, seed, text);
}

/* option processing is over: note the seed that a new game gets, so that
   a restore that fails partway can't leave another in its place */
void
nh_seed_options_done(void)
{
    gseed.config.active = gseed.active;
    gseed.config.hidden = gseed.hidden;
    gseed.config.seed = gseed.seed;
    Strcpy(gseed.config.text, gseed.text);
    gseed.config.option_ignored = gseed.option_ignored;
}

/* a new game: its seed is the one sysconf and the options set; FALSE if
   sysconf's SEED is invalid, when no new game may start */
boolean
nh_seed_new_game(void)
{
    if (gseed.server_seed_bad)
        return FALSE;
    seed_use(gseed.config.active, gseed.config.hidden, gseed.config.seed,
             gseed.config.text);
    gseed.option_ignored = gseed.config.option_ignored;
    return TRUE;
}

/* restoring a seeded game: the seed it was started with, and whether it
   was the server's hidden seed, are the ones that count, whatever the
   options or sysconf say now; FALSE if the saved seed isn't valid */
boolean
nh_restore_seed(const char *saved, boolean hidden)
{
#ifdef USE_ISAAC64
    char canon[SEEDSZ], text[SEEDSZ];
    uint64 seed = 0;

    if (!seed_canonical(saved, canon) || strcmp(canon, saved) || !*canon
        || seed_value(canon, hidden, &seed, text) || strcmp(text, saved))
        return FALSE;
    seed_use(TRUE, hidden, seed, text);
    /* (for a hidden seed, whether the option happens to match it isn't
       let on) */
    gseed.option_ignored = (*gseed.option_value
                            && (hidden || strcmp(gseed.option_value, text)));
    return TRUE;
#else
    nhUse(saved);
    nhUse(hidden);
    return FALSE;
#endif
}

/* restoring an unseeded game: it stays unseeded */
void
nh_restore_unseeded(void)
{
    seed_use(FALSE, FALSE, 0ULL, "");
    gseed.option_ignored = (*gseed.option_value != '\0');
}

/* the seed option's own value, for #saveoptions */
const char *
nh_seed_option_value(void)
{
    return gseed.option_value;
}

/* the seed generator version of the current game: that of the build it
   was started with, or the lowest one of any build that has restored it
   (from then on, levels not yet made no longer match other players') */
void
nh_set_game_seedver(int ver, boolean restoring)
{
    gseed.ver_changed = (restoring && ver != SEED_GEN_VERSION);
    gseed.game_ver = min(ver, SEED_GEN_VERSION);
}

int
nh_game_seedver(void)
{
    return gseed.game_ver;
}

/* TRUE if the restored game was started by a build with another version */
boolean
nh_seedver_changed(void)
{
    return gseed.ver_changed;
}

/* TRUE if the seed option was ignored: the server's seed took its place,
   or a restored game has a seed of its own */
boolean
nh_seed_option_ignored(void)
{
    return gseed.option_ignored;
}

/* ubirthday for the places where the game start time acts as a per-game
   random value (shopkeeper names, anthole monsters, glass gem prices, worn
   T-shirt and apron text, Hawaiian shirt designs); seeded games derive it
   from the seed instead */
time_t
gameplay_birthday(void)
{
    if (gseed.active)
        return (time_t) (nh_seed_for("birthday", 0L, 0L) & 0x3fffffffULL);
    return ubirthday;
}

/* stand-in hero used while a seeded level (or a wandering monster) is
   generated, so that the hero's experience level, alignment, Luck and
   position don't change which monsters and objects get made */

/* experience level of the stand-in hero: the depth, or anywhere in the
   endgame (the Elemental Planes and the Astral Plane, whose depth is 0 or
   less) that of a seasoned hero */
#define STAND_IN_ENDGAME_XL 20

/* properties the stand-in hero doesn't have: protection from shape
   changers (it changes how shapeshifters and mimics are made) and
   hallucination (messages made while generating draw random numbers) */
static const int stand_in_props[2] = { PROT_FROM_SHAPE_CHANGERS, HALLUC };

void
stand_in_hero_begin(void)
{
    int i;

    if (!gseed.active)
        return;
    if (gseed.stand_in_depth++ > 0)
        return;
    gseed.real_hero.ulevel = u.ulevel;
    gseed.real_hero.type = u.ualign.type;
    gseed.real_hero.record = u.ualign.record;
    gseed.real_hero.abuse = u.ualign.abuse;
    gseed.real_hero.uluck = u.uluck;
    gseed.real_hero.moreluck = u.moreluck;
    gseed.real_hero.ux = u.ux;
    gseed.real_hero.uy = u.uy;
    for (i = 0; i < SIZE(stand_in_props); i++) {
        struct prop *p = &u.uprops[stand_in_props[i]];

        gseed.real_hero.intrinsic[i] = p->intrinsic;
        gseed.real_hero.extrinsic[i] = p->extrinsic;
        p->intrinsic = p->extrinsic = 0L;
    }
    u.ulevel = In_endgame(&u.uz) ? STAND_IN_ENDGAME_XL
               : max(1, min(depth(&u.uz), MAXULEV));
    u.ualign.type = u.ualignbase[A_ORIGINAL];
    u.ualign.record = 10;
    u.ualign.abuse = 0;
    u.uluck = u.moreluck = 0;
    /* while a level is being made the hero isn't on it yet; the old
       position is from the previous level and would get in the way */
    if (rng_making_level())
        u.ux = u.uy = 0;
}

/* put the real hero back and forget any generation in progress; used when
   saving after a panic, which can happen in the middle of making a level */
void
seeded_gen_cancel(void)
{
    if (gseed.stand_in_depth > 0) {
        gseed.stand_in_depth = 1;
        stand_in_hero_end();
    }
#ifdef USE_ISAAC64
    gseed.level_active = FALSE;
    gseed.content_depth = 0;
    if (gseed.ignoring_gone > 0) {
        gseed.ignoring_gone = 1;
        seeded_fresh_species(FALSE);
    }
    if (gseed.stream_depth > 0) {
        rnglist[CORE].rng_state = rng_stream_saved[0];
        gseed.stream_depth = 0;
    }
#endif
}

void
stand_in_hero_end(void)
{
    int i;

    if (!gseed.active)
        return;
    if (gseed.stand_in_depth <= 0) {
        impossible("stand_in_hero_end: no stand-in hero");
        return;
    }
    if (--gseed.stand_in_depth > 0)
        return;
    u.ulevel = gseed.real_hero.ulevel;
    u.ualign.type = gseed.real_hero.type;
    u.ualign.record = gseed.real_hero.record;
    u.ualign.abuse = gseed.real_hero.abuse;
    u.uluck = gseed.real_hero.uluck;
    u.moreluck = gseed.real_hero.moreluck;
    u.ux = gseed.real_hero.ux;
    u.uy = gseed.real_hero.uy;
    for (i = 0; i < SIZE(stand_in_props); i++) {
        struct prop *p = &u.uprops[stand_in_props[i]];

        p->intrinsic = gseed.real_hero.intrinsic[i];
        p->extrinsic = gseed.real_hero.extrinsic[i];
    }
}

/*
 * Initializes the random number generator.
 * Only call once.
 */
void
init_random(int (*fn)(int))
{
    /* (a recorded game notes the seed; a replayed one uses the noted one;
       see files.c) */
    set_random(nhrec_seed(sys_random_seed()), fn);
}

/* Reshuffles the random number generator. */
void
reseed_random(int (*fn)(int))
{
   /* only reseed if we are certain that the seed generation is unguessable
    * by the players.  (A replayed game reseeds where the recorded one did,
    * whatever the system replaying it has; see files.c.) */
    if (nhrec_strong_seed(has_strong_rngseed))
        init_random(fn);
}

/* randomize the given list of numbers  0 <= i < count */
void
shuffle_int_array(int *indices, int count)
{
    int i, iswap, temp;

    for (i = count - 1; i > 0; i--) {
        if ((iswap = rn2(i + 1)) == i)
            continue;
        temp = indices[i];
        indices[i] = indices[iswap];
        indices[iswap] = temp;
    }
}

/*rnd.c*/
