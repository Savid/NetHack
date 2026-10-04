/* NetHack 5.0	feed.c */
/* NetHack may be freely redistributed.  See license for details. */

/*
 * The live feed: what is happening in a game, as it is played, one JSON
 * object per line, written to a file descriptor the game inherits.  With
 * NETHACK_FEED_FD=N in the environment an unprivileged tty game writes to
 * descriptor N (a pipe to a collector); without it nothing here does
 * anything.
 *
 * The feed only reads the game: it never changes its state and never
 * draws a random number, so a game played with it is the same game as
 * one played without it, and a recorded game (files.c) replays and
 * verifies either way.  Objects are named with gd.quietnaming and
 * gd.distantname set (objnam.c: no artifact becomes found, no boulder
 * flag is reset, a leash's monster is named by species, never through
 * hallucination) and their knowledge bits put back.  Monsters are named
 * from the tables (species, and a shopkeeper's own name), never through
 * x_monnam(): shkname() and priestname() consult hallucination whatever
 * the caller asks and draw random numbers, from the game's own RNG.
 * The game knows nothing of who reads the feed: no clock, no host, no
 * process ids.  The collector adds those.
 *
 * Every line has "k" (its kind), "t" (the turn, svm.moves) and "a" (the
 * action: a count of the times the game has come back for a command, see
 * feed_boundary()).  Kinds:
 *   hdr     once as a session starts: build, seed, character
 *   hero    at every action boundary, and while the game waits for a key
 *           when it changed: position, vital statistics, what the hero
 *           is in the middle of
 *   hero_x  when it changes: attributes, properties, conduct, skills...
 *   pos     whenever the hero's position changes (every square of a run)
 *   map     remembered glyphs that changed: [i, glyph, "ch", color, "what"]
 *   lvl     terrain that changed: [i, typ, flags, lit, horizontal]; or
 *           traps, engravings and stairs, whole, when one of them changed
 *   obj     floor objects added, changed or gone
 *   mon     monsters on the level added, changed or gone (with "inv"
 *           when a monster's inventory changed)
 *   inv     the hero's inventory, in full, when it changes
 *   disc    object types identified (or called something)
 *   msg     a message, as shown ("prompt": one kept out of the history)
 *   key     a key the game read
 *   ev      an event: livelog, level (from where, how, which trap),
 *           kill, death
 *   kf      a keyframe: everything needed to draw this game on this level
 *           without any earlier line (hero, hero_x, inv and disc are in
 *           it, not lines of their own); a level's first is "arrive", or
 *           "death" or "end" when the session ends as the hero arrives;
 *           its terrain is written against the level's layout
 *           (feed_terrain()), and its map, screen and view compactly
 *           (feed_map())
 *   chk     with NH_FEEDCHECK, hashes of the level as it really is, each
 *           time the feed has brought itself up to date at an action
 *           boundary or with something to write (feed_check())
 *   dump    the end-of-game dump, as the dumplog has it
 *   end     the session is over (saved, or the game ended)
 * Cells are numbered i = y * COLNO + x.
 *
 * A seeded level's layout (mklev.c) is what every game with the seed has
 * there: its terrain as made, and more.  A keyframe names it by its hash
 * ("layout") and gives only the runs of cells whose terrain differs from
 * it ("terr"); nethack --layouts writes every level's layout for the
 * seed, so that a reader can keep each one once.  With no layout
 * (unseeded, wizard mode) "layout" is null and "terr" differs from all
 * stone.
 *
 * The feed brings itself up to date at every action boundary and
 * whenever the game is about to wait for a key (a --More--, a prompt, a
 * menu, a line of text: feed_flush()), so what it has written is the
 * game as it stands while it waits: a snapshot of the process taken then
 * (a fork) starts from exactly the state the feed describes.  It can't
 * during naming (a prompt from inside the feed), between leaving a level
 * and arriving on the next, or in a panic (feed_settled()).  While it
 * waits, nothing is written unless something changed.
 *
 * A keyframe is written the next time the feed brings itself up to date
 * after one is asked for: on arriving on a level, after FEED_KF_EVERY
 * actions without one (NETHACK_FEED_KF_EVERY), and on SIGUSR1 (which a
 * collector sends when it wants one, for instance after its sandbox was
 * forked).  While the game waits for a key, a keyframe asked for by the
 * signal is written at once (feed_idle()), when the feed can.  A game
 * that ends, is saved or is hung up writes a last keyframe of the state
 * it ended in ("death", "end"; feed_last()) and nothing about its state
 * after that; there is none after a panic, a fatal error, a replay that
 * stops early, or a hangup in the middle of a line.  What
 * changed is always written before a keyframe, so a
 * keyframe on the level the game was already on is a checkpoint: folding
 * the lines before it gives exactly it.  Every "changed" test compares the
 * bytes that would be written (by hash), so no field can change unnoticed.
 *
 * While one action runs over many turns (running, travel, a repeated
 * command, an occupation, being helpless) the game doesn't come back for a
 * command; what changed is written once a turn meanwhile (feed_step()),
 * with the action's "a", so a viewer sees the monsters move as the hero
 * does.
 */

#include "hack.h"

#if defined(UNIX) && !defined(SFCTOOL)
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>

#define FEED_KEEN 20000L   /* a spell's full retention (KEEN, spell.c) */
#define FEED_KF_EVERY 500L /* actions between keyframes, at most (the
                              * default; NETHACK_FEED_KF_EVERY) */

struct feedbuf {
    char *buf;
    size_t len, siz;
};

struct feednest {
    struct feedbuf line;
    size_t body;
    struct feednest *prev;
};

/* the last written state of a monster or object, by id (h2: a monster's
   inventory) */
struct feedent {
    unsigned id;
    uint64 h, h2;
};

struct feedset {
    struct feedent *e;
    int n, siz;
};

static struct feed_state {
    boolean on;             /* writing to fd */
    boolean tried;          /* NETHACK_FEED_FD has been looked at */
    boolean started;        /* this session's hdr is out (feed_start()) */
    int fd;
    pid_t pid;              /* the process writing; */
    dev_t dev;              /* and the pipe it writes, */
    ino_t ino;              /* as fstat() has it */
    struct feedbuf out;     /* whole lines not yet written */
    struct feedbuf line;    /* the line being made */
    struct feednest *nest;  /* unfinished lines, innermost first */
    size_t body;            /* where the line's body starts */
    long a;                 /* actions */
    long kf_a;              /* the action of the last keyframe */
    long kf_every;          /* actions between keyframes, at most */
    boolean kf_want;        /* a keyframe at the next feed_sync() */
    boolean waiting;        /* waiting for a key */
    int naming;             /* depth of feed_naming_begin() */
    int canon;              /* depth of feed_canon_begin(), */
    boolean canon_forced;   /* ... and whether it rebuilt the glyph map */
    long canon_ts;          /* gg.glyph_reset_timestamp before that, */
    long canon_pl;          /* ... and gg.glyphmap_perlevel_flags */
    boolean have_lev;       /* the shadows describe level 'lev' */
    d_level lev;
    int glyph[COLNO][ROWNO]; /* remembered glyphs, as last written */
    int scr[COLNO][ROWNO];   /* the screen's glyphs, as last written */
    uchar vis[COLNO][ROWNO]; /* squares in sight, as last written */
    int terr[COLNO][ROWNO];  /* terrain, as last written */
    uint64 traps_h, inv_h, herox_h, hero_h;
    struct feedset mon, obj, tmp;
    uchar disc[NUM_OBJECTS]; /* 1: name known, 2: called something */
    unsigned disc_uh[NUM_OBJECTS]; /* ... and what, hashed */
    uchar disc_dirty[NUM_OBJECTS]; /* changed since the map was written */
    boolean any_disc_dirty;
    boolean have_pos;
    coordxy px, py;
    d_level plev;
    long step_t;             /* the turn last written mid-action */
    d_level llev;            /* the level the hero last left, */
    coordxy lx, ly;          /* where the hero left it from, */
    int ltrap;               /* the trap there that took the hero, if any */
    boolean ldug;            /* ... a hole the hero dug */
    boolean lstairs, lfall, lportal; /* how (goto_level()'s arguments) */
    boolean arrived;         /* the arrival has been written */
    boolean check;           /* NH_FEEDCHECK: "chk" lines */
    boolean final;           /* the last word on the game's state is out
                              * (feed_last()), or a save has begun */
} feed;

static volatile sig_atomic_t feed_signalled = 0;

staticfn void feed_sigusr1(int);
staticfn void feed_sigpipe(int);
staticfn boolean feed_open(void);
staticfn void feed_owner(void);
staticfn boolean feed_mine(void);
staticfn void fb_grow(struct feedbuf *, size_t);
staticfn void fb_put(const char *, size_t);
staticfn void fb_raw(const char *);
staticfn void fb_sep(void);
staticfn void fb_key(const char *);
staticfn void fb_int(const char *, long);
staticfn int fb_utf8len(const unsigned char *);
staticfn void fb_str(const char *, const char *);
staticfn void fb_chr(const char *, int);
staticfn void fb_open(const char *, char);
staticfn void fb_close(char);
staticfn boolean fb_nest(void);
staticfn void fb_unnest(boolean);
staticfn void fb_begin(const char *);
staticfn void fb_end(void);
staticfn uint64 fb_body_hash(void);
staticfn boolean fb_end_changed(uint64 *);
staticfn uint64 fb_hash(const char *, size_t, uint64);
staticfn void feed_write(void);
staticfn const char *feed_align(aligntyp);
staticfn int feed_defsym(int);
staticfn long feed_cond(void);
staticfn void feed_canon_begin(void);
staticfn void feed_canon_end(void);
staticfn void feed_glyphinfo(coordxy, coordxy, int, glyph_info *);
staticfn void feed_glyph(int);
staticfn int feed_capacity(void);
staticfn void feed_hero_body(void);
staticfn void feed_hero(boolean);
staticfn void feed_hero_x_fields(void);
staticfn void feed_hero_x(boolean);
staticfn uint64 feed_obj_hash(struct obj *);
staticfn void feed_obj_block(struct obj *, boolean);
staticfn void feed_objlist(const char *, struct obj *);
staticfn void feed_inv(boolean);
staticfn int feed_disc_items(boolean);
staticfn void feed_disc(void);
staticfn uint64 feed_mon_hash(struct monst *);
staticfn uint64 feed_minv_hash(struct monst *);
staticfn void feed_mon_names(struct monst *);
staticfn void feed_mon_block(struct monst *, int);
staticfn int feed_ent_cmp(const genericptr, const genericptr);
staticfn void feed_set_add(struct feedset *, unsigned, uint64, uint64);
staticfn void feed_set_sort(struct feedset *);
staticfn void feed_set_swap(struct feedset *);
staticfn struct monst *feed_mon_by_id(unsigned);
staticfn struct obj *feed_obj_by_id(unsigned);
staticfn void feed_mons(boolean);
staticfn void feed_objs(boolean);
staticfn void feed_screen(void);
staticfn void feed_view(void);
staticfn void feed_map(boolean);
staticfn const uint16 *feed_layout(void);
staticfn char feed_terr_chr(int, int);
staticfn void feed_terrain(boolean);
staticfn void feed_trap_tuple(struct trap *, const d_level *);
staticfn void feed_engr_tuple(struct engr *);
staticfn void feed_stair_tuple(stairway *);
staticfn void feed_room_tuple(struct mkroom *);
staticfn void feed_traps_engr(boolean);
staticfn void feed_level_id(void);
staticfn void feed_keyframe(const char *);
staticfn void feed_diffs(void);
staticfn void feed_ui_wrap(void);
staticfn void feed_menu_frames(void);
staticfn void feed_naming_begin(void);
staticfn void feed_naming_end(void);
staticfn boolean feed_changes(boolean);
staticfn void feed_sync(boolean);
staticfn boolean feed_settled(void);
staticfn void feed_last(const char *, const char *, const char *);
staticfn void feed_check(void);

/*ARGSUSED*/
staticfn void
feed_sigusr1(int sig UNUSED)
{
    feed_signalled = 1;
}

/* caught handlers reset on exec; SIG_IGN would leak to compressors */
/*ARGSUSED*/
staticfn void
feed_sigpipe(int sig UNUSED)
{
    return;
}

/* start writing if NETHACK_FEED_FD names an open descriptor */
staticfn boolean
feed_open(void)
{
    const char *s;
    int fd;

    if (feed.tried)
        return feed.on;
    feed.tried = TRUE;
    /* called before chdirx() can drop an installed game's privileges */
    if (getuid() != geteuid() || getgid() != getegid() || layout_dumping())
        return FALSE;
    if (!(s = nh_getenv("NETHACK_FEED_FD")) || !digit(*s))
        return FALSE;
    fd = atoi(s);
    if (fd < 0 || fcntl(fd, F_GETFD) < 0)
        return FALSE;
    feed.fd = fd;
    feed.on = TRUE;
    feed_owner();
    /* not inherited by programs the game runs (a compressor, a mail
       reader) */
    (void) fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC);
    feed.kf_every = FEED_KF_EVERY;
    if ((s = nh_getenv("NETHACK_FEED_KF_EVERY")) != 0 && atol(s) > 0)
        feed.kf_every = atol(s);
    feed.check = ((s = nh_getenv("NH_FEEDCHECK")) != 0 && *s);
    /* a reader that goes away ends the feed, not the game (feed_write) */
    (void) signal(SIGPIPE, feed_sigpipe);
    return TRUE;
}

/* note which process writes the feed, and to which pipe */
staticfn void
feed_owner(void)
{
    struct stat st;

    feed.pid = getpid();
    if (fstat(feed.fd, &st) == 0)
        feed.dev = st.st_dev, feed.ino = st.st_ino;
}

/* TRUE if this process may write the feed.  A child the game forked (to
   compress a file, say) shares its pipe: it mustn't write the game's
   queued lines a second time.  A game forked whole (a sandbox's VM, or
   the proof of concept's stand-in, which gives the child a pipe of its
   own on the same descriptor) is a writer in its own right */
staticfn boolean
feed_mine(void)
{
    struct stat st;

    if (getpid() == feed.pid)
        return TRUE;
    if (fstat(feed.fd, &st) == 0
        && (st.st_dev != feed.dev || st.st_ino != feed.ino)) {
        feed_owner();
        return TRUE;
    }
    feed.on = FALSE; /* (quietly: the game's own process still writes) */
    return FALSE;
}

/* ---------- a line of JSON ---------- */

staticfn void
fb_grow(struct feedbuf *b, size_t more)
{
    char *nb;
    size_t nsiz;

    if (b->len + more + 1 <= b->siz)
        return;
    nsiz = b->siz ? b->siz : 4096;
    while (nsiz < b->len + more + 1)
        nsiz *= 2;
    nb = (char *) alloc(nsiz);
    if (b->len)
        (void) memcpy((genericptr_t) nb, (genericptr_t) b->buf, b->len);
    if (b->buf)
        free((genericptr_t) b->buf);
    b->buf = nb;
    b->siz = nsiz;
}

staticfn void
fb_put(const char *s, size_t n)
{
    fb_grow(&feed.line, n);
    (void) memcpy((genericptr_t) (feed.line.buf + feed.line.len),
                  (genericptr_t) s, n);
    feed.line.len += n;
}

staticfn void
fb_raw(const char *s)
{
    fb_put(s, strlen(s));
}

/* a comma, unless this is the first thing in an object or array */
staticfn void
fb_sep(void)
{
    char c;

    if (!feed.line.len)
        return;
    c = feed.line.buf[feed.line.len - 1];
    if (c != '{' && c != '[' && c != ':')
        fb_put(",", 1);
}

staticfn void
fb_key(const char *k)
{
    fb_sep();
    if (k) {
        fb_put("\"", 1);
        fb_raw(k);
        fb_put("\":", 2);
    }
}

staticfn void
fb_int(const char *k, long v)
{
    char buf[40];

    fb_key(k);
    Sprintf(buf, "%ld", v);
    fb_raw(buf);
}

/* the length of a valid UTF-8 sequence, or zero for a legacy byte */
staticfn int
fb_utf8len(const unsigned char *p)
{
    int n, i;

    n = (*p >= 0xc2 && *p <= 0xdf) ? 2
        : (*p >= 0xe0 && *p <= 0xef) ? 3
          : (*p >= 0xf0 && *p <= 0xf4) ? 4 : 0;
    for (i = 1; i < n; i++)
        if (p[i] < 0x80 || p[i] > 0xbf)
            return 0; /* includes NUL, without reading beyond it */
    if (n && ((*p == 0xe0 && p[1] < 0xa0)
              || (*p == 0xed && p[1] >= 0xa0)
              || (*p == 0xf0 && p[1] < 0x90)
              || (*p == 0xf4 && p[1] >= 0x90)))
        return 0; /* overlong, surrogate, or beyond U+10FFFF */
    return n;
}

/* preserve UTF-8; escape controls and legacy non-ASCII bytes as \u00XX */
staticfn void
fb_str(const char *k, const char *s)
{
    char buf[8];
    const unsigned char *p;
    int n;

    fb_key(k);
    fb_put("\"", 1);
    for (p = (const unsigned char *) (s ? s : ""); *p; p++) {
        if (*p == '"' || *p == '\\') {
            buf[0] = '\\', buf[1] = (char) *p;
            fb_put(buf, 2);
        } else if (*p >= 0x80 && (n = fb_utf8len(p)) != 0) {
            fb_put((const char *) p, (size_t) n);
            p += n - 1;
        } else if (*p < 0x20 || *p >= 0x7f) {
            Sprintf(buf, "\\u%04x", (unsigned) *p);
            fb_put(buf, 6);
        } else {
            fb_put((const char *) p, 1);
        }
    }
    fb_put("\"", 1);
}

staticfn void
fb_chr(const char *k, int c)
{
    char buf[2];

    buf[0] = (char) c, buf[1] = '\0';
    fb_str(k, buf);
}

staticfn void
fb_open(const char *k, char c)
{
    fb_key(k);
    fb_put(&c, 1);
}

staticfn void
fb_close(char c)
{
    fb_put(&c, 1);
}

/* a line from the game (a message, a livelog entry, a kill) while the
   feed is making one of its own (naming an object can call impossible()):
   made in a separate buffer and queued, and the line being made carries on
   where it was (fb_unnest()) */
staticfn boolean
fb_nest(void)
{
    struct feednest *n;

    if (!feed.line.len)
        return FALSE;
    n = (struct feednest *) alloc(sizeof *n);
    n->line = feed.line;
    n->body = feed.body;
    n->prev = feed.nest;
    feed.nest = n;
    (void) memset((genericptr_t) &feed.line, 0, sizeof feed.line);
    return TRUE;
}

staticfn void
fb_unnest(boolean nested)
{
    struct feednest *n;

    if (!nested)
        return;
    n = feed.nest;
    if (feed.line.buf)
        free((genericptr_t) feed.line.buf);
    feed.line = n->line;
    feed.body = n->body;
    feed.nest = n->prev;
    free((genericptr_t) n);
}

/* start a line of kind 'kind' */
staticfn void
fb_begin(const char *kind)
{
    feed.line.len = 0;
    fb_open((char *) 0, '{');
    fb_str("k", kind);
    fb_int("t", svm.moves);
    fb_int("a", feed.a);
    feed.body = feed.line.len;
}

/* finish the line and queue it */
staticfn void
fb_end(void)
{
    fb_put("}\n", 2);
    fb_grow(&feed.out, feed.line.len);
    (void) memcpy((genericptr_t) (feed.out.buf + feed.out.len),
                  (genericptr_t) feed.line.buf, feed.line.len);
    feed.out.len += feed.line.len;
    feed.line.len = 0;
}

/* the hash of the line's body less its leading comma: the same bytes as
   when the same fields are embedded in a keyframe (see feed_hero_x()) */
staticfn uint64
fb_body_hash(void)
{
    return fb_hash(feed.line.buf + feed.body + 1,
                   feed.line.len - feed.body - 1, 0);
}

/* finish the line and queue it only if its body differs from the last
   one queued with the same hash; TRUE if queued */
staticfn boolean
fb_end_changed(uint64 *last)
{
    uint64 h = fb_body_hash();

    if (h == *last) {
        feed.line.len = 0;
        return FALSE;
    }
    *last = h;
    fb_end();
    return TRUE;
}

staticfn uint64
fb_hash(const char *s, size_t n, uint64 h)
{
    if (!h)
        h = 0xcbf29ce484222325ULL;
    while (n--)
        h ^= (uint64) (unsigned char) *s++, h *= 0x100000001b3ULL;
    return h;
}

/* write the queued lines; a reader that has gone away ends the feed.
   Nothing is written before this session's hdr (feed_start()), so that
   hdr is always its first line.  A reader that is slow makes the game
   wait (a blocking descriptor; or poll() on one that isn't) */
staticfn void
feed_write(void)
{
    size_t off = 0;
    ssize_t n;
    struct pollfd pfd;
    sigset_t blocked, oldmask;

    if (!feed.on || !feed.started || !feed_mine())
        return;
    /* unrecorded games can prompt or exit inside these handlers.  Their
       feed hooks must not resend, reallocate or clear a partially written
       buffer.  Deliver them once the write has finished instead. */
    (void) sigemptyset(&blocked);
    (void) sigaddset(&blocked, SIGINT);
    (void) sigaddset(&blocked, SIGHUP);
    if (sigprocmask(SIG_BLOCK, &blocked, &oldmask) < 0)
        return;
    while (feed.on && off < feed.out.len) {
        n = write(feed.fd, feed.out.buf + off, feed.out.len - off);
        if (n > 0) {
            off += (size_t) n;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            pfd.fd = feed.fd, pfd.events = POLLOUT, pfd.revents = 0;
            (void) poll(&pfd, 1, 1000);
        } else {
            feed.on = FALSE;
        }
    }
    feed.out.len = 0;
    (void) sigprocmask(SIG_SETMASK, &oldmask, (sigset_t *) 0);
}

/* ---------- pieces ---------- */

staticfn const char *
feed_align(aligntyp al)
{
    return (al == A_LAWFUL) ? "lawful" : (al == A_NEUTRAL) ? "neutral"
           : (al == A_CHAOTIC) ? "chaotic" : "unaligned";
}

/* the default character for a symbol index, whatever symset is loaded */
staticfn int
feed_defsym(int idx)
{
    if (idx < SYM_OFF_O)
        return defsyms[idx - SYM_OFF_P].sym;
    if (idx < SYM_OFF_M)
        return def_oc_syms[idx - SYM_OFF_O].sym;
    if (idx < SYM_OFF_W)
        return def_monsyms[idx - SYM_OFF_M].sym;
    if (idx < SYM_OFF_X)
        return def_warnsyms[idx - SYM_OFF_W].sym;
    switch (idx - SYM_OFF_X) {
    case SYM_BOULDER:
        return def_oc_syms[ROCK_CLASS].sym;
    case SYM_INVISIBLE:
        return DEF_INVISIBLE;
    default:
        return ' ';
    }
}

/* the status conditions, as BL_MASK_* bits, whatever the options */
staticfn long
feed_cond(void)
{
    long c = 0L;

    if (Blind)
        c |= BL_MASK_BLIND;
    if (Confusion)
        c |= BL_MASK_CONF;
    if (Deaf)
        c |= BL_MASK_DEAF;
    if (Flying)
        c |= BL_MASK_FLY;
    if (Hallucination)
        c |= BL_MASK_HALLU;
    if (Levitation)
        c |= BL_MASK_LEV;
    if (u.usteed)
        c |= BL_MASK_RIDE;
    if (Slimed)
        c |= BL_MASK_SLIME;
    if (Stoned)
        c |= BL_MASK_STONE;
    if (Strangled)
        c |= BL_MASK_STRNGL;
    if (Stunned)
        c |= BL_MASK_STUN;
    if (Underwater)
        c |= BL_MASK_SUBMERGED;
    if (Glib)
        c |= BL_MASK_SLIPPERY;
    if (Wounded_legs)
        c |= BL_MASK_WOUNDEDL;
    if (Sick && (u.usick_type & SICK_VOMITABLE))
        c |= BL_MASK_FOODPOIS;
    if (Sick && (u.usick_type & SICK_NONVOMITABLE))
        c |= BL_MASK_TERMILL;
    if (u.utrap)
        c |= (u.utraptype == TT_LAVA) ? BL_MASK_INLAVA
             : (u.utraptype == TT_BURIEDBALL) ? BL_MASK_TETHERED
               : BL_MASK_TRAPPED;
    if (u.ustuck)
        c |= (u.uswallow || u.ustuck->data->mlet != S_EEL) ? BL_MASK_HELD
                                                          : BL_MASK_GRAB;
    if (gm.multi < 0)
        c |= u.usleep ? BL_MASK_SLEEPING : BL_MASK_BUSY;
    return c;
}

/* the glyph map (display.c) holds each glyph's colour as the player's
   'color' option has it: NO_COLOR for everything when the option is off.
   The feed writes the game's own colours: while it looks at glyphs with
   the option off, the map is rebuilt as if it were on, and put back after
   (with the tty port's redraw timestamp, so nothing is redrawn).  Nested;
   feed_naming_begin() batches a whole sync */
staticfn void
feed_canon_begin(void)
{
    if (feed.canon++ || iflags.use_color)
        return;
    feed.canon_forced = TRUE;
    feed.canon_ts = gg.glyph_reset_timestamp;
    feed.canon_pl = gg.glyphmap_perlevel_flags;
    iflags.use_color = TRUE;
    reset_glyphmap(gm_nochange);
}

staticfn void
feed_canon_end(void)
{
    if (--feed.canon || !feed.canon_forced)
        return;
    feed.canon_forced = FALSE;
    iflags.use_color = FALSE;
    reset_glyphmap(gm_nochange);
    gg.glyph_reset_timestamp = feed.canon_ts;
    gg.glyphmap_perlevel_flags = feed.canon_pl;
}

/* a glyph's symbol and colour as the game defines them, whatever the
   player's options: colour as if 'color' were on, and no accessibility
   override */
staticfn void
feed_glyphinfo(coordxy x, coordxy y, int glyph, glyph_info *ginfo)
{
    feed_canon_begin();
    map_glyphinfo(x, y, glyph, MG_FLAG_NOOVERRIDE, ginfo);
    feed_canon_end();
}

/* how a remembered glyph looks: glyph, "ch", color, "what" (the same
   wherever it is on the map) */
staticfn void
feed_glyph(int glyph)
{
    glyph_info ginfo;
    int otyp, cm;
    char what[BUFSZ];
    const char *s;

    /* canonical metadata; the hero's positional styling is in hero */
    feed_glyphinfo(0, 0, glyph, &ginfo);
    what[0] = '\0';
    if (glyph_is_invisible(glyph)) {
        Strcpy(what, "remembered, unseen, creature");
    } else if (glyph_is_warning(glyph)) {
        Strcpy(what, def_warnsyms[glyph_to_warning(glyph)].explanation);
    } else if (glyph_is_monster(glyph)) {
        Strcpy(what, mons[glyph_to_mon(glyph)].pmnames[NEUTRAL]);
    } else if (glyph_is_body(glyph)) {
        cm = glyph_to_body_corpsenm(glyph);
        Snprintf(what, sizeof what, "%s corpse",
                 ismnum(cm) ? mons[cm].pmnames[NEUTRAL] : "unknown");
    } else if (glyph_is_statue(glyph)) {
        cm = glyph_to_statue_corpsenm(glyph);
        Snprintf(what, sizeof what, "statue of %s",
                 ismnum(cm) ? mons[cm].pmnames[NEUTRAL] : "something");
    } else if (glyph_is_object(glyph)) {
        otyp = glyph_to_obj(glyph);
        s = (objects[otyp].oc_name_known || !OBJ_DESCR(objects[otyp]))
                ? OBJ_NAME(objects[otyp]) : OBJ_DESCR(objects[otyp]);
        Strcpy(what, s ? s : def_oc_syms[(int) objects[otyp].oc_class].name);
    } else if (glyph_is_cmap_zap(glyph)) {
        static const char *const zaps[] = {
            "missile", "fire", "frost", "sleep", "death", "lightning",
            "poison gas", "acid"
        };

        Snprintf(what, sizeof what, "%s ray",
                 zaps[((glyph - GLYPH_ZAP_OFF) / 4) % SIZE(zaps)]);
    } else if (glyph_is_explosion(glyph)) {
        static const char *const expls[] = {
            "dark", "noxious", "muddy", "wet", "magical", "fiery", "frosty"
        };

        Snprintf(what, sizeof what, "%s explosion",
                 expls[((glyph - GLYPH_EXPLODE_OFF) / MAXEXPCHARS)
                       % SIZE(expls)]);
    } else if (glyph_is_swallow(glyph)) {
        Snprintf(what, sizeof what, "the inside of %s",
                 mons[(glyph - GLYPH_SWALLOW_OFF) >> 3].pmnames[NEUTRAL]);
    } else if (!glyph_is_unexplored(glyph) && !glyph_is_nothing(glyph)) {
        Strcpy(what, defsyms[glyph_to_cmap(glyph)].explanation);
    }
    fb_int((char *) 0, (long) glyph);
    fb_chr((char *) 0, feed_defsym(ginfo.gm.sym.symidx));
    fb_int((char *) 0, (long) ginfo.gm.sym.color);
    fb_str((char *) 0, what);
}

staticfn void
feed_hero_body(void)
{
    glyph_info ginfo;
    int glyph = glyph_at(u.ux, u.uy);

    fb_int("x", u.ux);
    fb_int("y", u.uy);
    feed_glyphinfo(u.ux, u.uy, glyph, &ginfo);
    fb_open("screen", '[');
    fb_int((char *) 0, glyph);
    fb_chr((char *) 0, feed_defsym(ginfo.gm.sym.symidx));
    fb_int((char *) 0, ginfo.gm.sym.color);
    fb_close(']');
    fb_int("dn", u.uz.dnum);
    fb_int("dl", u.uz.dlevel);
    fb_int("dep", depth(&u.uz));
    fb_int("hp", Upolyd ? u.mh : u.uhp);
    fb_int("hpmax", Upolyd ? u.mhmax : u.uhpmax);
    fb_int("uhp", u.uhp);
    fb_int("uhpmax", u.uhpmax);
    fb_int("pw", u.uen);
    fb_int("pwmax", u.uenmax);
    fb_int("ac", u.uac);
    fb_int("xl", u.ulevel);
    fb_int("exp", u.uexp);
    fb_int("gold", money_cnt(gi.invent));
    fb_int("hunger", u.uhunger);
    fb_int("hs", u.uhs);
    fb_int("luck", u.uluck + u.moreluck);
    fb_int("cap", feed_capacity());
    fb_int("cond", feed_cond());
    fb_int("poly", Upolyd ? u.umonnum : -1);
    /* what the hero is in the middle of: a count of moves still to make
       (running, a repeated command) or, below 0, of turns helpless; an
       occupation ("eating", "digging", "searching"...) */
    fb_int("multi", gm.multi);
    fb_str("occ", (go.occupation && go.occtxt) ? go.occtxt : "");
    /* held or engulfed by, and riding (monster ids) */
    fb_int("stuck", u.ustuck ? (long) u.ustuck->m_id : 0L);
    fb_int("swallowed", u.uswallow ? 1 : 0);
    fb_int("steed", u.usteed ? (long) u.usteed->m_id : 0L);
    /* where the hero is travelling to (the _ command), while travelling */
    if (svc.context.travel) {
        fb_open("travel", '[');
        fb_int((char *) 0, u.tx);
        fb_int((char *) 0, u.ty);
        fb_close(']');
    }
}

/* the hero's encumbrance as near_capacity() has it, without its side
   effects: weight_cap() recomputes which of levitation, flying and
   stealth block which (float_vs_flight()) and asks for the status line
   to be redrawn */
staticfn int
feed_capacity(void)
{
    long bfly = BFlying, blev = BLevitation, bste = BStealth;
    boolean botl = disp.botl;
    int cap = near_capacity();

    BFlying = bfly, BLevitation = blev, BStealth = bste;
    disp.botl = botl;
    return cap;
}

/* the hero: always at an action boundary; while the game waits for a key
   (waiting), only if it changed */
staticfn void
feed_hero(boolean waiting)
{
    fb_begin("hero");
    feed_hero_body();
    if (waiting) {
        (void) fb_end_changed(&feed.hero_h);
        return;
    }
    feed.hero_h = fb_body_hash();
    fb_end();
}

/* what changes less often: attributes, properties, conduct, skills */
staticfn void
feed_hero_x_fields(void)
{
    int i;
    struct u_conduct *uc = &u.uconduct;

    fb_str("name", svp.plname);
    fb_str("title", rank_of(u.ulevel, Role_switch, flags.female));
    fb_str("str", get_strength_str());
    fb_open("attrs", '[');
    for (i = 0; i < A_MAX; i++) {
        fb_open((char *) 0, '[');
        fb_int((char *) 0, ACURR(i));
        fb_int((char *) 0, ABASE(i));
        fb_int((char *) 0, ABON(i));
        fb_int((char *) 0, ATEMP(i));
        fb_int((char *) 0, AMAX(i));
        fb_close(']');
    }
    fb_close(']');
    fb_open("align", '[');
    fb_str((char *) 0, feed_align(u.ualign.type));
    fb_int((char *) 0, u.ualign.record);
    fb_close(']');
    fb_int("ugangr", u.ugangr);
    /* the prayer timeout, in steps (it falls every turn): 0 prayed out,
       then at most 100, 200, or more (safe to pray in major trouble below
       200, minor below 100, otherwise at 0) */
    fb_int("pray", !u.ublesscnt ? 0 : (u.ublesscnt <= 100) ? 100
                   : (u.ublesscnt <= 200) ? 200 : 300);
    /* properties: [prop, intrinsic, extrinsic, blocked]; intrinsic
       without its timeout's count (TIMEOUT when it has one), which
       changes every turn */
    fb_open("props", '[');
    for (i = 1; i <= LAST_PROP; i++) {
        struct prop *p = &u.uprops[i];

        if (!p->intrinsic && !p->extrinsic && !p->blocked)
            continue;
        fb_open((char *) 0, '[');
        fb_int((char *) 0, i);
        fb_int((char *) 0, (p->intrinsic & TIMEOUT)
                                ? ((p->intrinsic & ~TIMEOUT) | TIMEOUT)
                                : p->intrinsic);
        fb_int((char *) 0, p->extrinsic);
        fb_int((char *) 0, p->blocked);
        fb_close(']');
    }
    fb_close(']');
    fb_open("conduct", '{');
    fb_int("unvegetarian", uc->unvegetarian);
    fb_int("unvegan", uc->unvegan);
    fb_int("food", uc->food);
    fb_int("gnostic", uc->gnostic);
    fb_int("weaphit", uc->weaphit);
    fb_int("killer", uc->killer);
    fb_int("literate", uc->literate);
    fb_int("polypiles", uc->polypiles);
    fb_int("polyselfs", uc->polyselfs);
    fb_int("wishes", uc->wishes);
    fb_int("wisharti", uc->wisharti);
    fb_int("sokocheat", uc->sokocheat);
    fb_int("pets", uc->pets);
    fb_close('}');
    fb_open("achieve", '[');
    for (i = 0; i < SIZE(u.uachieved) && u.uachieved[i]; i++)
        fb_int((char *) 0, u.uachieved[i]);
    fb_close(']');
    /* skills that aren't restricted: [skill, level, max, advance] */
    fb_open("skills", '[');
    for (i = 0; i < P_NUM_SKILLS; i++) {
        if (P_RESTRICTED(i))
            continue;
        fb_open((char *) 0, '[');
        fb_int((char *) 0, i);
        fb_int((char *) 0, P_SKILL(i));
        fb_int((char *) 0, P_MAX_SKILL(i));
        fb_int((char *) 0, P_ADVANCE(i));
        fb_close(']');
    }
    fb_close(']');
    fb_int("slots", u.weapon_slots);
    /* spells: [otyp, "name", level, retention]; retention in steps of
       10% (as the spell menu shows it), not the count, which falls every
       turn */
    fb_open("spells", '[');
    for (i = 0; i < MAXSPELL && spellid(i) != NO_SPELL; i++) {
        fb_open((char *) 0, '[');
        fb_int((char *) 0, spellid(i));
        fb_str((char *) 0, OBJ_NAME(objects[spellid(i)]));
        fb_int((char *) 0, svs.spl_book[i].sp_lev);
        fb_int((char *) 0, ((long) spellknow(i) * 10L + FEED_KEEN - 1)
                                / FEED_KEEN * 10L);
        fb_close(']');
    }
    fb_close(']');
    /* the quest and the endgame: what the hero carries of the invocation
       items, and the milestones (u.uevent) */
    fb_open("have", '{');
    fb_int("amulet", u.uhave.amulet);
    fb_int("bell", u.uhave.bell);
    fb_int("book", u.uhave.book);
    fb_int("menorah", u.uhave.menorah);
    fb_int("questart", u.uhave.questart);
    fb_close('}');
    fb_open("events", '{');
    fb_int("minor_oracle", u.uevent.minor_oracle);
    fb_int("major_oracle", u.uevent.major_oracle);
    fb_int("read_tribute", u.uevent.read_tribute);
    fb_int("qcalled", u.uevent.qcalled);
    fb_int("qexpelled", u.uevent.qexpelled);
    fb_int("qcompleted", u.uevent.qcompleted);
    fb_int("uheard_tune", u.uevent.uheard_tune);
    fb_int("uopened_dbridge", u.uevent.uopened_dbridge);
    fb_int("invoked", u.uevent.invoked);
    fb_int("gehennom_entered", u.uevent.gehennom_entered);
    fb_int("uhand_of_elbereth", u.uevent.uhand_of_elbereth);
    fb_int("udemigod", u.uevent.udemigod);
    fb_int("uvibrated", u.uevent.uvibrated);
    fb_int("ascended", u.uevent.ascended);
    fb_close('}');
    fb_open("genocided", '[');
    for (i = LOW_PM; i < NUMMONS; i++)
        if (svm.mvitals[i].mvflags & G_GENOD)
            fb_int((char *) 0, i);
    fb_close(']');
}

/* hero_x as a line of its own if it changed; or, for a keyframe (embed),
   as "hero_x" in the line being made */
staticfn void
feed_hero_x(boolean embed)
{
    size_t start;

    if (embed) {
        fb_open("hero_x", '{');
        start = feed.line.len;
        feed_hero_x_fields();
        feed.herox_h = fb_hash(feed.line.buf + start,
                               feed.line.len - start, 0);
        fb_close('}');
        return;
    }
    fb_begin("hero_x");
    feed_hero_x_fields();
    (void) fb_end_changed(&feed.herox_h);
}

/* an object's state, for telling whether it changed: the hash of the
   block it would be written as, so no field can be missed */
staticfn uint64
feed_obj_hash(struct obj *o)
{
    size_t mark = feed.line.len;
    uint64 h;

    feed_obj_block(o, TRUE);
    h = fb_hash(feed.line.buf + mark, feed.line.len - mark, 0);
    feed.line.len = mark;
    return h;
}

/* an object: what the hero believes ("name") and what it is */
staticfn void
feed_obj_block(struct obj *o, boolean at)
{
    struct obj *c;
    char name[BUFSZ];
    unsigned known, dknown, bknown;

    fb_open((char *) 0, '{');
    fb_int("id", (long) o->o_id);
    if (at) {
        fb_int("x", o->ox);
        fb_int("y", o->oy);
    }
    fb_int("otyp", o->otyp);
    fb_chr("cls", def_oc_syms[(int) o->oclass].sym);
    fb_int("color", objects[o->otyp].oc_color);
    fb_int("quan", o->quan);
    if (o->invlet)
        fb_chr("let", o->invlet);
    /* named quietly (see the top of this file); and naming can still
       mark an object's blessedness known (doname() for a cleric): put
       the bits back */
    known = o->known, dknown = o->dknown, bknown = o->bknown;
    gd.distantname++, gd.quietnaming++;
    Strcpy(name, doname(o));
    gd.distantname--, gd.quietnaming--;
    o->known = known, o->dknown = dknown, o->bknown = bknown;
    fb_str("name", name);
    fb_str("true", OBJ_NAME(objects[o->otyp]) ? OBJ_NAME(objects[o->otyp])
                                              : "?");
    if (OBJ_DESCR(objects[o->otyp]))
        fb_str("appearance", OBJ_DESCR(objects[o->otyp]));
    if (ismnum(o->corpsenm) && (o->otyp == CORPSE || o->otyp == STATUE
                                || o->otyp == FIGURINE || o->otyp == EGG
                                || o->otyp == TIN))
        fb_str("of", mons[o->corpsenm].pmnames[NEUTRAL]);
    if (o->oartifact)
        fb_str("art", artiname(o->oartifact));
    fb_int("spe", o->spe);
    fb_int("bless", o->blessed ? 1 : o->cursed ? -1 : 0);
    fb_int("erode", o->oeroded);
    fb_int("erode2", o->oeroded2);
    fb_int("eproof", o->oerodeproof);
    fb_int("worn", (long) o->owornmask);
    fb_int("known", ((long) o->known << 0) | ((long) o->dknown << 1)
                        | ((long) o->bknown << 2) | ((long) o->rknown << 3)
                        | ((long) o->cknown << 4) | ((long) o->lknown << 5));
    if (o->unpaid)
        fb_int("unpaid", 1);
    /* the rest of what the object is, known to the hero or not */
    fb_int("wt", (long) o->owt);
    if (o->oeaten)
        fb_int("eaten", (long) o->oeaten);
    if (o->greased)
        fb_int("greased", 1);
    if (o->recharged)
        fb_int("recharged", o->recharged);
    if (o->lamplit)
        fb_int("lit", 1);
    if (Is_box(o) || o->otyp == ICE_BOX) {
        fb_int("locked", o->olocked);
        fb_int("broken", o->obroken);
        fb_int("trapped", o->otrapped);
        fb_int("tknown", o->tknown);
    } else if (o->oclass == WEAPON_CLASS && o->opoisoned) {
        fb_int("poisoned", 1);
    }
    if (o->cobj) {
        fb_open("contents", '[');
        for (c = o->cobj; c; c = c->nobj)
            feed_obj_block(c, FALSE);
        fb_close(']');
    }
    fb_close('}');
}

staticfn void
feed_objlist(const char *k, struct obj *list)
{
    struct obj *o;

    fb_open(k, '[');
    for (o = list; o; o = o->nobj)
        feed_obj_block(o, FALSE);
    fb_close(']');
}

/* the inventory, in full, in the game's own order, when it changed; or,
   for a keyframe (embed), as "inv" in the line being made */
staticfn void
feed_inv(boolean embed)
{
    size_t start;

    if (embed) {
        fb_open("inv", '{');
        start = feed.line.len;
        feed_objlist("items", gi.invent);
        feed.inv_h = fb_hash(feed.line.buf + start, feed.line.len - start,
                             0);
        fb_close('}');
        return;
    }
    fb_begin("inv");
    feed_objlist("items", gi.invent);
    (void) fb_end_changed(&feed.inv_h);
}

/* discoveries, as [otyp, "true name", "appearance", known, "called"],
   into the line being made: every type known or called (all), or those
   that changed since the last time (noted in disc_dirty for feed_map());
   -> how many */
staticfn int
feed_disc_items(boolean all)
{
    int i, any = 0;
    uchar d;
    unsigned uh;

    for (i = 0; i < NUM_OBJECTS; i++) {
        if (!OBJ_DESCR(objects[i]) || !OBJ_NAME(objects[i]))
            continue;
        d = (objects[i].oc_name_known ? 1 : 0)
            | (objects[i].oc_uname ? 2 : 0);
        /* (what it's called, too: a type can be called something else) */
        uh = objects[i].oc_uname
                 ? (unsigned) fb_hash(objects[i].oc_uname,
                                      strlen(objects[i].oc_uname), 0)
                 : 0U;
        if (all ? !d : (d == feed.disc[i] && uh == feed.disc_uh[i])) {
            feed.disc[i] = d, feed.disc_uh[i] = uh;
            continue;
        }
        if (!all && d != feed.disc[i])
            feed.disc_dirty[i] = 1, feed.any_disc_dirty = TRUE;
        feed.disc[i] = d, feed.disc_uh[i] = uh;
        fb_open((char *) 0, '[');
        fb_int((char *) 0, i);
        fb_str((char *) 0, OBJ_NAME(objects[i]));
        fb_str((char *) 0, OBJ_DESCR(objects[i]));
        fb_int((char *) 0, objects[i].oc_name_known);
        fb_str((char *) 0, objects[i].oc_uname ? objects[i].oc_uname : "");
        fb_close(']');
        any++;
    }
    return any;
}

/* object types identified (or called something) since the last time */
staticfn void
feed_disc(void)
{
    int any;

    fb_begin("disc");
    fb_open("items", '[');
    any = feed_disc_items(FALSE);
    fb_close(']');
    if (any)
        fb_end();
    else
        feed.line.len = 0;
}

/* what a monster is called, from the tables alone: "name" its species
   (as its gender has it), "given" a name given it, "shk" a shopkeeper's
   own name, "align" a priest's or minion's alignment (not x_monnam(); see
   the top of this file) */
staticfn void
feed_mon_names(struct monst *m)
{
    const char *s;

    fb_str("name", mon_pmname(m));
    if (has_mgivenname(m))
        fb_str("given", MGIVENNAME(m));
    if (m->isshk && has_eshk(m)) {
        s = ESHK(m)->shknam;
        fb_str("shk", letter(*s) ? s : s + 1);
    }
    if (m->ispriest && has_epri(m))
        fb_str("align", feed_align(EPRI(m)->shralign));
    else if (m->isminion && has_emin(m))
        fb_str("align", feed_align(EMIN(m)->min_align));
}

/* a monster's state, for telling whether it changed (as
   feed_obj_hash()) */
staticfn uint64
feed_mon_hash(struct monst *m)
{
    size_t mark = feed.line.len;
    uint64 h;

    feed_mon_block(m, 0);
    h = fb_hash(feed.line.buf + mark, feed.line.len - mark, 0);
    feed.line.len = mark;
    return h;
}

/* a monster's inventory, for telling whether it changed */
staticfn uint64
feed_minv_hash(struct monst *m)
{
    size_t mark = feed.line.len;
    uint64 h;

    if (!m->minvent)
        return 0;
    feed_objlist("inv", m->minvent);
    h = fb_hash(feed.line.buf + mark, feed.line.len - mark, 0);
    feed.line.len = mark;
    return h;
}

/* a monster, as it is (whether or not the hero can see it: "seen");
   inv 1: with its inventory if it has one (a keyframe), 2: with its
   inventory even if empty (it changed) */
staticfn void
feed_mon_block(struct monst *m, int inv)
{
    fb_open((char *) 0, '{');
    fb_int("id", (long) m->m_id);
    fb_int("mnum", monsndx(m->data));
    feed_mon_names(m);
    fb_chr("sym", def_monsyms[(int) m->data->mlet].sym);
    fb_int("color", m->data->mcolor);
    fb_int("x", m->mx);
    fb_int("y", m->my);
    fb_int("hp", m->mhp);
    fb_int("hpmax", m->mhpmax);
    fb_int("lev", m->m_lev);
    fb_int("peace", m->mpeaceful);
    fb_int("tame", m->mtame);
    fb_int("sleep", m->msleeping);
    fb_int("move", m->mcanmove);
    fb_int("invis", m->minvis);
    fb_int("hidden", m->mundetected);
    fb_int("flee", m->mflee);
    /* conditions */
    fb_int("female", m->female);
    fb_int("speed", m->mspeed);
    if (m->mcan)
        fb_int("cancelled", 1);
    if (m->mconf)
        fb_int("conf", 1);
    if (m->mstun)
        fb_int("stun", 1);
    if (!m->mcansee)
        fb_int("blind", 1);
    if (m->mfrozen)
        fb_int("frozen", 1); /* (not the count: it falls every turn) */
    if (m->mtrapped)
        fb_int("trapped", 1);
    if (m->meating)
        fb_int("eating", 1);
    if (m->mleashed)
        fb_int("leashed", 1);
    if (m->m_ap_type) {
        fb_int("ap", m->m_ap_type);
        fb_int("appear", (long) m->mappearance);
    }
    if (m->isshk)
        fb_int("shopkeeper", 1);
    if (m->ispriest)
        fb_int("priest", 1);
    if (m->isminion)
        fb_int("minion", 1);
    fb_int("seen", canspotmon(m) ? 1 : 0);
    /* unseen, but the hero is warned of it: the warning level shown */
    if (!canspotmon(m) && mon_warning(m))
        fb_int("warn", warning_of(m));
    if (inv == 2 || (inv == 1 && m->minvent))
        feed_objlist("inv", m->minvent);
    fb_close('}');
}

staticfn int
feed_ent_cmp(const genericptr a, const genericptr b)
{
    unsigned ia = ((const struct feedent *) a)->id,
             ib = ((const struct feedent *) b)->id;

    return (ia < ib) ? -1 : (ia > ib) ? 1 : 0;
}

staticfn void
feed_set_add(struct feedset *s, unsigned id, uint64 h, uint64 h2)
{
    struct feedent *ne;

    if (s->n == s->siz) {
        s->siz = s->siz ? s->siz * 2 : 256;
        ne = (struct feedent *) alloc((unsigned) (s->siz * sizeof *ne));
        if (s->n)
            (void) memcpy((genericptr_t) ne, (genericptr_t) s->e,
                          s->n * sizeof *ne);
        if (s->e)
            free((genericptr_t) s->e);
        s->e = ne;
    }
    s->e[s->n].id = id;
    s->e[s->n].h = h;
    s->e[s->n].h2 = h2;
    s->n++;
}

staticfn void
feed_set_sort(struct feedset *s)
{
    if (s->n > 1)
        qsort((genericptr_t) s->e, (size_t) s->n, sizeof *s->e,
              feed_ent_cmp);
}

/* feed.tmp becomes the set; the old set is kept for reuse */
staticfn void
feed_set_swap(struct feedset *s)
{
    struct feedset t = *s;

    *s = feed.tmp;
    feed.tmp = t;
    feed.tmp.n = 0;
}

staticfn struct monst *
feed_mon_by_id(unsigned id)
{
    struct monst *m;

    for (m = fmon; m; m = m->nmon)
        if (m->m_id == id && !DEADMONSTER(m))
            return m;
    return (struct monst *) 0;
}

staticfn struct obj *
feed_obj_by_id(unsigned id)
{
    struct obj *o;

    for (o = fobj; o; o = o->nobj)
        if (o->o_id == id)
            return o;
    return (struct obj *) 0;
}

/* monsters on the level: in full (keyframe), or those added, changed or
   gone since the last time */
staticfn void
feed_mons(boolean full)
{
    struct monst *m;
    int i, j, any = 0;
    boolean known;
    struct feedset *old = &feed.mon, *cur = &feed.tmp;

    cur->n = 0;
    for (m = fmon; m; m = m->nmon)
        if (!DEADMONSTER(m))
            feed_set_add(cur, m->m_id, feed_mon_hash(m), feed_minv_hash(m));
    feed_set_sort(cur);
    if (full) {
        feed_set_swap(old);
        return;
    }
    fb_begin("mon");
    /* added or changed; with "inv" when the monster is new or its
       inventory changed (and without it otherwise: it is as it was) */
    fb_open("upd", '[');
    for (i = j = 0; i < cur->n; i++) {
        while (j < old->n && old->e[j].id < cur->e[i].id)
            j++;
        known = (j < old->n && old->e[j].id == cur->e[i].id);
        if (known && old->e[j].h == cur->e[i].h
            && old->e[j].h2 == cur->e[i].h2)
            continue;
        if ((m = feed_mon_by_id(cur->e[i].id)) != 0)
            feed_mon_block(m, (!known || old->e[j].h2 != cur->e[i].h2)
                                  ? 2 : 0),
                any++;
    }
    fb_close(']');
    /* gone */
    fb_open("rm", '[');
    for (i = j = 0; i < old->n; i++) {
        while (j < cur->n && cur->e[j].id < old->e[i].id)
            j++;
        if (j < cur->n && cur->e[j].id == old->e[i].id)
            continue;
        fb_int((char *) 0, (long) old->e[i].id), any++;
    }
    fb_close(']');
    if (any)
        fb_end();
    else
        feed.line.len = 0;
    feed_set_swap(old);
}

/* floor objects, as feed_mons() */
staticfn void
feed_objs(boolean full)
{
    struct obj *o;
    int i, j, any = 0;
    struct feedset *old = &feed.obj, *cur = &feed.tmp;

    cur->n = 0;
    for (o = fobj; o; o = o->nobj)
        feed_set_add(cur, o->o_id, feed_obj_hash(o), 0);
    feed_set_sort(cur);
    if (full) {
        feed_set_swap(old);
        return;
    }
    fb_begin("obj");
    fb_open("upd", '[');
    for (i = j = 0; i < cur->n; i++) {
        while (j < old->n && old->e[j].id < cur->e[i].id)
            j++;
        if (j < old->n && old->e[j].id == cur->e[i].id
            && old->e[j].h == cur->e[i].h)
            continue;
        if ((o = feed_obj_by_id(cur->e[i].id)) != 0)
            feed_obj_block(o, TRUE), any++;
    }
    fb_close(']');
    fb_open("rm", '[');
    for (i = j = 0; i < old->n; i++) {
        while (j < cur->n && cur->e[j].id < old->e[i].id)
            j++;
        if (j < cur->n && cur->e[j].id == old->e[i].id)
            continue;
        fb_int((char *) 0, (long) old->e[i].id), any++;
    }
    fb_close(']');
    if (any)
        fb_end();
    else
        feed.line.len = 0;
    feed_set_swap(old);
}

/* remembered glyphs that changed, as [i, glyph, "ch", color, "what"];
   or, for a keyframe (into the line being made): the remembered map
   ("g": each cell's glyph as "gw" characters, 1 or 2, high digit first,
   of the base64 alphabet, indexing "sym"), the cells where the screen
   differs from it ("scr": [i, glyph]), the runs of cells in sight ("vis":
   [i, n]), and how each glyph in either looks ("sym": [glyph, "ch",
   color, "what"], in order of first appearance in "g", then in "scr") */
staticfn void
feed_map(boolean full)
{
    static const char codes[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int x, y, i, n = 0, any = 0, glyph, layer, width, run;
    int seen[2 * COLNO * ROWNO], code[COLNO * ROWNO];
    char *g;

    if (full) {
        /* the palette: each glyph once, in order of first appearance in
           the remembered map, then on the screen */
        for (layer = 0; layer < 2; layer++)
            for (y = 0; y < ROWNO; y++)
                for (x = 0; x < COLNO; x++) {
                    if (layer)
                        glyph = feed.scr[x][y] = glyph_at(x, y);
                    else
                        glyph = feed.glyph[x][y] = levl[x][y].glyph;
                    for (i = 0; i < n && seen[i] != glyph; i++)
                        continue;
                    if (i == n)
                        seen[n++] = glyph;
                    if (!layer)
                        code[y * COLNO + x] = i;
                }
        width = (n <= 64) ? 1 : 2;
        g = (char *) alloc(COLNO * ROWNO * 2 + 1);
        for (i = 0; i < COLNO * ROWNO; i++) {
            if (width == 1) {
                g[i] = codes[code[i]];
            } else {
                g[2 * i] = codes[code[i] / 64];
                g[2 * i + 1] = codes[code[i] % 64];
            }
        }
        g[COLNO * ROWNO * width] = '\0';
        fb_str("g", g);
        free((genericptr_t) g);
        fb_int("gw", width);
        fb_open("scr", '[');
        for (y = 0; y < ROWNO; y++)
            for (x = 0; x < COLNO; x++)
                if (feed.scr[x][y] != feed.glyph[x][y]) {
                    fb_open((char *) 0, '[');
                    fb_int((char *) 0, (long) (y * COLNO + x));
                    fb_int((char *) 0, (long) feed.scr[x][y]);
                    fb_close(']');
                }
        fb_close(']');
        fb_open("vis", '[');
        for (i = 0; i < COLNO * ROWNO; i += run ? run : 1) {
            for (run = 0; i + run < COLNO * ROWNO; run++) {
                x = (i + run) % COLNO, y = (i + run) / COLNO;
                if (!(feed.vis[x][y] = cansee(x, y) ? 1 : 0))
                    break;
            }
            if (run) {
                fb_open((char *) 0, '[');
                fb_int((char *) 0, (long) i);
                fb_int((char *) 0, (long) run);
                fb_close(']');
            }
        }
        fb_close(']');
        fb_open("sym", '[');
        for (i = 0; i < n; i++) {
            fb_open((char *) 0, '[');
            feed_glyph(seen[i]);
            fb_close(']');
        }
        fb_close(']');
        return;
    }
    fb_begin("map");
    fb_int("dn", u.uz.dnum);
    fb_int("dl", u.uz.dlevel);
    fb_open("cells", '[');
    for (y = 0; y < ROWNO; y++)
        for (x = 0; x < COLNO; x++) {
            /* (a glyph for a type just identified looks the same but is
               described differently) */
            if ((glyph = levl[x][y].glyph) == feed.glyph[x][y]
                && !(feed.any_disc_dirty && glyph_is_object(glyph)
                     && feed.disc_dirty[glyph_to_obj(glyph)]))
                continue;
            feed.glyph[x][y] = glyph;
            fb_open((char *) 0, '[');
            fb_int((char *) 0, (long) (y * COLNO + x));
            feed_glyph(glyph);
            fb_close(']');
            any++;
        }
    fb_close(']');
    if (feed.any_disc_dirty) {
        (void) memset((genericptr_t) feed.disc_dirty, 0,
                      sizeof feed.disc_dirty);
        feed.any_disc_dirty = FALSE;
    }
    if (any)
        fb_end();
    else
        feed.line.len = 0;
}

/* what the map window shows now (gg.gbuf): the remembered glyphs, and the
   monsters, the hero, warnings, things detected, hallucinations, the
   inside of an engulfer; cells that changed, as [i, glyph, "ch", color,
   "what"].  Before feed_map(), which forgets which types were just
   identified */
staticfn void
feed_screen(void)
{
    int x, y, glyph, any = 0;

    fb_begin("scr");
    fb_int("dn", u.uz.dnum);
    fb_int("dl", u.uz.dlevel);
    fb_open("cells", '[');
    for (y = 0; y < ROWNO; y++)
        for (x = 0; x < COLNO; x++) {
            if ((glyph = glyph_at(x, y)) == feed.scr[x][y]
                && !(feed.any_disc_dirty && glyph_is_object(glyph)
                     && feed.disc_dirty[glyph_to_obj(glyph)]))
                continue;
            feed.scr[x][y] = glyph;
            fb_open((char *) 0, '[');
            fb_int((char *) 0, (long) (y * COLNO + x));
            feed_glyph(glyph);
            fb_close(']');
            any++;
        }
    fb_close(']');
    if (any)
        fb_end();
    else
        feed.line.len = 0;
}

/* which squares the hero can see right now (cansee()): those that came
   into sight ("on") and went out of it ("off"), as cell numbers */
staticfn void
feed_view(void)
{
    int x, y, any = 0, pass;
    uchar v;

    fb_begin("vis");
    fb_int("dn", u.uz.dnum);
    fb_int("dl", u.uz.dlevel);
    for (pass = 0; pass < 2; pass++) {
        fb_open(pass ? "off" : "on", '[');
        for (y = 0; y < ROWNO; y++)
            for (x = 0; x < COLNO; x++) {
                v = cansee(x, y) ? 1 : 0;
                if (v != feed.vis[x][y] && v == (pass ? 0 : 1))
                    fb_int((char *) 0, (long) (y * COLNO + x)), any++;
            }
        fb_close(']');
    }
    for (y = 0; y < ROWNO; y++)
        for (x = 0; x < COLNO; x++)
            feed.vis[x][y] = cansee(x, y) ? 1 : 0;
    if (any)
        fb_end();
    else
        feed.line.len = 0;
}

/* the layout a seeded level's keyframes are written against: its terrain
   as it was made (none in an unseeded game, nor in wizard mode, which
   makes levels differently) */
staticfn const uint16 *
feed_layout(void)
{
    if (!nh_seeded() || wizard)
        return (const uint16 *) 0;
    return level_layout(ledger_no(&u.uz));
}

/* a cell's terrain as a keyframe's "terr" and a "chk" line write it:
   'A' + typ, 'A' + flags, '0' or '1' (lit), '0' or '1' (horizontal) */
staticfn char
feed_terr_chr(int t, int field)
{
    return (field == 0) ? (char) ('A' + (t & 0xff))
           : (field == 1) ? (char) ('A' + ((t >> 8) & 0x1f))
             : (field == 2) ? (char) ('0' + ((t >> 13) & 1))
               : (char) ('0' + ((t >> 14) & 1));
}

/* terrain as it really is: [i, typ, flags, lit, horizontal] for cells
   that changed; or, for a keyframe, the level's layout ("layout": its hash
   in hex, or null) and the runs of cells whose terrain differs from it
   ("terr": [i, "typ", "flags", "lit", "horiz"], a character per cell, see
   feed_terr_chr()); with no layout, from all stone */
staticfn void
feed_terrain(boolean full)
{
    int x, y, any = 0, t, i, run, field;
    const uint16 *base;
    uint64 parts[NUM_LEVELHASH];
    char *s, hex[LAYOUT_HEXSZ];

    if (full) {
        /* (a level's terrain as made and its fingerprint are kept
           together, so the one is there whenever the other is) */
        if ((base = feed_layout()) != 0) {
            (void) level_fingerprint_at_creation(ledger_no(&u.uz), parts);
            fb_str("layout", layout_hex(parts[0], hex));
        } else {
            fb_key("layout");
            fb_raw("null");
        }
        s = (char *) alloc(COLNO * ROWNO + 1);
        fb_open("terr", '[');
        for (i = 0; i < COLNO * ROWNO; i += run ? run : 1) {
            for (run = 0; i + run < COLNO * ROWNO; run++) {
                x = (i + run) % COLNO, y = (i + run) / COLNO;
                t = feed.terr[x][y] = LAYOUT_TERR(x, y);
                if (t == (base ? (int) base[i + run] : 0))
                    break;
            }
            if (!run)
                continue;
            fb_open((char *) 0, '[');
            fb_int((char *) 0, (long) i);
            for (field = 0; field < 4; field++) {
                for (t = 0; t < run; t++)
                    s[t] = feed_terr_chr(feed.terr[(i + t) % COLNO]
                                                  [(i + t) / COLNO], field);
                s[run] = '\0';
                fb_str((char *) 0, s);
            }
            fb_close(']');
        }
        fb_close(']');
        free((genericptr_t) s);
        return;
    }
    fb_begin("lvl");
    fb_int("dn", u.uz.dnum);
    fb_int("dl", u.uz.dlevel);
    fb_open("cells", '[');
    for (y = 0; y < ROWNO; y++)
        for (x = 0; x < COLNO; x++) {
            t = LAYOUT_TERR(x, y);
            if (t == feed.terr[x][y])
                continue;
            feed.terr[x][y] = t;
            fb_open((char *) 0, '[');
            fb_int((char *) 0, (long) (y * COLNO + x));
            fb_int((char *) 0, levl[x][y].typ);
            fb_int((char *) 0, levl[x][y].flags);
            fb_int((char *) 0, levl[x][y].lit);
            fb_int((char *) 0, levl[x][y].horizontal);
            fb_close(']');
            any++;
        }
    fb_close(']');
    if (any)
        fb_end();
    else
        feed.line.len = 0;
}

/* a trap: [x, y, ttyp, seen, "name", to dn, to dl]; where a hole, trap
   door or portal leads (-1, -1: nowhere set) */
staticfn void
feed_trap_tuple(struct trap *t, const d_level *dst)
{
    fb_open((char *) 0, '[');
    fb_int((char *) 0, t->tx);
    fb_int((char *) 0, t->ty);
    fb_int((char *) 0, t->ttyp);
    fb_int((char *) 0, t->tseen);
    fb_str((char *) 0, trapname(t->ttyp, TRUE));
    fb_int((char *) 0, dst->dnum);
    fb_int((char *) 0, dst->dlevel);
    fb_close(']');
}

/* an engraving: [x, y, type, "text", read] */
staticfn void
feed_engr_tuple(struct engr *e)
{
    fb_open((char *) 0, '[');
    fb_int((char *) 0, e->engr_x);
    fb_int((char *) 0, e->engr_y);
    fb_int((char *) 0, e->engr_type);
    fb_str((char *) 0, e->engr_txt[actual_text]);
    fb_int((char *) 0, e->eread);
    fb_close(']');
}

/* stairs or a ladder: [x, y, up, ladder, to dn, to dl] */
staticfn void
feed_stair_tuple(stairway *st)
{
    fb_open((char *) 0, '[');
    fb_int((char *) 0, st->sx);
    fb_int((char *) 0, st->sy);
    fb_int((char *) 0, st->up);
    fb_int((char *) 0, st->isladder);
    fb_int((char *) 0, st->tolev.dnum);
    fb_int((char *) 0, st->tolev.dlevel);
    fb_close(']');
}

/* a room: [lx, ly, hx, hy, rtype, lit] */
staticfn void
feed_room_tuple(struct mkroom *r)
{
    fb_open((char *) 0, '[');
    fb_int((char *) 0, r->lx);
    fb_int((char *) 0, r->ly);
    fb_int((char *) 0, r->hx);
    fb_int((char *) 0, r->hy);
    fb_int((char *) 0, r->rtype);
    fb_int((char *) 0, r->rlit);
    fb_close(']');
}

/* traps, engravings, stairs and rooms, whole, when any of them changed
   (or into the line being made, for a keyframe) */
staticfn void
feed_traps_engr(boolean full)
{
    struct trap *t;
    struct engr *e;
    stairway *st;
    size_t start;
    uint64 h;
    int i;

    if (!full) {
        fb_begin("lvl");
        fb_int("dn", u.uz.dnum);
        fb_int("dl", u.uz.dlevel);
    }
    start = feed.line.len;
    fb_open("traps", '[');
    for (t = gf.ftrap; t; t = t->ntrap)
        feed_trap_tuple(t, &t->dst);
    fb_close(']');
    fb_open("engr", '[');
    for (e = head_engr; e; e = e->nxt_engr)
        feed_engr_tuple(e);
    fb_close(']');
    /* (the invocation makes stairs where there were none) */
    fb_open("stairs", '[');
    for (st = gs.stairs; st; st = st->next)
        feed_stair_tuple(st);
    fb_close(']');
    /* (a zoo becomes an ordinary room once entered, a scroll of light
       lights one) */
    fb_open("rooms", '[');
    for (i = 0; i < svn.nroom; i++)
        feed_room_tuple(&svr.rooms[i]);
    fb_close(']');
    /* (from the first key on, so a line's and a keyframe's agree) */
    while (start < feed.line.len && feed.line.buf[start] != '"')
        start++;
    h = fb_hash(feed.line.buf + start, feed.line.len - start, 0);
    if (full) {
        feed.traps_h = h;
        return;
    }
    if (h == feed.traps_h) {
        feed.line.len = 0;
        return;
    }
    feed.traps_h = h;
    fb_end();
}

/* which level this is, beyond its number: the special level it was made
   from ("oracle", "medusa", "castle"; "" for an ordinary one) and the
   flags that decide what can happen on it */
staticfn void
feed_level_id(void)
{
    s_level *sp = Is_special(&u.uz);

    fb_str("special", sp ? sp->proto : "");
    fb_open("lflags", '{');
    fb_int("hardfloor", svl.level.flags.hardfloor);
    fb_int("noteleport", svl.level.flags.noteleport);
    fb_int("maze", svl.level.flags.is_maze_lev);
    fb_int("nommap", svl.level.flags.nommap);
    fb_int("shortsighted", svl.level.flags.shortsighted);
    fb_int("graveyard", svl.level.flags.graveyard);
    fb_int("dig_down", Can_dig_down(&u.uz) ? 1 : 0);
    fb_int("fall_thru", Can_fall_thru(&u.uz) ? 1 : 0);
    fb_close('}');
}

/* a keyframe: all of this game's state that a viewer draws, on this
   level; the shadows start again from it.  why: "arrive" (the first word
   on a level: nothing else says what the level is, so a viewer needs it),
   "signal" (asked for: a fork waking, a collector's timer), "every" (the
   action count), "death" (the state the game ended in), "end" (the state
   a session ended in when the game didn't: a save, a hangup); only
   "arrive" carries anything the lines around it don't.  On a level the
   hero has only just reached, "death" or "end" stands in for "arrive". */
staticfn void
feed_keyframe(const char *why)
{
    struct monst *m;
    struct obj *o;
    size_t start;
    int i;

    fb_begin("kf");
    fb_str("why", why);
    fb_open("hero", '{');
    start = feed.line.len;
    feed_hero_body();
    feed.hero_h = fb_hash(feed.line.buf + start, feed.line.len - start, 0);
    fb_close('}');
    feed_hero_x(TRUE);
    feed_inv(TRUE);
    feed_menu_frames();
    fb_open("disc", '[');
    (void) feed_disc_items(TRUE);
    fb_close(']');
    /* monsters killed: [mnum, count, "name", "sym", color] */
    fb_open("vanq", '[');
    for (i = LOW_PM; i < NUMMONS; i++)
        if (svm.mvitals[i].died) {
            fb_open((char *) 0, '[');
            fb_int((char *) 0, i);
            fb_int((char *) 0, svm.mvitals[i].died);
            fb_str((char *) 0, mons[i].pmnames[NEUTRAL]);
            fb_chr((char *) 0, def_monsyms[(int) mons[i].mlet].sym);
            fb_int((char *) 0, mons[i].mcolor);
            fb_close(']');
        }
    fb_close(']');
    fb_open("level", '{');
    fb_int("dn", u.uz.dnum);
    fb_int("dl", u.uz.dlevel);
    fb_int("dep", depth(&u.uz));
    fb_str("dname", svd.dungeons[u.uz.dnum].dname);
    feed_level_id();
    feed_terrain(TRUE);
    feed_map(TRUE);
    feed_traps_engr(TRUE);
    fb_open("objects", '[');
    for (o = fobj; o; o = o->nobj)
        feed_obj_block(o, TRUE);
    fb_close(']');
    fb_open("monsters", '[');
    for (m = fmon; m; m = m->nmon)
        if (!DEADMONSTER(m))
            feed_mon_block(m, 1);
    fb_close(']');
    fb_close('}');
    fb_end();

    feed_objs(TRUE);
    feed_mons(TRUE);
    feed.have_lev = TRUE;
    assign_level(&feed.lev, &u.uz);
    feed.kf_a = feed.a;
    feed.kf_want = FALSE;
    feed_signalled = 0;
}

/* what changed on the level since the last time */
staticfn void
feed_diffs(void)
{
    feed_screen();
    feed_view();
    feed_map(FALSE);
    feed_terrain(FALSE);
    feed_traps_engr(FALSE);
    feed_objs(FALSE);
    feed_mons(FALSE);
}

/* around everything that names objects: the game's object-name buffers
   are put back as they were (the game may hold names it made in them, at
   a prompt or in the middle of an action) */
staticfn void
feed_naming_begin(void)
{
    if (!feed.naming++)
        obufs_keep(FALSE);
    feed_canon_begin();
}

staticfn void
feed_naming_end(void)
{
    feed_canon_end();
    if (!--feed.naming)
        obufs_keep(TRUE);
}

/* what changed since the last time, then the hero (see feed_hero());
   whether the hero is on the level the feed last described */
staticfn boolean
feed_changes(boolean waiting)
{
    boolean same = feed.have_lev && on_level(&feed.lev, &u.uz);

    feed_pos();
    if (same) {
        feed_disc();
        feed_diffs();
        feed_hero_x(FALSE);
        feed_inv(FALSE);
    }
    feed_hero(waiting);
    return same;
}

/* bring the feed up to date: what changed, then the hero, then a
   keyframe if one is wanted.  What changed is written even when a
   keyframe follows, so that a keyframe on the same level is a checkpoint:
   the lines before it add up to it.  On arriving on a level the keyframe
   follows the level event and supplies the new level's initial state.
   While the game waits for a key (waiting), nothing already written is
   written again: the hero only if it changed, "chk" only after something
   new. */
staticfn void
feed_sync(boolean waiting)
{
    size_t queued = feed.out.len;
    boolean same;

    feed_naming_begin();
    same = feed_changes(waiting);
    if (feed_signalled || feed.a - feed.kf_a >= feed.kf_every || !same)
        feed.kf_want = TRUE;
    if (feed.kf_want)
        feed_keyframe(!same ? "arrive" : feed_signalled ? "signal"
                      : feed.a - feed.kf_a >= feed.kf_every ? "every"
                      : "want");
    feed_naming_end();
    if (!waiting || feed.out.len != queued)
        feed_check();
}

/* whether the feed can bring itself up to date while the game waits for
   a key: not before this session's hdr (there is no hero yet), during
   naming or in the middle of a line (a prompt from inside the feed),
   between leaving a level and arriving on the next, after its last word
   on the game's state, or in a panic */
staticfn boolean
feed_settled(void)
{
    return (feed.on && feed.started && !feed.naming && !feed.line.len
            && !feed.nest && feed.arrived && !feed.final
            && !program_state.panicking);
}

/* NH_FEEDCHECK (a test hook; test/layouttest.py): once the feed has
   brought itself up to date, hashes of the level as it really is, which
   folding the feed must reproduce: FNV-1a 64 of the terrain (every
   cell's typ character, then every flags, lit and horiz one, as "terr"
   writes them), the remembered glyphs and the screen's (each in decimal
   followed by a comma) and the cells in sight ('0' or '1' each) */
staticfn void
feed_check(void)
{
    coordxy x, y;
    int field;
    uint64 h[4];
    char buf[20], hex[LAYOUT_HEXSZ];

    if (!feed.check)
        return;
    h[0] = h[1] = h[2] = h[3] = 0;
    for (field = 0; field < 4; field++)
        for (y = 0; y < ROWNO; y++)
            for (x = 0; x < COLNO; x++) {
                buf[0] = feed_terr_chr(LAYOUT_TERR(x, y), field);
                h[0] = fb_hash(buf, 1, h[0]);
            }
    for (y = 0; y < ROWNO; y++)
        for (x = 0; x < COLNO; x++) {
            Sprintf(buf, "%d,", levl[x][y].glyph);
            h[1] = fb_hash(buf, strlen(buf), h[1]);
            Sprintf(buf, "%d,", glyph_at(x, y));
            h[2] = fb_hash(buf, strlen(buf), h[2]);
            buf[0] = cansee(x, y) ? '1' : '0';
            h[3] = fb_hash(buf, 1, h[3]);
        }
    fb_begin("chk");
    fb_int("dn", u.uz.dnum);
    fb_int("dl", u.uz.dlevel);
    fb_str("terr", layout_hex(h[0], hex));
    fb_str("g", layout_hex(h[1], hex));
    fb_str("scr", layout_hex(h[2], hex));
    fb_str("vis", layout_hex(h[3], hex));
    fb_end();
}

/* the session's last word on the game's state: what changed, the death
   that ends it (how, cause; null when the session ends some other way),
   and the state it ends in, whole (why), so that a viewer can show a
   session that has ended without folding up to its end; a level change
   that hasn't said the hero arrived (dying, or hung up, on the way) says
   it first.  Nothing about the game's state is written after this. */
staticfn void
feed_last(const char *why, const char *how, const char *cause)
{
    if (!feed.arrived)
        feed_level_arrive();
    feed_naming_begin();
    (void) feed_changes(FALSE);
    if (how) {
        fb_begin("ev");
        fb_str("ev", "death");
        fb_str("how", how);
        fb_str("cause", cause);
        fb_str("killer", svk.killer.name);
        fb_int("dep", depth(&u.uz));
        fb_int("gold", money_cnt(gi.invent));
        fb_int("exp", u.uexp);
        fb_end();
    }
    feed_keyframe(why);
    feed_naming_end();
    feed_check();
    feed.final = TRUE;
}

/* ---------- the game's calls ---------- */

/* the program has started: take the feed's descriptor, if it has one, and
   the keyframe signal */
void
feed_init(void)
{
    struct sigaction sa;

    /* the keyframe signal is caught whether or not there is a feed: a
       collector that asks a game with no feed (or before it has opened
       it) mustn't kill it.  Interrupted reads restart: a key being read
       mustn't see the signal as the end of input */
    (void) memset((genericptr_t) &sa, 0, sizeof sa);
    sa.sa_handler = feed_sigusr1;
    sa.sa_flags = SA_RESTART;
    (void) sigemptyset(&sa.sa_mask);
    (void) sigaction(SIGUSR1, &sa, (struct sigaction *) 0);
    (void) feed_open();
}

/* a session has started (new game or restored): the header, and a
   keyframe at the first boundary */
void
feed_start(boolean restored)
{
    int i;
    char buf[BUFSZ], *tmp;
    size_t before, hlen;

    if (!feed_open())
        return;
    /* key and idle hooks currently belong to the tty port */
    if (strcmp(windowprocs.name, "tty")) {
        feed.on = FALSE;
        feed.out.len = 0;
        return;
    }
    fb_begin("hdr");
    fb_str("version", nomakedefs.version_string);
    fb_str("build", nomakedefs.git_sha ? nomakedefs.git_sha : "");
    fb_str("seed", nh_seeded() ? nh_seed_display(FALSE) : "");
    fb_int("seedver", SEED_GEN_VERSION);
    fb_int("restored", restored ? 1 : 0);
    fb_str("mode", wizard ? "wizard" : discover ? "explore" : "normal");
    fb_open("map", '{');
    fb_int("cols", COLNO);
    fb_int("rows", ROWNO);
    fb_close('}');
    for (i = 0; i < MAXOCLASSES && flags.inv_order[i]; i++)
        buf[i] = def_oc_syms[(int) flags.inv_order[i]].sym;
    buf[i] = '\0';
    fb_str("inv_order", buf);
    fb_open("character", '{');
    fb_str("name", svp.plname);
    fb_str("role", (flags.female && gu.urole.name.f) ? gu.urole.name.f
                                                     : gu.urole.name.m);
    fb_str("role_m", gu.urole.name.m);
    fb_str("role_f", gu.urole.name.f ? gu.urole.name.f : gu.urole.name.m);
    fb_str("code", gu.urole.filecode);
    fb_str("race", gu.urace.noun);
    fb_str("race_adj", gu.urace.adj);
    fb_str("race_coll", gu.urace.coll);
    fb_str("gender", flags.female ? "female" : "male");
    fb_str("align", feed_align(u.ualignbase[A_ORIGINAL]));
    fb_open("gods", '[');
    fb_str((char *) 0, align_gname(A_LAWFUL));
    fb_str((char *) 0, align_gname(A_NEUTRAL));
    fb_str((char *) 0, align_gname(A_CHAOTIC));
    fb_close(']');
    fb_str("god", u_gname());
    fb_open("quest", '{');
    fb_str("home", gu.urole.homebase);
    fb_str("goal", gu.urole.intermed);
    fb_str("leader", mons[gu.urole.ldrnum].pmnames[NEUTRAL]);
    fb_str("guard", mons[gu.urole.guardnum].pmnames[NEUTRAL]);
    fb_str("nemesis", mons[gu.urole.neminum].pmnames[NEUTRAL]);
    fb_str("artifact", artiname(gu.urole.questarti));
    fb_close('}');
    fb_open("ranks", '[');
    for (i = 1; i <= 30; i++)
        fb_str((char *) 0, rank_of(i, Role_switch, flags.female));
    fb_close(']');
    fb_open("ranks_other", '[');
    for (i = 1; i <= 30; i++)
        fb_str((char *) 0, rank_of(i, Role_switch, !flags.female));
    fb_close(']');
    /* what this role and race allow, and the race's attribute limits */
    fb_open("races_ok", '[');
    for (i = 0; races[i].noun; i++)
        if (validrace(flags.initrole, i))
            fb_str((char *) 0, races[i].noun);
    fb_close(']');
    fb_open("aligns_ok", '[');
    for (i = 0; i < ROLE_ALIGNS; i++)
        if (validalign(flags.initrole, flags.initrace, i))
            fb_str((char *) 0, aligns[i].adj);
    fb_close(']');
    fb_open("roles_ok", '[');
    for (i = 0; roles[i].name.m; i++)
        if (validrace(i, flags.initrace))
            fb_str((char *) 0, roles[i].name.m);
    fb_close(']');
    fb_open("attr_limits", '[');
    for (i = 0; i < A_MAX; i++) {
        fb_open((char *) 0, '[');
        fb_int((char *) 0, gu.urace.attrmin[i]);
        fb_int((char *) 0, gu.urace.attrmax[i]);
        fb_close(']');
    }
    fb_close(']');
    fb_close('}');
    before = feed.out.len;
    fb_end();
    /* the hdr goes first, before whatever the game said on its way here
       (the welcome, restoring, the livelog's "entered the dungeon") */
    hlen = feed.out.len - before;
    if (before) {
        tmp = (char *) alloc((unsigned) hlen);
        (void) memcpy((genericptr_t) tmp,
                      (genericptr_t) (feed.out.buf + before), hlen);
        (void) memmove((genericptr_t) (feed.out.buf + hlen),
                       (genericptr_t) feed.out.buf, before);
        (void) memcpy((genericptr_t) feed.out.buf, (genericptr_t) tmp,
                      hlen);
        free((genericptr_t) tmp);
    }
    feed.started = TRUE;
    feed.arrived = TRUE; /* (no level change under way) */
    feed_ui_wrap();
    feed.have_lev = FALSE;
    feed.have_pos = FALSE;
    feed.kf_want = TRUE;
    feed_write();
}

/* the hero's position, if it changed */
void
feed_pos(void)
{
    if (!feed.on || !u.ux)
        return;
    if (feed.have_pos && feed.px == u.ux && feed.py == u.uy
        && on_level(&feed.plev, &u.uz))
        return;
    feed.have_pos = TRUE;
    feed.px = u.ux, feed.py = u.uy;
    assign_level(&feed.plev, &u.uz);
    fb_begin("pos");
    fb_int("x", u.ux);
    fb_int("y", u.uy);
    fb_int("dn", u.uz.dnum);
    fb_int("dl", u.uz.dlevel);
    fb_end();
}

/* the top of the game's loop (moveloop_core()): the square the hero is
   on; and, once a turn while an action runs over several (running,
   travel, a repeated command, an occupation, being helpless), what
   changed, so a viewer sees the monsters move while the hero does */
void
feed_step(void)
{
    if (!feed.on || feed.final)
        return;
    feed_pos();
    if ((gm.multi || go.occupation) && svm.moves != feed.step_t) {
        feed.step_t = svm.moves;
        feed_sync(FALSE);
        feed_write();
    }
}

/* the game is about to read a command (moveloop_core(), gm.multi == 0):
   one action has ended and the next begins */
void
feed_boundary(void)
{
    if (!feed.on || feed.final)
        return;
    feed.a++;
    feed_sync(FALSE);
    feed_write();
}

/* the hero is leaving the level: what changed on it, before it goes, and
   where the hero left from (and the trap, if one took the hero) */
void
feed_level_leave(boolean at_stairs, boolean falling, boolean portal)
{
    struct trap *t;

    if (!feed.on || feed.final)
        return;
    feed.lstairs = at_stairs, feed.lfall = falling, feed.lportal = portal;
    feed.arrived = FALSE;
    t = t_at(u.ux, u.uy);
    /* (not u.uz0 on arrival: goto_level() has reset it by then) */
    assign_level(&feed.llev, &u.uz);
    feed.lx = u.ux, feed.ly = u.uy;
    feed.ltrap = (t && (is_hole(t->ttyp) || t->ttyp == LEVEL_TELEP
                        || t->ttyp == MAGIC_PORTAL)) ? t->ttyp : NO_TRAP;
    feed.ldug = (t && t->ttyp == HOLE && t->madeby_u);
    if (!feed.have_lev || !on_level(&feed.lev, &u.uz))
        return;
    feed_naming_begin();
    feed_pos();
    feed_diffs();
    feed_naming_end();
    feed_check();
}

/* the hero has arrived on a level (goto_level()) */
void
feed_level_arrive(void)
{
    stairway *st;
    const char *how;

    if (!feed.on || feed.arrived || feed.final)
        return;
    feed.arrived = TRUE;
    st = stairway_at(u.ux, u.uy);
    how = feed.lportal ? "portal" : feed.lfall ? "fall"
          : (feed.lstairs && st && st->isladder) ? "ladder"
            : feed.lstairs ? "stairs" : "other";
    fb_begin("ev");
    fb_str("ev", "level");
    fb_open("from", '{');
    fb_int("dn", feed.llev.dnum);
    fb_int("dl", feed.llev.dlevel);
    fb_int("x", feed.lx);
    fb_int("y", feed.ly);
    fb_close('}');
    fb_open("to", '{');
    fb_int("dn", u.uz.dnum);
    fb_int("dl", u.uz.dlevel);
    fb_int("dep", depth(&u.uz));
    fb_close('}');
    fb_str("how", how);
    /* the trap that took the hero ("trap door", "hole", "level
       teleporter", "magic portal"; "" for none: stairs, a scroll...) */
    fb_str("trap", feed.ltrap != NO_TRAP ? trapname(feed.ltrap, TRUE) : "");
    if (feed.ldug)
        fb_int("dug", 1);
    fb_open("land", '{');
    fb_int("x", u.ux);
    fb_int("y", u.uy);
    fb_close('}');
    feed_level_id();
    fb_end();
    feed.ltrap = NO_TRAP, feed.ldug = FALSE;
    feed_pos();
    feed.kf_want = TRUE;
}

/* a message, as the game shows it (pline()); one kept out of the
   message history (a prompt: "What do you want to eat? [fg or ?*]") has
   "prompt" */
void
feed_msg(const char *text, boolean prompt)
{
    boolean nested;

    if (!feed.on)
        return;
    nested = fb_nest();
    fb_begin("msg");
    fb_str("text", text);
    if (prompt)
        fb_int("prompt", 1);
    fb_end();
    fb_unnest(nested);
}

/* a key the game has read */
void
feed_key(int key)
{
    boolean nested;

    if (!feed.on)
        return;
    nested = fb_nest();
    fb_begin("key");
    fb_int("key", key);
    fb_end();
    fb_unnest(nested);
}

/* an entry for the livelog (livelog_printf()), whatever sysconf's LIVELOG
   lets through */
void
feed_livelog(long ll_type, const char *text)
{
    boolean nested;

    if (!feed.on)
        return;
    nested = fb_nest();
    fb_begin("ev");
    fb_str("ev", "livelog");
    fb_int("ll", ll_type);
    fb_str("text", text);
    fb_end();
    fb_unnest(nested);
}

/* a monster has died (mondead()); phase is when, not who killed it */
void
feed_kill(struct monst *m)
{
    boolean nested;

    if (!feed.on)
        return;
    nested = fb_nest();
    fb_begin("ev");
    fb_str("ev", "kill");
    fb_int("id", (long) m->m_id);
    fb_int("mnum", monsndx(m->data));
    feed_mon_names(m);
    fb_str("species", m->data->pmnames[NEUTRAL]);
    fb_chr("sym", def_monsyms[(int) m->data->mlet].sym);
    fb_int("color", m->data->mcolor);
    fb_int("x", m->mx);
    fb_int("y", m->my);
    fb_str("phase", svc.context.mon_moving ? "monster" : "hero");
    fb_end();
    fb_unnest(nested);
}

/* the game is over (really_done()): the state it ended in, and how (how:
   "died", "quit", "escaped"...; cause: "killed by a jackal") */
void
feed_death(const char *how, const char *cause)
{
    if (!feed.on || feed.final)
        return;
    feed_last("death", how, cause);
    feed_write();
}

/* the end-of-game dump, as written to the dumplog */
void
feed_dump(const char *text)
{
    if (!feed.on)
        return;
    fb_begin("dump");
    /* (by now the final score: the dump's "with N points") */
    fb_int("score", u.urexp);
    fb_str("text", text);
    fb_end();
    feed_write();
}

/* the game is about to wait for a key, at a command or in the middle of
   one (a --More--, a prompt, a menu, a line of text): bring the feed up
   to date and write it, so that a snapshot taken while the game waits
   starts from exactly what the feed has said.  What is queued goes out
   even when the feed can't describe the game (feed_settled()).  (Naming
   puts the game's name buffers back, so a prompt that holds a name it
   made is safe.) */
void
feed_flush(void)
{
    if (!feed.on)
        return;
    feed.waiting = TRUE;
    if (feed_settled())
        feed_sync(TRUE);
    feed_write();
}

/* while waiting for a key (a signal interrupted the wait, or it timed
   out): write a keyframe that the signal asked for */
void
feed_idle(void)
{
    if (!feed_signalled || !feed.waiting || !feed_settled())
        return;
    feed_sync(TRUE);
    feed_write();
}

/* a key has been read */
void
feed_got_key(int key)
{
    if (!feed.on)
        return;
    feed.waiting = FALSE;
    feed_key(key);
}

/* the session is ending and the game isn't (a save, a hangup): its last
   word (feed_last()), which answers a keyframe asked for and not yet
   written.  It is only made here, not written: it goes out with what
   follows (feed_end() at the latest), so a collector that doesn't read
   can't hold up a save under way. */
void
feed_final(void)
{
    if (!feed.on || feed.final)
        return;
    if (feed.started && !program_state.gameover && !program_state.panicking
        && !feed.naming && !feed.line.len && !feed.nest)
        feed_last("end", (const char *) 0, (const char *) 0);
    feed.final = TRUE;
}

/* the session is over (nh_terminate()) */
void
feed_end(const char *how)
{
    if (!feed.on)
        return;
    feed.started = TRUE; /* (whether or not there was a session) */
    fb_begin("end");
    fb_str("how", how);
    fb_end();
    feed_write();
    feed.on = FALSE;
}

/* only replay's next session inherits the descriptor across exec */
void
feed_replay_next(void)
{
    int fdflags;

    if (!feed.on || !feed_mine())
        return;
    feed_end("session");
    fdflags = fcntl(feed.fd, F_GETFD);
    if (fdflags >= 0)
        (void) fcntl(feed.fd, F_SETFD, fdflags & ~FD_CLOEXEC);
}

/* ---------- effects: what the game animates ---------- */

/*
 * Rays, beams, thrown and fired objects, explosions: the game draws them
 * with tmp_at(), a glyph at each square in turn, and they leave no state
 * behind.  feed_fx() follows the same calls (display.c) and writes each
 * effect when it ends: "fx" with "how" (beam, all, tether, flash, always:
 * tmp_at()'s styles), "glyph" [glyph, "ch", color, "what"], "path"
 * [[x, y]...], "change" [[at, glyph, "ch", color, "what"]] (the glyph
 * changed from path index 'at' on: a ray bouncing, an explosion's cells)
 * and "back" (a tethered weapon pulled back).  Effects nest (a ray from an
 * exploding wand); a few levels are followed.
 */

#define FX_DEPTH 4
#define FX_PATH (COLNO * 2)
#define FX_CHANGES 16

static struct feed_fx {
    int style, glyph, n, nchg;
    coordxy path[FX_PATH][2];
    int chg[FX_CHANGES][2]; /* [path index, glyph] */
} fxs[FX_DEPTH];
static int fxdepth = 0;

staticfn void feed_fx_line(struct feed_fx *, boolean);
staticfn void feed_fx_one(const char *, coordxy, coordxy, int);

staticfn void
feed_fx_line(struct feed_fx *f, boolean back)
{
    boolean nested;
    int i;

    nested = fb_nest();
    fb_begin("fx");
    fb_str("how", f->style == DISP_BEAM ? "beam" : f->style == DISP_ALL
                  ? "all" : f->style == DISP_TETHER ? "tether"
                  : f->style == DISP_FLASH ? "flash" : "always");
    fb_open("glyph", '[');
    feed_glyph(f->glyph);
    fb_close(']');
    fb_open("path", '[');
    for (i = 0; i < f->n; i++) {
        fb_open((char *) 0, '[');
        fb_int((char *) 0, f->path[i][0]);
        fb_int((char *) 0, f->path[i][1]);
        fb_close(']');
    }
    fb_close(']');
    if (f->nchg) {
        fb_open("change", '[');
        for (i = 0; i < f->nchg; i++) {
            fb_open((char *) 0, '[');
            fb_int((char *) 0, f->chg[i][0]);
            feed_glyph(f->chg[i][1]);
            fb_close(']');
        }
        fb_close(']');
    }
    if (back)
        fb_int("back", 1);
    fb_end();
    fb_unnest(nested);
}

/* tmp_at(x, y), as the game called it */
void
feed_fx(coordxy x, coordxy y)
{
    struct feed_fx *f;

    if (!feed.on || !feed.started)
        return;
    switch (x) {
    case DISP_BEAM:
    case DISP_ALL:
    case DISP_TETHER:
    case DISP_FLASH:
    case DISP_ALWAYS:
        if (fxdepth < FX_DEPTH) {
            f = &fxs[fxdepth];
            f->style = x, f->glyph = y, f->n = f->nchg = 0;
        }
        fxdepth++;
        return;
    case DISP_FREEMEM:
        fxdepth = 0;
        return;
    case DISP_CHANGE:
        if (fxdepth > 0 && fxdepth <= FX_DEPTH) {
            f = &fxs[fxdepth - 1];
            if (f->nchg < FX_CHANGES)
                f->chg[f->nchg][0] = f->n, f->chg[f->nchg++][1] = y;
        }
        return;
    case DISP_END:
        if (fxdepth > 0 && --fxdepth < FX_DEPTH)
            feed_fx_line(&fxs[fxdepth], y == BACKTRACK);
        return;
    default:
        if (fxdepth > 0 && fxdepth <= FX_DEPTH) {
            f = &fxs[fxdepth - 1];
            if (f->n < FX_PATH)
                f->path[f->n][0] = x, f->path[f->n++][1] = y;
        }
        return;
    }
}

/* an effect at one square: "shield" (shieldeff(): magic resistance's
   sparkle) or "flash" (flash_glyph_at()) */
staticfn void
feed_fx_one(const char *how, coordxy x, coordxy y, int glyph)
{
    boolean nested;

    if (!feed.on || !feed.started)
        return;
    nested = fb_nest();
    fb_begin("fx");
    fb_str("how", how);
    if (glyph != NO_GLYPH) {
        fb_open("glyph", '[');
        feed_glyph(glyph);
        fb_close(']');
    }
    fb_open("path", '[');
    fb_open((char *) 0, '[');
    fb_int((char *) 0, x);
    fb_int((char *) 0, y);
    fb_close(']');
    fb_close(']');
    fb_int("seen", cansee(x, y) ? 1 : 0);
    fb_end();
    fb_unnest(nested);
}

void
feed_fx_shield(coordxy x, coordxy y)
{
    feed_fx_one("shield", x, y, NO_GLYPH);
}

void
feed_fx_flash(coordxy x, coordxy y, int glyph)
{
    feed_fx_one("flash", x, y, glyph);
}

/* ---------- windows: the menus and text the game shows ---------- */

/*
 * What the game puts in front of the player besides messages: menus (the
 * inventory, what to pick up, #enhance, the spell list...) and text
 * windows (what's here, the discoveries, the tombstone, help).  Menus come
 * through add_menu() and select_menu() (windows.c); the rest by wrapping a
 * few of the window port's procedures once the session starts, each doing
 * what it did and noting what went by.  Lines, kind "ui":
 *   ev "menu_open"  before input: "win", "prompt", "how" (none, one,
 *              any), "items" [["ch", "text", flags]] (flags: 1
 *              preselected, 2 a heading)
 *   ev "menu"  after input: the same fields and "picked" [[item, count]]
 *              (or "cancelled"); closes the menu identified by "win"
 *   ev "text"  "lines" []
 *   ev "file"  "name" (a help file shown)
 * Only windows made after the session starts are followed, and the
 * inventory's; the message window's lines are msg lines already.
 * Keyframes carry "menus", an array of the open menus' fields.
 */

#define FEED_WINS 32

struct feed_item {
    char ch;
    unsigned flags;
    anything id;
    char *text;
};

static struct feed_win {
    int type;               /* NHW_*; 0: not followed */
    boolean active;         /* select_menu() is waiting for an answer */
    int how;
    char *prompt;
    struct feed_item *items;
    int nitems, szitems;
    char **lines;
    int nlines, szlines;
} fwin[FEED_WINS];

static struct window_procs feed_real; /* the port's own procedures */
static boolean feed_ui_on = FALSE;

staticfn boolean feed_win_ours(winid);
staticfn void feed_win_items_free(winid);
staticfn void feed_win_lines_free(winid);
staticfn void feed_ui_text(winid);
staticfn void feed_menu_fields(winid, int);
staticfn winid feed_w_create(int);
staticfn void feed_w_clear(winid);
staticfn void feed_w_display(winid, boolean);
staticfn void feed_w_destroy(winid);
staticfn void feed_w_putstr(winid, int, const char *);
staticfn void feed_w_note(winid, const char *);
staticfn void feed_w_putmixed(winid, int, const char *);
staticfn void feed_w_start_menu(winid, unsigned long);
staticfn void feed_w_end_menu(winid, const char *);
staticfn void feed_w_display_file(const char *, boolean);

staticfn boolean
feed_win_ours(winid w)
{
    return (feed.on && w >= 0 && w < FEED_WINS && fwin[w].type != 0);
}

staticfn void
feed_win_items_free(winid w)
{
    int i;

    for (i = 0; i < fwin[w].nitems; i++)
        free((genericptr_t) fwin[w].items[i].text);
    fwin[w].nitems = 0;
    fwin[w].active = FALSE;
    if (fwin[w].prompt)
        free((genericptr_t) fwin[w].prompt), fwin[w].prompt = 0;
}

staticfn void
feed_win_lines_free(winid w)
{
    int i;

    for (i = 0; i < fwin[w].nlines; i++)
        free((genericptr_t) fwin[w].lines[i]);
    fwin[w].nlines = 0;
}

/* a text window's lines, as shown */
staticfn void
feed_ui_text(winid w)
{
    boolean nested;
    int i;

    nested = fb_nest();
    fb_begin("ui");
    fb_str("ev", "text");
    fb_open("lines", '[');
    for (i = 0; i < fwin[w].nlines; i++)
        fb_str((char *) 0, fwin[w].lines[i]);
    fb_close(']');
    fb_end();
    fb_unnest(nested);
    feed_win_lines_free(w);
}

staticfn winid
feed_w_create(int type)
{
    winid w = (*feed_real.win_create_nhwindow)(type);

    if (w >= 0 && w < FEED_WINS) {
        feed_win_items_free(w), feed_win_lines_free(w);
        fwin[w].type = (type == NHW_MENU || type == NHW_TEXT) ? type : 0;
    }
    return w;
}

staticfn void
feed_w_clear(winid w)
{
    if (feed_win_ours(w))
        feed_win_lines_free(w);
    (*feed_real.win_clear_nhwindow)(w);
}

staticfn void
feed_w_display(winid w, boolean blocking)
{
    if (feed_win_ours(w) && fwin[w].nlines)
        feed_ui_text(w);
    (*feed_real.win_display_nhwindow)(w, blocking);
}

staticfn void
feed_w_destroy(winid w)
{
    if (w >= 0 && w < FEED_WINS) {
        feed_win_items_free(w), feed_win_lines_free(w);
        fwin[w].type = 0;
    }
    (*feed_real.win_destroy_nhwindow)(w);
}

staticfn void
feed_w_putstr(winid w, int attr, const char *str)
{
    feed_w_note(w, str);
    (*feed_real.win_putstr)(w, attr, str);
}

/* a line put in a followed window */
staticfn void
feed_w_note(winid w, const char *str)
{
    if (feed_win_ours(w) && str) {
        if (fwin[w].nlines == fwin[w].szlines) {
            char **nl;

            fwin[w].szlines = fwin[w].szlines ? 2 * fwin[w].szlines : 32;
            nl = (char **) alloc((unsigned) (fwin[w].szlines * sizeof *nl));
            if (fwin[w].nlines)
                (void) memcpy((genericptr_t) nl,
                              (genericptr_t) fwin[w].lines,
                              fwin[w].nlines * sizeof *nl);
            if (fwin[w].lines)
                free((genericptr_t) fwin[w].lines);
            fwin[w].lines = nl;
        }
        fwin[w].lines[fwin[w].nlines++] = dupstr(str);
    }
}

staticfn void
feed_w_putmixed(winid w, int attr, const char *str)
{
    char buf[BUFSZ];

    /* with the \G glyph escapes turned into the symbols the port shows:
       a message (putmixed() bypasses pline(), so feed_msg() hasn't seen
       it), or a line of a followed window, as putstr() notes one; then
       passed on to the port's own */
    if (feed.on && w == WIN_MESSAGE && str)
        feed_msg(decode_mixed(buf, str), FALSE);
    else if (feed_win_ours(w) && str)
        feed_w_note(w, decode_mixed(buf, str));
    (*feed_real.win_putmixed)(w, attr, str);
}

staticfn void
feed_w_start_menu(winid w, unsigned long mbehavior)
{
    if (w >= 0 && w < FEED_WINS && fwin[w].type)
        feed_win_items_free(w), feed_win_lines_free(w);
    (*feed_real.win_start_menu)(w, mbehavior);
}

staticfn void
feed_w_end_menu(winid w, const char *prompt)
{
    if (feed_win_ours(w) && prompt) {
        if (fwin[w].prompt)
            free((genericptr_t) fwin[w].prompt);
        fwin[w].prompt = dupstr(prompt);
    }
    (*feed_real.win_end_menu)(w, prompt);
}

staticfn void
feed_w_display_file(const char *fname, boolean complain)
{
    boolean nested;

    if (feed.on && fname) {
        nested = fb_nest();
        fb_begin("ui");
        fb_str("ev", "file");
        fb_str("name", fname);
        fb_end();
        fb_unnest(nested);
    }
    (*feed_real.win_display_file)(fname, complain);
}

/* follow the windows from now on (feed_start()) */
staticfn void
feed_ui_wrap(void)
{
    if (feed_ui_on)
        return;
    feed_ui_on = TRUE;
    feed_real = windowprocs;
    windowprocs.win_create_nhwindow = feed_w_create;
    windowprocs.win_clear_nhwindow = feed_w_clear;
    windowprocs.win_display_nhwindow = feed_w_display;
    windowprocs.win_destroy_nhwindow = feed_w_destroy;
    windowprocs.win_putstr = feed_w_putstr;
    windowprocs.win_putmixed = feed_w_putmixed;
    windowprocs.win_start_menu = feed_w_start_menu;
    windowprocs.win_end_menu = feed_w_end_menu;
    windowprocs.win_display_file = feed_w_display_file;
    /* the inventory's window exists already */
    if (WIN_INVEN != WIN_ERR && WIN_INVEN >= 0 && WIN_INVEN < FEED_WINS)
        fwin[WIN_INVEN].type = NHW_MENU;
}

/* an item added to a menu (add_menu()) */
void
feed_menu_add(winid w, const anything *id, char ch, const char *str,
              unsigned itemflags)
{
    struct feed_item *it;

    if (!feed_win_ours(w) || !str)
        return;
    if (fwin[w].nitems == fwin[w].szitems) {
        struct feed_item *ni;

        fwin[w].szitems = fwin[w].szitems ? 2 * fwin[w].szitems : 32;
        ni = (struct feed_item *) alloc(
            (unsigned) (fwin[w].szitems * sizeof *ni));
        if (fwin[w].nitems)
            (void) memcpy((genericptr_t) ni, (genericptr_t) fwin[w].items,
                          fwin[w].nitems * sizeof *ni);
        if (fwin[w].items)
            free((genericptr_t) fwin[w].items);
        fwin[w].items = ni;
    }
    it = &fwin[w].items[fwin[w].nitems++];
    it->ch = ch;
    it->id = id ? *id : cg.zeroany;
    it->flags = ((itemflags & MENU_ITEMFLAGS_SELECTED) ? 1U : 0U)
                | ((!id || !id->a_void) ? 2U : 0U);
    it->text = dupstr(str);
}

/* tty has assigned a selector to an item whose caller supplied none */
void
feed_menu_accel(winid w, const anything *id, char ch)
{
    int i;

    if (!feed_win_ours(w) || !id || !id->a_void)
        return;
    for (i = 0; i < fwin[w].nitems; i++)
        if (!fwin[w].items[i].ch
            && !memcmp((genericptr_t) &fwin[w].items[i].id,
                       (genericptr_t) id, sizeof *id)) {
            fwin[w].items[i].ch = ch;
            break;
        }
}

/* shared by menu-open events, results and keyframes */
staticfn void
feed_menu_fields(winid w, int how)
{
    int i;
    char buf[2];

    fb_int("win", w);
    fb_str("prompt", fwin[w].prompt ? fwin[w].prompt : "");
    fb_str("how", how == PICK_NONE ? "none" : how == PICK_ONE ? "one"
                                                               : "any");
    fb_open("items", '[');
    for (i = 0; i < fwin[w].nitems; i++) {
        buf[0] = fwin[w].items[i].ch, buf[1] = '\0';
        fb_open((char *) 0, '[');
        fb_str((char *) 0, buf);
        fb_str((char *) 0, fwin[w].items[i].text);
        fb_int((char *) 0, (long) fwin[w].items[i].flags);
        fb_close(']');
    }
    fb_close(']');
}

/* the menus still waiting for input, for a self-contained keyframe */
staticfn void
feed_menu_frames(void)
{
    winid w;

    fb_open("menus", '[');
    for (w = 0; w < FEED_WINS; w++)
        if (feed_win_ours(w) && fwin[w].active) {
            fb_open((char *) 0, '{');
            feed_menu_fields(w, fwin[w].how);
            fb_close('}');
        }
    fb_close(']');
}

/* end_menu() has assigned tty's selectors; publish before select_menu()
   waits.  The input hook flushes this event before reading a key. */
void
feed_menu_open(winid w, int how)
{
    boolean nested;

    if (!feed_win_ours(w) || !fwin[w].nitems)
        return;
    fwin[w].active = TRUE;
    fwin[w].how = how;
    nested = fb_nest();
    fb_begin("ui");
    fb_str("ev", "menu_open");
    feed_menu_fields(w, how);
    fb_end();
    fb_unnest(nested);
}

/* a menu has been shown and answered (select_menu()): what it offered and
   what was picked */
void
feed_menu_selected(winid w, int how, int n, menu_item *picks)
{
    boolean nested;
    int i, j;

    if (!feed_win_ours(w))
        return;
    fwin[w].active = FALSE;
    if (!fwin[w].nitems) {
        /* (a menu window used for text) */
        if (fwin[w].nlines)
            feed_ui_text(w);
        return;
    }
    nested = fb_nest();
    fb_begin("ui");
    fb_str("ev", "menu");
    feed_menu_fields(w, how);
    if (n < 0) {
        fb_int("cancelled", 1);
    } else {
        fb_open("picked", '[');
        for (j = 0; j < n && picks; j++)
            for (i = 0; i < fwin[w].nitems; i++)
                if (!(fwin[w].items[i].flags & 2U)
                    && !memcmp((genericptr_t) &fwin[w].items[i].id,
                               (genericptr_t) &picks[j].item,
                               sizeof (anything))) {
                    fb_open((char *) 0, '[');
                    fb_int((char *) 0, i);
                    fb_int((char *) 0, picks[j].count);
                    fb_close(']');
                    break;
                }
        fb_close(']');
    }
    fb_end();
    fb_unnest(nested);
}

/* ---------- the feed's test hook ---------- */

staticfn uint64 sl_objs(uint64, struct obj *);

staticfn uint64
sl_objs(uint64 h, struct obj *list)
{
    struct obj *o;
    long v[16];

    for (o = list; o; o = o->nobj) {
        v[0] = o->o_id, v[1] = o->otyp, v[2] = o->quan, v[3] = o->spe;
        v[4] = o->ox, v[5] = o->oy, v[6] = o->owornmask;
        v[7] = o->known | (o->dknown << 1) | (o->bknown << 2)
               | (o->rknown << 3) | (o->cknown << 4) | (o->lknown << 5)
               | (o->blessed << 6) | (o->cursed << 7);
        v[8] = o->corpsenm, v[9] = o->oartifact, v[10] = o->invlet;
        v[11] = o->leashmon, v[12] = o->age, v[13] = o->where;
        v[14] = o->oeroded | (o->oeroded2 << 4), v[15] = o->olocked;
        h = fb_hash((const char *) v, sizeof v, h);
        if (o->cobj)
            h = sl_objs(h, o->cobj);
    }
    return h;
}

/* NH_STATELOG=FILE: after every key the game reads, a line of its state:
   the key's number and value, the turn, how many numbers the core and the
   display RNG have drawn, and a hash of what the feed could disturb (the
   objects carried, on the level and in monsters' hands with their
   knowledge bits; the monsters; discoveries; the game log).  Whether or
   not the feed is on: test/feedtest.py compares a replay with the feed
   and one without, line by line, which pins down the first key after
   which they differ (the record's digests are checked far less often and
   don't cover all of this) */
void
feed_statelog(int key)
{
    static FILE *fp = 0;
    static boolean tried = FALSE;
    static long n = 0;
    const char *path;
    struct monst *m;
    struct gamelog_line *glp;
    uint64 h = 0;
    long v[8];
    int i;

    if (!tried) {
        tried = TRUE;
        /* a test hook, only with the player's own permissions, as for
           NH_RECORD: never open a player's path with elevated IDs */
        if (getuid() == geteuid() && getgid() == getegid()
            && !layout_dumping()
            && (path = nh_getenv("NH_STATELOG")) != 0 && *path)
            fp = fopen(path, "w");
    }
    if (!fp)
        return;
    h = sl_objs(h, gi.invent);
    h = sl_objs(h, fobj);
    for (m = fmon; m; m = m->nmon) {
        v[0] = m->m_id, v[1] = monsndx(m->data), v[2] = m->mx;
        v[3] = m->my, v[4] = m->mhp, v[5] = m->mpeaceful | (m->mtame << 1);
        v[6] = m->msleeping | (m->mcanmove << 1) | (m->mflee << 2);
        v[7] = m->mstrategy;
        h = fb_hash((const char *) v, sizeof v, h);
        h = sl_objs(h, m->minvent);
    }
    for (i = 0; i < NUM_OBJECTS; i++) {
        v[0] = objects[i].oc_name_known | (objects[i].oc_encountered << 1)
               | ((objects[i].oc_uname != 0) << 2);
        h = fb_hash((const char *) v, sizeof v[0], h);
    }
    for (i = 0, glp = gg.gamelog; glp; glp = glp->next)
        i++;
    (void) fprintf(fp, "%ld %d %ld %lu %lu %d %016llx\n", ++n, key,
                   svm.moves, nh_rng_draws[0], nh_rng_draws[1], i,
                   (unsigned long long) h);
    (void) fflush(fp);
}

/* TRUE if the feed is being written */
boolean
feed_active(void)
{
    return feed.on;
}

/* ---------- the layout dump ---------- */

/*
 * nethack --layouts FILE (files.c; wizcmds.c makes the levels): the
 * layout (mklev.c) of every level of the seed's dungeon, as JSON lines,
 * with the feed's writer and encodings:
 *   hdr      form "full", seedver, build, datahash, seed (as players see
 *            it), character, map, symbols (hash of the symbols line)
 *   symbols  terrain rules, trap symbols, trap coverage and engravings
 *   dungeon  dungeons: [dn, "name", first depth, levels]; branches:
 *            [type, [dn, dl], [dn, dl], end1_up], the Fort Ludios
 *            branch's vault end null; ludios: the levels whose vault
 *            would get that portal, [dn, dl] (a game puts it on whichever
 *            of them is made first)
 *   level    dn, dl, dep, ledger, dname, special, lflags, moves (1: Water
 *            or Air, whose bubbles or clouds move), layout, the terrain as
 *            made ("typ", "flags", "lit", "horiz", a character for every
 *            cell, as in a keyframe's "terr"), and the layout's traps,
 *            engravings, stairs and rooms as a keyframe has them, but
 *            sorted by cell, without the traps layout_trap() leaves out,
 *            and with the Fort Ludios level's portal leading nowhere set
 *            (-1, -1); wallset, arboreal, arboreal_sdoors (cell indices)
 *   skip     a level never made: ledger, dn, dl, why ("tutorial", or
 *            "placeholder": the endgame's dummy level)
 *   end      levels, hash: FNV-1a 64 of every byte before this line
 * --layout-hashes writes the hdr with form "hashes" and without character
 * and map, each level's dn, dl and layout, and the same end line (its
 * hash is the full form's).
 */

static struct layout_dump {
    struct feedbuf hdr, hdr_brief; /* each form's hdr */
    struct feedbuf symbols;
    struct feedbuf lines, brief;   /* level and skip lines; level lines */
    FILE *oracle;
    struct {
        int type;
        d_level end1, end2;
        boolean end1_up, vault1, vault2;
    } br[MAXDUNGEON * 2];
    int nbr;
    d_level ludios[MAXLINFO];
    int nludios;
    long levels;
} ldbuf;

staticfn void ld_begin(const char *);
staticfn void ld_end(struct feedbuf *);
staticfn void ld_level(const d_level *);
staticfn void ld_terrain(const uint16 *);
staticfn void ld_rule(int, int, int, const char *, int);
staticfn void ld_symbols(void);
staticfn void ld_oracle(void);

static const char *const ld_wallsets[] = {
    "main", "mines", "gehennom", "ludios", "sokoban"
};

staticfn void
ld_begin(const char *kind)
{
    feed.line.len = 0;
    fb_open((char *) 0, '{');
    fb_str("k", kind);
}

/* finish the line, onto the end of b */
staticfn void
ld_end(struct feedbuf *b)
{
    fb_put("}\n", 2);
    fb_grow(b, feed.line.len);
    (void) memcpy((genericptr_t) (b->buf + b->len),
                  (genericptr_t) feed.line.buf, feed.line.len);
    b->len += feed.line.len;
    feed.line.len = 0;
}

staticfn void
ld_level(const d_level *lev)
{
    fb_open((char *) 0, '[');
    fb_int((char *) 0, lev->dnum);
    fb_int((char *) 0, lev->dlevel);
    fb_close(']');
}

/* a level's terrain as made, a string of a character per cell for each
   of typ, flags, lit and horizontal */
staticfn void
ld_terrain(const uint16 *terr)
{
    static const char *const names[] = { "typ", "flags", "lit", "horiz" };
    char *s = (char *) alloc(COLNO * ROWNO + 1);
    int field, i;

    for (field = 0; field < 4; field++) {
        for (i = 0; i < COLNO * ROWNO; i++)
            s[i] = feed_terr_chr((int) terr[i], field);
        s[COLNO * ROWNO] = '\0';
        fb_str(names[field], s);
    }
    free((genericptr_t) s);
}

/* ordered rules; conditions are conjunctive, omitted fields match anything */
staticfn void
ld_rule(int glyph, int mask, int value, const char *field, int equal)
{
    static const int walls[] = {
        GLYPH_CMAP_MAIN_OFF, GLYPH_CMAP_MINES_OFF, GLYPH_CMAP_GEH_OFF,
        GLYPH_CMAP_KNOX_OFF, GLYPH_CMAP_SOKO_OFF
    };
    boolean wall = (glyph >= GLYPH_CMAP_MAIN_OFF
                    && glyph < GLYPH_CMAP_A_OFF);
    int i;

    for (i = 0; i < (wall ? SIZE(walls) : 1); i++) {
        fb_open((char *) 0, '{');
        if (mask) {
            fb_open("flags", '[');
            fb_int((char *) 0, mask);
            fb_int((char *) 0, value);
            fb_close(']');
        }
        if (field)
            fb_int(field, equal);
        if (wall)
            fb_str("wallset", ld_wallsets[i]);
        fb_open("sym", '[');
        feed_glyph(wall ? glyph_to_cmap(glyph) - S_vwall + walls[i]
                       : glyph);
        fb_close(']');
        fb_close('}');
    }
}

staticfn void
ld_symbols(void)
{
    static const int simple[] = {
        S_stone, S_vwall, S_hwall, S_tlcorn, S_trcorn, S_blcorn,
        S_brcorn, S_crwall, S_tuwall, S_tdwall, S_tlwall, S_trwall,
        S_vcdbridge, S_tree, S_vwall, S_stone, S_pool, S_pool,
        S_water, S_room, S_lava, S_lavawall, S_bars, S_vcdoor,
        S_corr, S_room, S_upstair, S_upladder, S_fountain, S_throne,
        S_sink, S_grave, S_altar, S_ice, S_vodbridge, S_air, S_cloud
    };
    static const int under[] = { DB_MOAT, DB_LAVA, DB_ICE, DB_FLOOR };
    static const int spans[] = { S_pool, S_lava, S_ice, S_room };
    static const int altars[] = {
        AM_SANCTUM, AM_LAWFUL, AM_NEUTRAL, AM_CHAOTIC
    };
    static const int covered[] = { POOL, MOAT, WATER, LAVAPOOL, LAVAWALL,
                                  DRAWBRIDGE_UP };
    int typ, i, sym;

    /* fail the build if a terrain type is added without its symbol */
    (void) sizeof (char[SIZE(simple) == MAX_TYPE ? 1 : -1]);
    layout_glyphmap(TRUE);
    ld_begin("symbols");
    fb_open("unexplored", '[');
    feed_glyph(GLYPH_UNEXPLORED);
    fb_close(']');
    fb_open("terrain", '[');
    for (typ = 0; typ < MAX_TYPE; typ++) {
        fb_open((char *) 0, '[');
        switch (typ) {
        case STONE:
        case SCORR:
            ld_rule(cmap_to_glyph(S_tree), 0, 0, "arboreal", 1);
            break;
        case SDOOR:
            ld_rule(cmap_to_glyph(S_tree), 0, 0, "arboreal_sdoor", 1);
            ld_rule(cmap_to_glyph(S_hwall), 0, 0, "horiz", 1);
            break;
        case DOOR:
            ld_rule(cmap_to_glyph(S_ndoor), 31, 0, (char *) 0, 0);
            ld_rule(cmap_to_glyph(S_ndoor), D_BROKEN, D_BROKEN,
                    (char *) 0, 0);
            ld_rule(cmap_to_glyph(S_hodoor), D_ISOPEN, D_ISOPEN,
                    "horiz", 1);
            ld_rule(cmap_to_glyph(S_vodoor), D_ISOPEN, D_ISOPEN,
                    (char *) 0, 0);
            ld_rule(cmap_to_glyph(S_hcdoor), 0, 0, "horiz", 1);
            break;
        case CORR:
            ld_rule(cmap_to_glyph(S_litcorr), 0, 0, "lit", 1);
            break;
        case STAIRS:
        case LADDER:
            sym = (typ == STAIRS) ? S_dnstair : S_dnladder;
            ld_rule(cmap_to_glyph(sym), LA_DOWN, LA_DOWN, (char *) 0, 0);
            break;
        case DBWALL:
        case DRAWBRIDGE_DOWN:
            sym = (typ == DBWALL) ? S_hcdbridge : S_hodbridge;
            ld_rule(cmap_to_glyph(sym), 0, 0, "horiz", 1);
            break;
        case DRAWBRIDGE_UP:
            for (i = 0; i < SIZE(under); i++)
                ld_rule(cmap_to_glyph(spans[i]), DB_UNDER, under[i],
                        (char *) 0, 0);
            break;
        case ALTAR:
            for (i = 0; i < SIZE(altars); i++)
                ld_rule(altar_to_glyph(altars[i]),
                        i ? AM_MASK : AM_SANCTUM, altars[i],
                        (char *) 0, 0);
            break;
        }
        ld_rule(typ == ALTAR ? altar_to_glyph(0)
                            : cmap_to_glyph(simple[typ]),
                0, 0, (char *) 0, 0);
        fb_close(']');
    }
    fb_close(']');
    fb_open("traps", '[');
    fb_key((char *) 0);
    fb_raw("null");
    /* the last two symbols are detected door and chest traps */
    for (i = 1; i < TRAPNUM; i++) {
        fb_open((char *) 0, '[');
        feed_glyph(cmap_to_glyph(trap_to_defsym(i)));
        fb_close(']');
    }
    fb_close(']');
    fb_open("covers_traps", '[');
    for (i = 0; i < SIZE(covered); i++) {
        fb_open((char *) 0, '{');
        fb_int("typ", covered[i]);
        if (covered[i] == DRAWBRIDGE_UP) {
            fb_open("flags", '[');
            fb_int((char *) 0, DB_UNDER);
            fb_int((char *) 0, DB_MOAT);
            fb_close(']');
        }
        fb_close('}');
    }
    fb_close(']');
    fb_open("engravings", '[');
    for (typ = 0; typ < MAX_TYPE; typ++) {
        if (typ == CORR || typ == ROOM || typ == ICE) {
            fb_open((char *) 0, '[');
            feed_glyph(cmap_to_glyph(typ == CORR ? S_engrcorr : S_engroom));
            fb_close(']');
        } else {
            fb_key((char *) 0);
            fb_raw("null");
        }
    }
    fb_close(']');
    ld_end(&ldbuf.symbols);
    layout_glyphmap(FALSE);
}

/* independent of the exported rules: ask the display code about each cell */
staticfn void
ld_oracle(void)
{
    struct feedbuf line = { 0 };
    coordxy x, y;
    struct rm saved;
    struct trap *trap;
    struct engr *engr;
    stairway *stairs;
    boolean traversed, litcorr = flags.lit_corridor;
    boolean underwater = u.uinwater;
    int glyph, typ, mask;

    if (!ldbuf.oracle)
        return;
    layout_glyphmap(TRUE);
    flags.lit_corridor = FALSE;
    u.uinwater = FALSE;
    ld_begin("oracle");
    fb_int("dn", u.uz.dnum);
    fb_int("dl", u.uz.dlevel);
    fb_open("cells", '[');
    for (y = 0; y < ROWNO; y++)
        for (x = 0; x < COLNO; x++) {
            saved = levl[x][y];
            levl[x][y].seenv = SVALL;
            levl[x][y].waslit = levl[x][y].lit;
            stairs = stairway_at(x, y);
            traversed = stairs ? stairs->u_traversed : FALSE;
            if (stairs)
                stairs->u_traversed = FALSE;
            glyph = back_to_glyph(x, y);
            if (stairs)
                stairs->u_traversed = traversed;
            levl[x][y] = saved;
            trap = t_at(x, y);
            engr = engr_at(x, y);
            if (trap && layout_trap(trap) && !covers_traps(x, y))
                glyph = trap_to_glyph(trap);
            else if (engr && spot_shows_engravings(x, y))
                glyph = engraving_to_glyph(engr);
            fb_open((char *) 0, '[');
            feed_glyph(glyph);
            fb_close(']');
        }
    fb_close(']');
    /* probe the game's overlay predicates independently of the exported
       rules, including terrain/flag pairs absent from generated levels */
    saved = levl[1][0];
    fb_open("overlays", '[');
    for (typ = 0; typ < MAX_TYPE; typ++)
        for (mask = 0; mask < 32; mask++) {
            levl[1][0].typ = typ;
            levl[1][0].flags = mask;
            fb_open((char *) 0, '[');
            fb_int((char *) 0, typ);
            fb_int((char *) 0, mask);
            fb_int((char *) 0, covers_traps(1, 0) ? 1 : 0);
            fb_int((char *) 0, spot_shows_engravings(1, 0) ? 1 : 0);
            fb_close(']');
        }
    fb_close(']');
    levl[1][0] = saved;
    ld_end(&line);
    flags.lit_corridor = litcorr;
    u.uinwater = underwater;
    layout_glyphmap(FALSE);
    if (fwrite(line.buf, 1, line.len, ldbuf.oracle) != line.len)
        layout_dump_fail("can't write NH_LAYOUTCHECK");
    free((genericptr_t) line.buf);
}

/* the dump begins, once the game has been set up: its hdr lines, and the
   branches as the game starts with them */
void
layout_dump_start(void)
{
    branch *br;
    char hex[LAYOUT_HEXSZ];
    int form;

    ldbuf.oracle = layout_dump_check_open();
    ld_symbols();
    (void) layout_hex(fb_hash(ldbuf.symbols.buf, ldbuf.symbols.len, 0), hex);
    for (form = 0; form < 2; form++) {
        ld_begin("hdr");
        fb_str("form", form ? "hashes" : "full");
        fb_int("seedver", SEED_GEN_VERSION);
        fb_str("build", build_id());
        fb_str("datahash", data_files_hash());
        fb_str("seed", nh_seed_display(FALSE));
        if (!form) {
            fb_str("symbols", hex);
            fb_open("character", '{');
            fb_str("role", (flags.female && gu.urole.name.f)
                               ? gu.urole.name.f : gu.urole.name.m);
            fb_str("race", gu.urace.noun);
            fb_str("gender", flags.female ? "female" : "male");
            fb_str("align", feed_align(u.ualignbase[A_ORIGINAL]));
            fb_close('}');
            fb_open("map", '{');
            fb_int("cols", COLNO);
            fb_int("rows", ROWNO);
            fb_close('}');
        }
        ld_end(form ? &ldbuf.hdr_brief : &ldbuf.hdr);
    }
    for (br = svb.branches; br && ldbuf.nbr < SIZE(ldbuf.br); br = br->next) {
        ldbuf.br[ldbuf.nbr].type = br->type;
        ldbuf.br[ldbuf.nbr].end1 = br->end1;
        ldbuf.br[ldbuf.nbr].end2 = br->end2;
        ldbuf.br[ldbuf.nbr].end1_up = br->end1_up;
        ldbuf.br[ldbuf.nbr].vault1 = (br->end2.dnum == knox_level.dnum
                                      && br->end1.dnum != knox_level.dnum);
        ldbuf.br[ldbuf.nbr].vault2 = (br->end1.dnum == knox_level.dnum
                                      && br->end2.dnum != knox_level.dnum);
        ldbuf.nbr++;
    }
}

/* the level just made, ledger */
void
layout_dump_level(int ledger)
{
    const uint16 *terr = level_layout(ledger);
    uint64 parts[NUM_LEVELHASH];
    char hex[LAYOUT_HEXSZ];
    struct trap **traps;
    struct engr **engrs;
    stairway **stairs;
    d_level dst;
    int i, n;

    (void) level_fingerprint_at_creation(ledger, parts);
    (void) layout_hex(parts[0], hex);
    ld_begin("level");
    fb_int("dn", u.uz.dnum);
    fb_int("dl", u.uz.dlevel);
    fb_int("dep", depth(&u.uz));
    fb_int("ledger", ledger);
    fb_str("dname", svd.dungeons[u.uz.dnum].dname);
    feed_level_id();
    fb_int("moves", (Is_waterlevel(&u.uz) || Is_airlevel(&u.uz)) ? 1 : 0);
    fb_str("layout", hex);
    fb_str("wallset", ld_wallsets[In_mines(&u.uz) ? 1
                                  : In_hell(&u.uz) ? 2
                                    : Is_knox(&u.uz) ? 3
                                      : In_sokoban(&u.uz) ? 4 : 0]);
    fb_int("arboreal", svl.level.flags.arboreal);
    fb_open("arboreal_sdoors", '[');
    for (i = 0; i < COLNO * ROWNO; i++)
        if (levl[i % COLNO][i / COLNO].typ == SDOOR
            && levl[i % COLNO][i / COLNO].arboreal_sdoor)
            fb_int((char *) 0, i);
    fb_close(']');
    ld_terrain(terr);
    traps = layout_traps(&n);
    fb_open("traps", '[');
    for (i = 0; i < n; i++) {
        layout_trap_dest(traps[i], &dst);
        feed_trap_tuple(traps[i], &dst);
    }
    fb_close(']');
    free((genericptr_t) traps);
    engrs = layout_engravings(&n);
    fb_open("engr", '[');
    for (i = 0; i < n; i++)
        feed_engr_tuple(engrs[i]);
    fb_close(']');
    free((genericptr_t) engrs);
    stairs = layout_stairs(&n);
    fb_open("stairs", '[');
    for (i = 0; i < n; i++)
        feed_stair_tuple(stairs[i]);
    fb_close(']');
    free((genericptr_t) stairs);
    fb_open("rooms", '[');
    for (i = 0; i < svn.nroom; i++)
        feed_room_tuple(&svr.rooms[i]);
    fb_close(']');
    ld_end(&ldbuf.lines);

    ld_begin("level");
    fb_int("dn", u.uz.dnum);
    fb_int("dl", u.uz.dlevel);
    fb_str("layout", hex);
    ld_end(&ldbuf.brief);
    ldbuf.levels++;
    ld_oracle();
}

/* a level that is never made (why) */
void
layout_dump_skip(int ledger, const char *why)
{
    ld_begin("skip");
    fb_int("ledger", ledger);
    fb_int("dn", ledger_to_dnum((xint16) ledger));
    fb_int("dl", ledger_to_dlev((xint16) ledger));
    fb_str("why", why);
    ld_end(&ldbuf.lines);
}

/* the level just made would get the Fort Ludios portal, if it were the
   first such level made */
void
layout_dump_ludios(d_level *lev)
{
    if (ldbuf.nludios < SIZE(ldbuf.ludios))
        assign_level(&ldbuf.ludios[ldbuf.nludios++], lev);
}

/* the dump, whole, in the form asked for (alloc()ed; *len gets its
   length) */
char *
layout_dump_text(size_t *len)
{
    struct feedbuf all, brief, *out;
    char hex[LAYOUT_HEXSZ];
    int i;

    (void) memset((genericptr_t) &all, 0, sizeof all);
    (void) memset((genericptr_t) &brief, 0, sizeof brief);
    fb_grow(&all, ldbuf.hdr.len + ldbuf.symbols.len);
    (void) memcpy((genericptr_t) all.buf, (genericptr_t) ldbuf.hdr.buf,
                  ldbuf.hdr.len);
    all.len = ldbuf.hdr.len;
    (void) memcpy((genericptr_t) (all.buf + all.len),
                  (genericptr_t) ldbuf.symbols.buf, ldbuf.symbols.len);
    all.len += ldbuf.symbols.len;
    ld_begin("dungeon");
    fb_open("dungeons", '[');
    for (i = 0; i < svn.n_dgns; i++) {
        fb_open((char *) 0, '[');
        fb_int((char *) 0, i);
        fb_str((char *) 0, svd.dungeons[i].dname);
        fb_int((char *) 0, svd.dungeons[i].depth_start);
        fb_int((char *) 0, svd.dungeons[i].num_dunlevs);
        fb_close(']');
    }
    fb_close(']');
    fb_open("branches", '[');
    for (i = 0; i < ldbuf.nbr; i++) {
        fb_open((char *) 0, '[');
        fb_int((char *) 0, ldbuf.br[i].type);
        if (ldbuf.br[i].vault1)
            fb_key((char *) 0), fb_raw("null");
        else
            ld_level(&ldbuf.br[i].end1);
        if (ldbuf.br[i].vault2)
            fb_key((char *) 0), fb_raw("null");
        else
            ld_level(&ldbuf.br[i].end2);
        fb_int((char *) 0, ldbuf.br[i].end1_up);
        fb_close(']');
    }
    fb_close(']');
    fb_open("ludios", '[');
    for (i = 0; i < ldbuf.nludios; i++)
        ld_level(&ldbuf.ludios[i]);
    fb_close(']');
    ld_end(&all);
    fb_grow(&all, ldbuf.lines.len);
    (void) memcpy((genericptr_t) (all.buf + all.len),
                  (genericptr_t) ldbuf.lines.buf, ldbuf.lines.len);
    all.len += ldbuf.lines.len;
    (void) layout_hex(fb_hash(all.buf, all.len, 0), hex);

    out = &all;
    if (layout_dump_hashes()) {
        fb_grow(&brief, ldbuf.hdr_brief.len + ldbuf.brief.len);
        (void) memcpy((genericptr_t) brief.buf,
                      (genericptr_t) ldbuf.hdr_brief.buf,
                      ldbuf.hdr_brief.len);
        (void) memcpy((genericptr_t) (brief.buf + ldbuf.hdr_brief.len),
                      (genericptr_t) ldbuf.brief.buf, ldbuf.brief.len);
        brief.len = ldbuf.hdr_brief.len + ldbuf.brief.len;
        free((genericptr_t) all.buf);
        out = &brief;
    }
    ld_begin("end");
    fb_int("levels", ldbuf.levels);
    fb_str("hash", hex);
    ld_end(out);
    free((genericptr_t) ldbuf.hdr.buf);
    free((genericptr_t) ldbuf.hdr_brief.buf);
    free((genericptr_t) ldbuf.symbols.buf);
    free((genericptr_t) ldbuf.lines.buf);
    free((genericptr_t) ldbuf.brief.buf);
    if (ldbuf.oracle && fclose(ldbuf.oracle))
        layout_dump_fail("can't close NH_LAYOUTCHECK");
    (void) memset((genericptr_t) &ldbuf, 0, sizeof ldbuf);
    *len = out->len;
    return out->buf;
}

#else /* !(UNIX && !SFCTOOL) */

void feed_init(void) { return; }
void feed_start(boolean restored UNUSED) { return; }
void feed_pos(void) { return; }
void feed_step(void) { return; }
void feed_boundary(void) { return; }
void feed_level_leave(boolean a UNUSED, boolean f UNUSED,
                      boolean p UNUSED) { return; }
void feed_level_arrive(void) { return; }
void feed_msg(const char *t UNUSED, boolean p UNUSED) { return; }
void feed_key(int k UNUSED) { return; }
void feed_livelog(long l UNUSED, const char *t UNUSED) { return; }
void feed_kill(struct monst *m UNUSED) { return; }
void feed_death(const char *h UNUSED, const char *c UNUSED) { return; }
void feed_dump(const char *t UNUSED) { return; }
void feed_flush(void) { return; }
void feed_idle(void) { return; }
void feed_got_key(int k UNUSED) { return; }
void feed_final(void) { return; }
void feed_end(const char *h UNUSED) { return; }
void feed_replay_next(void) { return; }
void feed_statelog(int k UNUSED) { return; }
void feed_fx(coordxy x UNUSED, coordxy y UNUSED) { return; }
void feed_fx_shield(coordxy x UNUSED, coordxy y UNUSED) { return; }
void feed_fx_flash(coordxy x UNUSED, coordxy y UNUSED, int g UNUSED)
{
    return;
}
void feed_menu_add(winid w UNUSED, const anything *i UNUSED, char c UNUSED,
                   const char *s UNUSED, unsigned f UNUSED) { return; }
void feed_menu_open(winid w UNUSED, int h UNUSED) { return; }
void feed_menu_selected(winid w UNUSED, int h UNUSED, int n UNUSED,
                        menu_item *p UNUSED) { return; }
void feed_menu_accel(winid w UNUSED, const anything *i UNUSED,
                     char c UNUSED) { return; }
boolean feed_active(void) { return FALSE; }

#endif /* ?(UNIX && !SFCTOOL) */

/*feed.c*/
