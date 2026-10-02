/* NetHack 5.0	files.c	$NHDT-Date: 1781973049 2026/06/20 16:30:49 $  $NHDT-Branch: NetHack-5.0 $:$NHDT-Revision: 1.448 $ */
/* Copyright (c) Stichting Mathematisch Centrum, Amsterdam, 1985. */
/*-Copyright (c) Derek S. Ray, 2015. */
/* NetHack may be freely redistributed.  See license for details. */

#define NEED_VARARGS

#if defined(WIN32)
#include "win32api.h"
#endif

#include "hack.h"
#include "dlb.h"

#ifdef SFCTOOL
#ifdef TTY_GRAPHICS
#undef TTY_GRAPHICS
#endif
#ifdef mark_synch
#undef mark_synch
#endif
#define mark_synch()
#ifdef raw_print
#undef raw_print
#endif
#define raw_print(a)
#ifdef WINDOWPORT
#undef WINDOWPORT
#endif
#define WINDOWPORT(x) FALSE
#ifdef clear_nhwindow
#undef clear_nhwindow
#endif
#define clear_nhwindow(x)
#endif /* SFCTOOL */

#ifdef TTY_GRAPHICS
#include "wintty.h" /* more() */
#endif

#if (!defined(MAC68K) && !defined(O_WRONLY) && !defined(AZTEC_C)) \
    || defined(USE_FCNTL)
#include <fcntl.h>
#endif

#include <errno.h>
#ifdef _MSC_VER /* MSC 6.0 defines errno quite differently */
#if (_MSC_VER >= 600)
#define SKIP_ERRNO
#endif
#else
#ifdef NHSTDC
#define SKIP_ERRNO
#endif
#endif
#ifndef SKIP_ERRNO
#ifdef _DCC
const
#endif
    extern int errno;
#endif

#ifdef ZLIB_COMP /* RLC 09 Mar 1999: Support internal ZLIB */
#include "zlib.h"
#ifndef COMPRESS_EXTENSION
#define COMPRESS_EXTENSION ".gz"
#endif
#endif

#if defined(UNIX) && defined(SELECTSAVED)
#include <sys/types.h>
#include <dirent.h>
#endif
#ifdef UNIX /* replaying recorded games (below) */
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <poll.h>
#include <unistd.h>
#endif

#if defined(UNIX) || defined(VMS) || !defined(NO_SIGNAL)
#include <signal.h>
#endif

#if defined(MSDOS) || defined(OS2) || defined(TOS) || defined(WIN32)
#if !defined(__DJGPP__) && !defined(__MINT__)
#include <sys\stat.h>
#else
#include <sys/stat.h>
#endif
#endif
#ifndef O_BINARY /* used for micros, no-op for others */
#define O_BINARY 0
#endif

#ifdef PREFIXES_IN_USE
#define FQN_NUMBUF 8
static char fqn_filename_buffer[FQN_NUMBUF][FQN_MAX_FILENAME];
#endif

#if !defined(SAVE_EXTENSION)
#ifdef MICRO
#define SAVE_EXTENSION ".sav"
#endif
#ifdef WIN32
#define SAVE_EXTENSION ".NetHack-saved-game"
#endif
#endif

#if defined(WIN32)
#include <share.h>
#include <io.h>
#define F_OK 0
#define access _access
#endif

#ifdef AMIGA
extern char PATH[]; /* see sys/amiga/amidos.c */

#include <libraries/dos.h>
extern void amii_set_text_font(char *, int);
#endif

#if defined(WIN32) || defined(MSDOS)
#ifdef MSDOS
#define Delay(a) msleep(a)
#endif
#define Close close
#ifndef WIN_CE
#ifdef DeleteFile
#undef DeleteFile
#endif
#define DeleteFile unlink
#endif
#ifdef WIN32
/*from windsys.c */
extern char *translate_path_variables(const char *, char *);
extern boolean get_user_home_folder(char *, size_t);
#endif
#endif

#ifdef MAC68K
#undef unlink
#define unlink macunlink
#endif

#if (defined(macintosh) && (defined(__SC__) || defined(__MRC__))) \
    || defined(__MWERKS__)
#define PRAGMA_UNUSED
#endif

#ifndef SFCTOOL
staticfn NHFILE *new_nhfile(void);
staticfn void free_nhfile(NHFILE *);
#else
NHFILE *new_nhfile(void);
void free_nhfile(NHFILE *);
#endif /* SFCTOOL */

#ifdef SELECTSAVED
staticfn int QSORTCALLBACK strcmp_wrap(const void *, const void *);
#endif
staticfn char *set_bonesfile_name(char *, d_level *);
staticfn char *set_bonestemp_name(void);
#ifdef COMPRESS
staticfn void redirect(const char *, const char *, FILE *, boolean);
#endif
#if defined(COMPRESS) || defined(ZLIB_COMP)
staticfn void docompress_file(const char *, boolean);
#endif
#if defined(ZLIB_COMP)
staticfn boolean make_compressed_name(const char *, char *);
#endif

ATTRNORETURN staticfn void problematic_savefile(NHFILE *, int, const char *)
    NORETURN;
staticfn void preserve_savefile(void);
#ifndef SFCTOOL
staticfn int doconvert_file(const char *, int, boolean);
#endif /* SFCTOOL */
staticfn boolean make_converted_name(const char *);

#ifndef SFCTOOL
staticfn NHFILE *viable_nhfile(NHFILE *);
#ifdef SELECTSAVED
staticfn int QSORTCALLBACK strcmp_wrap(const void *, const void *);
#endif
staticfn char *set_bonesfile_name(char *, d_level *);
staticfn char *set_bonestemp_name(void);
#ifndef USE_FCNTL
staticfn char *make_lockname(const char *, char *);
#endif
staticfn FILE *fopen_wizkit_file(void);
staticfn void wizkit_addinv(struct obj *);
boolean proc_wizkit_line(char *buf);
void read_wizkit(void);  /* in extern.h; why here too? */
staticfn FILE *fopen_sym_file(void);
staticfn NHFILE *viable_nhfile(NHFILE *);
#endif /* !SFCTOOL */

/* return a file's name without its path and optionally trailing 'type' */
const char *
nh_basename(const char *fname, boolean keep_suffix)
{
#ifndef VMS
    static char basebuf[80];
    const char *p;

    if ((p = strrchr(fname, '/')) != 0)
        fname = p + 1;
#if defined(WIN32) || defined(MSDOS)
    if ((p = strrchr(fname, '\\')) != 0)
        fname = p + 1;
#endif
    if ((p = strrchr(fname, '.')) != 0 && !keep_suffix) {
        size_t ln = (size_t) (p - fname);
        /* note that without path, name should be reasonable length;
           it is expected to refer to a source file name or run-time
           configuration file name and those aren't arbitrarily long;
           if "name" part of "name.suffix" is too long for 'basebuf[]',
           just return that as-is without stripping off ".suffix" */

        if (ln < sizeof basebuf) {
            strncpy(basebuf, fname, ln);
            basebuf[ln] = '\0';
            fname = basebuf;
        }
    }
#else /* VMS */
    fname = vms_basename(fname, keep_suffix);
#endif
    return fname;
}

/*
 * fname_encode()
 *
 *   Args:
 *      legal       zero-terminated list of acceptable file name characters
 *      quotechar   lead-in character used to quote illegal characters as
 *                  hex digits
 *      s           string to encode
 *      callerbuf   buffer to house result
 *      bufsz       size of callerbuf
 *
 *   Notes:
 *      The hex digits 0-9 and A-F are always part of the legal set due to
 *      their use in the encoding scheme, even if not explicitly included in
 *      'legal'.
 *
 *   Sample:
 *      The following call:
 * (void) fname_encode("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz",
 *                     '%', "This is a % test!", buf, 512);
 *      results in this encoding:
 *          "This%20is%20a%20%25%20test%21"
 */
char *
fname_encode(
    const char *legal,
    char quotechar,
    char *s,
    char *callerbuf,
    int bufsz)
{
    char *sp, *op;
    int cnt = 0;
    static char hexdigits[] = "0123456789ABCDEF";

    sp = s;
    op = callerbuf;
    *op = '\0';

    while (*sp) {
        /* Do we have room for one more character or encoding? */
        if ((bufsz - cnt) <= 4)
            return callerbuf;

        if (*sp == quotechar) {
            (void) sprintf(op, "%c%02X", quotechar, *sp);
            op += 3;
            cnt += 3;
        } else if ((strchr(legal, *sp) != 0)
                   || (strchr(hexdigits, *sp) != 0)) {
            *op++ = *sp;
            *op = '\0';
            cnt++;
        } else {
            (void) sprintf(op, "%c%02X", quotechar, *sp);
            op += 3;
            cnt += 3;
        }
        sp++;
    }
    return callerbuf;
}

/*
 * fname_decode()
 *
 *   Args:
 *      quotechar   lead-in character used to quote illegal characters as
 *                  hex digits
 *      s           string to decode
 *      callerbuf   buffer to house result
 *      bufsz       size of callerbuf
 */
char *
fname_decode(char quotechar, char *s, char *callerbuf, int bufsz)
{
    char *sp, *op;
    int k, calc, cnt = 0;
    static char hexdigits[] = "0123456789ABCDEF";

    sp = s;
    op = callerbuf;
    *op = '\0';

    while (*sp) {
        /* Do we have room for one more character? */
        if ((bufsz - cnt) <= 2)
            return callerbuf;
        if (*sp == quotechar) {
            sp++;
            for (k = 0; k < 16; ++k)
                if (*sp == hexdigits[k])
                    break;
            if (k >= 16)
                return callerbuf; /* impossible, so bail */
            calc = k << 4;
            sp++;
            for (k = 0; k < 16; ++k)
                if (*sp == hexdigits[k])
                    break;
            if (k >= 16)
                return callerbuf; /* impossible, so bail */
            calc += k;
            sp++;
            *op++ = calc;
            *op = '\0';
        } else {
            *op++ = *sp++;
            *op = '\0';
        }
        cnt++;
    }
    return callerbuf;
}

#ifdef PREFIXES_IN_USE
#define UNUSED_if_not_PREFIXES_IN_USE /*empty*/
#else
#define UNUSED_if_not_PREFIXES_IN_USE UNUSED
#endif

/*ARGSUSED*/
const char *
fqname(const char *basenam,
       int whichprefix UNUSED_if_not_PREFIXES_IN_USE,
       int buffnum UNUSED_if_not_PREFIXES_IN_USE)
{
#ifdef PREFIXES_IN_USE
    char *bufptr;
#endif
#ifdef WIN32
    char tmpbuf[BUFSZ];
#endif

#ifndef PREFIXES_IN_USE
    return basenam;
#else
    if (!basenam || whichprefix < 0 || whichprefix >= PREFIX_COUNT)
        return basenam;
    if (!gf.fqn_prefix[whichprefix])
        return basenam;
    if (buffnum < 0 || buffnum >= FQN_NUMBUF) {
        impossible("Invalid fqn_filename_buffer specified: %d", buffnum);
        buffnum = 0;
    }
    bufptr = gf.fqn_prefix[whichprefix];
#ifdef WIN32
    if (strchr(gf.fqn_prefix[whichprefix], '%')
        || strchr(gf.fqn_prefix[whichprefix], '~'))
        bufptr = translate_path_variables(gf.fqn_prefix[whichprefix], tmpbuf);
#endif
    if (strlen(bufptr) + strlen(basenam) >= FQN_MAX_FILENAME) {
        impossible("fqname too long: %s + %s", bufptr, basenam);
        return basenam; /* XXX */
    }
    Strcpy(fqn_filename_buffer[buffnum], bufptr);
    return strcat(fqn_filename_buffer[buffnum], basenam);
#endif /* !PREFIXES_IN_USE */
}

#ifndef SFCTOOL
/* reasonbuf must be at least BUFSZ, supplied by caller */
int
validate_prefix_locations(char *reasonbuf)
{
#if defined(NOCWD_ASSUMPTIONS)
    FILE *fp;
    const char *filename;
    int prefcnt, failcount = 0;
    char panicbuf1[BUFSZ], panicbuf2[BUFSZ];
    const char *details;
#endif

    if (reasonbuf)
        reasonbuf[0] = '\0';
#if defined(NOCWD_ASSUMPTIONS)
    for (prefcnt = 1; prefcnt < PREFIX_COUNT; prefcnt++) {
        /* don't test writing to configdir or datadir; they're readonly */
        if (prefcnt == SYSCONFPREFIX || prefcnt == CONFIGPREFIX
            || prefcnt == DATAPREFIX)
            continue;
        filename = fqname("validate", prefcnt, 3);
        if ((fp = fopen(filename, "w"))) {
            fclose(fp);
            (void) unlink(filename);
        } else {
            if (reasonbuf) {
                if (failcount)
                    Strcat(reasonbuf, ", ");
                Strcat(reasonbuf, fqn_prefix_names[prefcnt]);
            }
            /* the paniclog entry gets the value of errno as well */
            Sprintf(panicbuf1, "Invalid %s", fqn_prefix_names[prefcnt]);
#if defined(NHSTDC) && !defined(NOTSTDC)
            if (!(details = strerror(errno)))
#endif
                details = "";
            Sprintf(panicbuf2, "\"%s\", (%d) %s",
                    gf.fqn_prefix[prefcnt], errno, details);
            paniclog(panicbuf1, panicbuf2);
            failcount++;
        }
    }
    if (failcount)
        return 0;
    else
#endif
        return 1;
}

/* fopen a file, with OS-dependent bells and whistles */
/* NOTE: a simpler version of this routine also exists in util/dlb_main.c */
FILE *
fopen_datafile(const char *filename, const char *mode, int prefix)
{
    FILE *fp;

    filename = fqname(filename, prefix, prefix == TROUBLEPREFIX ? 3 : 0);
    fp = fopen(filename, mode);
    return fp;
}
#endif /* !SFCTOOL */

/* ----------  EXTERNAL FILE SUPPORT ----------- */

/* determine byte order */
static const int bei = 1;
#define IS_BIGENDIAN() ( (*(char*)&bei) == 0 )

void
init_nhfile(NHFILE *nhfp)
{
    if (nhfp->structlevel) {
        if (nhfp->fd != -1) {
            impossible("Warning - Unclosed structlevel file being reinitialized");
            (void) nhclose(nhfp->fd);
        }
    } else if (nhfp->fpdef) {
        if (nhfp->fpdef) {
            impossible("Warning - Unclosed fieldlevel file being reinitialized");
            (void) fclose(nhfp->fpdef);
        }
    }
    nhfp->fd = -1;
    nhfp->fpdef = (FILE *) 0;

    nhfp->mode = COUNTING;
    nhfp->structlevel = TRUE;
    nhfp->fieldlevel = FALSE;
    nhfp->addinfo = FALSE;
    nhfp->bendian = IS_BIGENDIAN();
    nhfp->fplog = (FILE *) 0;
    nhfp->fpdebug = (FILE *) 0;
    nhfp->rcount = nhfp->wcount = 0;
    nhfp->eof = FALSE;
    nhfp->seeded = FALSE;
    nhfp->validation_error[0] = '\0';
    nhfp->fnidx = 0;
    nhfp->style.deflt = FALSE;
    nhfp->style.binary = TRUE;
    nhfp->nhfpconvert = 0;
}

#ifndef SFCTOOL
staticfn
#endif
NHFILE *
new_nhfile(void)
{
    NHFILE *nhfp = (NHFILE *) alloc(sizeof *nhfp);

    memset((genericptr_t) nhfp, 0, sizeof *nhfp);
    init_nhfile(nhfp);
    return nhfp;
}

#ifndef SFCTOOL
staticfn
#endif
void
free_nhfile(NHFILE *nhfp)
{
    if (nhfp) {
        init_nhfile(nhfp);
        free((genericptr_t) nhfp);
    }
}

void
close_nhfile(NHFILE *nhfp)
{
    if (nhfp->structlevel && nhfp->fd != -1)
        (void) nhclose(nhfp->fd), nhfp->fd = -1;
    else if (nhfp->fpdef)
        (void) fclose(nhfp->fpdef), nhfp->fpdef = (FILE *) 0;
    if (nhfp->fplog)
        (void) fprintf(nhfp->fplog, "# closing\n");
    if (nhfp->fplog)
        (void) fclose(nhfp->fplog);
    if (nhfp->fpdebug)
        (void) fclose(nhfp->fpdebug);
    free_nhfile(nhfp);
}

void
rewind_nhfile(NHFILE *nhfp)
{
    if (nhfp->structlevel) {
#ifdef BSD
        (void) lseek(nhfp->fd, 0L, 0);
#else
        (void) lseek(nhfp->fd, (off_t) 0, 0);
#endif
    } else {
        rewind(nhfp->fpdef);
    }
}

#ifndef SFCTOOL
staticfn NHFILE *
viable_nhfile(NHFILE *nhfp)
{
    /* perform some sanity checks before returning
       the pointer to the nethack file descriptor */
    if (nhfp) {
         /* check for no open file at all,
          * not a structlevel legacy file,
          * nor a fieldlevel file.
          */
         if (((nhfp->fd == -1) && !nhfp->fpdef)
                || (nhfp->structlevel && nhfp->fd < 0)
                || (nhfp->fieldlevel && !nhfp->fpdef)) {
            /* not viable, start the cleanup */
            if (nhfp->fieldlevel) {
                if (nhfp->fpdef) {
                    (void) fclose(nhfp->fpdef);
                    nhfp->fpdef = (FILE *) 0;
                }
                if (nhfp->fplog) {
                    (void) fprintf(nhfp->fplog, "# closing, not viable\n");
                    (void) fclose(nhfp->fplog);
                }
                if (nhfp->fpdebug)
                    (void) fclose(nhfp->fpdebug);
            }
            free_nhfile(nhfp);
            nhfp = (NHFILE *) 0;
        }
    }
    return nhfp;
}
#endif /* !SFCTOOL */

int
nhclose(int fd)
{
    int retval = 0;

    if (fd >= 0) {
        if (close_check(fd))
            bclose(fd);
        else
            retval = close(fd);
    }
    return retval;
}

/* ----------  BEGIN LEVEL FILE HANDLING ----------- */

#ifndef SFCTOOL
/* Construct a file name for a level-type file, which is of the form
 * something.level (with any old level stripped off).
 * This assumes there is space on the end of 'file' to append
 * a two digit number.  This is true for 'level'
 * but be careful if you use it for other things -dgk
 */
void
set_levelfile_name(char *file, int lev)
{
    char *tf;

    tf = strrchr(file, '.');
    if (!tf)
        tf = eos(file);
    Sprintf(tf, ".%d", lev);
#ifdef VMS
    Strcat(tf, ";1");
#endif
    return;
}

NHFILE *
create_levelfile(int lev, char errbuf[])
{
    const char *fq_lock;
    NHFILE *nhfp = (NHFILE *) 0;

    if (errbuf)
        *errbuf = '\0';
    set_levelfile_name(gl.lock, lev);
    fq_lock = fqname(gl.lock, LEVELPREFIX, 0);

    nhfp = new_nhfile();
    if (nhfp) {
        nhfp->ftype = NHF_LEVELFILE;
        nhfp->mode = WRITING;
        nhfp->structlevel = TRUE; /* do set this TRUE for levelfiles */
        nhfp->fieldlevel = FALSE; /* don't set this TRUE for levelfiles */
        nhfp->addinfo = FALSE;
        nhfp->style.deflt = FALSE;
        nhfp->style.binary = TRUE;
        nhfp->fnidx = historical;
        nhfp->fd = -1;
        nhfp->fpdef = (FILE *) 0;
#if defined(MICRO) || defined(WIN32)
        /* Use O_TRUNC to force the file to be shortened if it already
         * exists and is currently longer.
         */
        nhfp->fd = open(fq_lock, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY,
                        FCMASK);
#else
#ifdef MAC68K
        nhfp->fd = maccreat(fq_lock, LEVL_TYPE);
#else
        nhfp->fd = creat(fq_lock, FCMASK);
#endif
#endif /* MICRO || WIN32 */

        if (nhfp->fd >= 0)
            svl.level_info[lev].flags |= LFILE_EXISTS;
        else if (errbuf) /* failure explanation */
            Sprintf(errbuf,
                    "Cannot create file \"%s\" for level %d (errno %d).",
                    gl.lock, lev, errno);
#if defined(MSDOS) || defined(WIN32)
        if (nhfp->fd >= 0)
            (void) setmode(nhfp->fd, O_BINARY);
#endif
    }
    nhfp = viable_nhfile(nhfp);
    return nhfp;
}

NHFILE *
open_levelfile(int lev, char errbuf[])
{
    const char *fq_lock;
    NHFILE *nhfp = (NHFILE *) 0;

    if (errbuf)
        *errbuf = '\0';
    set_levelfile_name(gl.lock, lev);
    fq_lock = fqname(gl.lock, LEVELPREFIX, 0);
    nhfp = new_nhfile();
    if (nhfp) {
        nhfp->mode = READING;
        nhfp->structlevel = TRUE; /* do set this TRUE for levelfiles */
        nhfp->fieldlevel = FALSE; /* do not set this TRUE for levelfiles */
        nhfp->addinfo = FALSE;
        nhfp->style.deflt = FALSE;
        nhfp->style.binary = TRUE;
        nhfp->ftype = NHF_LEVELFILE;
        nhfp->fnidx = historical;
        nhfp->fd = -1;
        nhfp->fpdef = (FILE *) 0;
    }
    if (nhfp && nhfp->structlevel) {
#ifdef MAC68K
        nhfp->fd = macopen(fq_lock, O_RDONLY | O_BINARY, LEVL_TYPE);
#else
        nhfp->fd = open(fq_lock, O_RDONLY | O_BINARY, 0);
#endif

        /* for failure, return an explanation that our caller can use;
           settle for `lock' instead of `fq_lock' because the latter
           might end up being too big for nethack's BUFSZ */
        if (nhfp->fd < 0 && errbuf)
            Sprintf(errbuf,
                    "Cannot open file \"%s\" for level %d (errno %d).",
                    gl.lock, lev, errno);
#if defined(MSDOS) || defined(WIN32)
        if (nhfp->fd >= 0)
            (void) setmode(nhfp->fd, O_BINARY);
#endif
    }
    nhfp = viable_nhfile(nhfp);
    return nhfp;
}

void
delete_levelfile(int lev)
{
    /*
     * Level 0 might be created by port specific code that doesn't
     * call create_levfile(), so always assume that it exists.
     */
    if (lev == 0 || (svl.level_info[lev].flags & LFILE_EXISTS)) {
        set_levelfile_name(gl.lock, lev);
        (void) unlink(fqname(gl.lock, LEVELPREFIX, 0));
        svl.level_info[lev].flags &= ~LFILE_EXISTS;
    }
}

void
clearlocks(void)
{
    int x;

#ifdef HANGUPHANDLING
    if (program_state.preserve_locks)
        return;
#endif
#ifndef NO_SIGNAL
    (void) signal(SIGINT, SIG_IGN);
#if defined(UNIX) || defined(VMS)
    sethanguphandler((void (*)(int)) SIG_IGN);
#endif
#endif /* NO_SIGNAL */
    /* can't access maxledgerno() before dungeons are created -dlc */
    for (x = (svn.n_dgns ? maxledgerno() : 0); x >= 0; x--)
        delete_levelfile(x); /* not all levels need be present */
}

#if defined(SELECTSAVED)
/* qsort comparison routine */
staticfn int QSORTCALLBACK
strcmp_wrap(const void *p, const void *q)
{
    return strcmp(*(char **) p, *(char **) q);
}
#endif

/* ----------  END LEVEL FILE HANDLING ----------- */

/* ----------  BEGIN BONES FILE HANDLING ----------- */

/* set up "file" to be file name for retrieving bones, and return a
 * bonesid to be read/written in the bones file.
 */
staticfn char *
set_bonesfile_name(char *file, d_level *lev)
{
    s_level *sptr;
    char *dptr;

    /*
     * "bonD0.nn"   = bones for level nn in the main dungeon;
     * "bonM0.T"    = bones for Minetown;
     * "bonQBar.n"  = bones for level n in the Barbarian quest;
     * "bon3D0.nn"  = \
     * "bon3M0.T"   =  > same as above, but for bones pool #3.
     * "bon3QBar.n" = /
     *
     * Return value for content validation skips "bon" and the
     * pool number (if present), making it feasible for the admin
     * to manually move a bones file from one pool to another by
     * renaming it.
     */
    Strcpy(file, "bon");
#ifdef SYSCF
    if (sysopt.bones_pools > 1) {
        unsigned poolnum = min((unsigned) sysopt.bones_pools, 10);

        poolnum = (unsigned) ubirthday % poolnum; /* 0..9 */
        Sprintf(eos(file), "%u", poolnum);
    }
#endif
    dptr = eos(file);
    /* when this naming scheme was adopted, 'filecode' was one letter;
       3.3.0 turned it into a three letter string for quest levels */
    Sprintf(dptr, "%c%s", svd.dungeons[lev->dnum].boneid,
            In_quest(lev) ? gu.urole.filecode : "0");
    if ((sptr = Is_special(lev)) != 0)
        Sprintf(eos(dptr), ".%c", sptr->boneid);
    else
        Sprintf(eos(dptr), ".%d", lev->dlevel);
#ifdef VMS
    Strcat(dptr, ";1");
#endif
    return dptr;
}

/* set up temporary file name for writing bones, to avoid another game's
 * trying to read from an uncompleted bones file.  we want an uncontentious
 * name, so use one in the namespace reserved for this game's level files.
 * (we are not reading or writing level files while writing bones files, so
 * the same array may be used instead of copying.)
 */
staticfn char *
set_bonestemp_name(void)
{
    char *tf;

    tf = strrchr(gl.lock, '.');
    if (!tf)
        tf = eos(gl.lock);
    Sprintf(tf, ".bn");
#ifdef VMS
    Strcat(tf, ";1");
#endif
    return gl.lock;
}

NHFILE *
create_bonesfile(d_level *lev, char **bonesid, char errbuf[])
{
    const char *file;
    NHFILE *nhfp = (NHFILE *) 0;
    int failed = 0;
#if defined(WIN32)
    errno_t err;
#endif

    if (errbuf)
        *errbuf = '\0';
    *bonesid = set_bonesfile_name(gb.bones, lev);
    file = set_bonestemp_name();
    file = fqname(file, BONESPREFIX, 0);

    nhfp = new_nhfile();
    if (nhfp) {
        nhfp->ftype = NHF_BONESFILE;
        nhfp->mode = WRITING;
        nhfp->structlevel = TRUE;
        nhfp->fieldlevel = FALSE;
        nhfp->addinfo = TRUE;
        nhfp->style.deflt = TRUE;
        nhfp->style.binary = TRUE;
        nhfp->fnidx = historical;
        nhfp->fd = -1;
        nhfp->fpdef = (FILE *) 0;
        if (nhfp->fpdef) {
#ifdef SAVEFILE_DEBUGGING
            nhfp->fpdebug = fopen("create_bonesfile-debug.log", "a");
#endif
        } else {
            failed = errno;
        }
        if (nhfp->structlevel) {
#if defined(MICRO)
            /* Use O_TRUNC to force the file to be shortened if it already
             * exists and is currently longer.
             */
            nhfp->fd = open(file,
                            O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, FCMASK);
#elif defined(WIN32)
            err = _sopen_s(&nhfp->fd, file,
                           O_WRONLY | O_CREAT | O_TRUNC | O_BINARY,
                           _SH_DENYRW, _S_IREAD | _S_IWRITE);
#else /* ?MICRO || WIN32 */
/* implies UNIX or MAC68K (MAC68K is for OS9 or earlier) */
#ifdef MAC68K
            nhfp->fd = maccreat(file, BONE_TYPE);
#else
            nhfp->fd = creat(file, FCMASK);
#endif  /* ?MAC68K */
#endif  /* ?MICRO || WIN32 */
            if (nhfp->fd < 0)
                failed = errno;
#if defined(MSDOS) || defined(WIN32)
            if (nhfp->fd >= 0)
                (void) setmode(nhfp->fd, O_BINARY);
#endif
        }
        if (failed && errbuf)  /* failure explanation */
            Sprintf(errbuf, "Cannot create bones \"%s\", id %s (errno %d).",
                    gl.lock, *bonesid, errno);
    }
#if defined(VMS) && !defined(SECURE)
    /*
       Re-protect bones file with world:read+write+execute+delete access.
       umask() doesn't seem very reliable; also, vaxcrtl won't let us set
       delete access without write access, which is what's really wanted.
       Can't simply create it with the desired protection because creat
       ANDs the mask with the user's default protection, which usually
       denies some or all access to world.
     */
    (void) chmod(file, FCMASK | 007); /* allow other users full access */
#endif /* VMS && !SECURE */

    nhfp = viable_nhfile(nhfp);
    return nhfp;
}

/* move completed bones file to proper name */
void
commit_bonesfile(d_level *lev)
{
    const char *fq_bones, *tempname;
    int ret;

    (void) set_bonesfile_name(gb.bones, lev);
    fq_bones = fqname(gb.bones, BONESPREFIX, 0);
    tempname = set_bonestemp_name();
    tempname = fqname(tempname, BONESPREFIX, 1);

#if (defined(SYSV) && !defined(SVR4)) || defined(GENIX)
    /* old SYSVs don't have rename.  Some SVR3's may, but since they
     * also have link/unlink, it doesn't matter. :-)
     */
    (void) unlink(fq_bones);
    ret = link(tempname, fq_bones);
    ret += unlink(tempname);
#else
    ret = rename(tempname, fq_bones);
#endif
    if (wizard && ret != 0)
        pline("couldn't rename %s to %s.", tempname, fq_bones);
}

NHFILE *
open_bonesfile(d_level *lev, char **bonesid)
{
    const char *fq_bones;
    NHFILE *nhfp = (NHFILE *) 0;
#if defined(WIN32)
    errno_t err UNUSED;
#endif

    *bonesid = set_bonesfile_name(gb.bones, lev);
    fq_bones = fqname(gb.bones, BONESPREFIX, 0);
    nh_uncompress(fq_bones);  /* no effect if nonexistent */

    nhfp = new_nhfile();
    if (nhfp) {
#if defined(WIN32) && defined(DEBUG)
        if (nhfp->fd >= 0)
            impossible("bones file NHFILE * has odd fd (%d)", nhfp->fd);
#endif
        nhfp->structlevel = TRUE;
        nhfp->fieldlevel = FALSE;
        nhfp->ftype = NHF_BONESFILE;
        nhfp->mode = READING;
        nhfp->addinfo = TRUE;
        nhfp->style.deflt = TRUE;
        nhfp->style.binary = (sysopt.bonesformat[0] != exportascii);
        nhfp->fnidx = sysopt.bonesformat[0];
        nhfp->fd = -1;
        nhfp->fpdef = (FILE *) 0;
        if (nhfp->fpdef) {
#ifdef SAVEFILE_DEBUGGING
            nhfp->fpdebug = fopen("open_bonesfile-debug.log", "a");
#endif
        }
        if (nhfp->structlevel) {
#if defined(MAC68K)
            nhfp->fd = macopen(fq_bones, O_RDONLY | O_BINARY, BONE_TYPE);
#elif defined(WIN32)
            err = _sopen_s(&nhfp->fd, fq_bones, _O_RDONLY | _O_BINARY,
                           _SH_DENYRW, _S_IREAD | _S_IWRITE);
#else
            nhfp->fd = open(fq_bones, O_RDONLY | O_BINARY, 0);
#endif
#if defined(MSDOS) || defined(WIN32)
            if (nhfp->fd >= 0)
                (void) setmode(nhfp->fd, O_BINARY);
#endif
        }
    }
    nhfp = viable_nhfile(nhfp);
    return nhfp;
}

int
delete_bonesfile(d_level *lev)
{
    int reslt;

    (void) set_bonesfile_name(gb.bones, lev);
    reslt = unlink(fqname(gb.bones, BONESPREFIX, 0));
    delete_convertedfile(fqname(gb.bones, BONESPREFIX, 0));
    return !(reslt < 0);
}

/* assume we're compressing the recently read or created bonesfile, so the
 * file name is already set properly */
void
compress_bonesfile(void)
{
    nh_sfconvert(fqname(gb.bones, BONESPREFIX, 0));
    nh_compress(fqname(gb.bones, BONESPREFIX, 0));
}
#endif /* !SFCTOOL */

/* ----------  END BONES FILE HANDLING ----------- */

/* ----------  BEGIN SAVE FILE HANDLING ----------- */

/* set savefile name in OS-dependent manner from pre-existing svp.plname,
 * avoiding troublesome characters */
void
set_savefile_name(boolean regularize_it)
{
    int regoffset = 0, overflow = 0,
        indicator_spot = 0; /* 0=no indicator, 1=before ext, 2=after ext */
    const char *postappend = (const char *) 0,
               *sfindicator = (const char *) 0;
#if defined(WIN32)
    char tmp[BUFSZ];
#endif

#ifdef VMS
    Sprintf(gs.SAVEF, "[.save]%d%s", getuid(), svp.plname);
    regoffset = 7;
    indicator_spot = 1;
    postappend = ";1";
#endif
#if defined(WIN32)
    if (regularize_it) {
        static const char okchars[]
            = "*ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_-.";
        const char *legal = okchars;

        ++legal; /* skip '*' wildcard character */
        (void) fname_encode(legal, '%', svp.plname, tmp, sizeof tmp);
    } else {
        Sprintf(tmp, "%s", svp.plname);
    }
    if (strlen(tmp) < (SAVESIZE - 1))
        Strcpy(gs.SAVEF, tmp);
    else
        overflow = 1;
    indicator_spot = 1;
    regularize_it = FALSE;
#endif
#ifdef UNIX
    Sprintf(gs.SAVEF, "save/%d%s", (int) getuid(), svp.plname);
    regoffset = 5;
    indicator_spot = 2;
#endif
#if defined(MSDOS)
    if (strlen(gs.SAVEP) < (SAVESIZE - 1))
        Strcpy(gs.SAVEF, gs.SAVEP);
    if (strlen(gs.SAVEF) < (SAVESIZE - 1))
        (void) strncat(gs.SAVEF, svp.plname, (SAVESIZE - strlen(gs.SAVEF)));
#endif
#if defined(MAC68K)
    /* Mac: "<plname>.sav" (extension added below); macfile.c puts it
       in the Saves folder */
    Sprintf(gs.SAVEF, "%.*s", MAC_FNAME_PLMAX, svp.plname);
#elif defined(MICRO) && !defined(WIN32) && !defined(MSDOS)
    if (strlen(gs.SAVEP) < (SAVESIZE - 1))
        Strcpy(gs.SAVEF, gs.SAVEP);
    {
        int i = strlen(gs.SAVEP);
#ifdef AMIGA
        /* svp.plname has to share space with gs.SAVEP and ".sav" */
        (void) strncat(gs.SAVEF, svp.plname,
                       FILENAME - i - strlen(SAVE_EXTENSION));
#else
        (void) strncat(gs.SAVEF, svp.plname, 8);
#endif
        regoffset = i;
    }
#endif /* MICRO */

    if (regularize_it)
         regularize(gs.SAVEF + regoffset);
    if (indicator_spot == 1 && sfindicator && !overflow) {
        if (strlen(gs.SAVEF) + strlen(sfindicator) < (SAVESIZE - 1))
            Strcat(gs.SAVEF, sfindicator);
        else
            overflow = 2;
    }
#ifdef SAVE_EXTENSION
    /* (0) is placed in brackets below so that the [&& !overflow] is
       explicit dead code (the ">" comparison is detected as always
       FALSE at compile-time). Done to appease clang's -Wunreachable-code */
    if (strlen(SAVE_EXTENSION) > (0) && !overflow) {
        if (strlen(gs.SAVEF) + strlen(SAVE_EXTENSION) < (SAVESIZE - 1)) {
            Strcat(gs.SAVEF, SAVE_EXTENSION);
        } else
            overflow = 3;
    }
#endif
    if (indicator_spot == 2 && sfindicator && !overflow) {
        if (strlen(gs.SAVEF) + strlen(sfindicator) < (SAVESIZE - 1))
           Strcat(gs.SAVEF, sfindicator);
        else
            overflow = 4;
    }
    if (postappend && !overflow) {
        if (strlen(gs.SAVEF) + strlen(postappend) < (SAVESIZE - 1))
            Strcat(gs.SAVEF, postappend);
        else
            overflow = 5;
    }
#if (NH_DEVEL_STATUS != NH_STATUS_RELEASED)
    if (overflow)
        impossible("set_savefile_name() couldn't complete"
                   " without overflow %d",
                   overflow);
#endif
}

#ifndef SFCTOOL
#ifdef INSURANCE
void
save_savefile_name(NHFILE *nhfp)
{
    Sfo_char(nhfp, gs.SAVEF, "savefile_name", sizeof(gs.SAVEF));
}
#endif

#ifndef MICRO
/* change pre-existing savefile name to indicate an error savefile */
void
set_error_savefile(void)
{
#ifdef VMS
    {
        char *semi_colon = strrchr(gs.SAVEF, ';');

        if (semi_colon)
            *semi_colon = '\0';
    }
    Strcat(gs.SAVEF, ".e;1");
#else
#ifdef MAC68K
    {
        /* swap ".sav" for ".err": same length, still in the Saves folder */
        size_t n = strlen(gs.SAVEF), e = strlen(SAVE_EXTENSION);

        if (n >= e && !strcmp(gs.SAVEF + n - e, SAVE_EXTENSION))
            gs.SAVEF[n - e] = '\0';
        Strcat(gs.SAVEF, MAC_ERRSAVE_EXT);
    }
#else
    Strcat(gs.SAVEF, ".e");
#endif
#endif
}
#endif

/* create save file, overwriting one if it already exists */
NHFILE *
create_savefile(void)
{
    const char *fq_save;
    NHFILE *nhfp = (NHFILE *) 0;
    boolean do_historical = TRUE;

    fq_save = fqname(gs.SAVEF, SAVEPREFIX, 0);
    nhfp = new_nhfile();
    if (nhfp) {
        nhfp->ftype = NHF_SAVEFILE;
        nhfp->mode = WRITING;
        if (program_state.in_self_recover || do_historical) {
            nhUse(do_historical);
            nhfp->structlevel = TRUE;
            nhfp->fieldlevel = FALSE;
            nhfp->addinfo = FALSE;
            nhfp->style.deflt = FALSE;
            nhfp->style.binary = TRUE;
            nhfp->fnidx = historical;
            nhfp->fd = -1;
            nhfp->fpdef = (FILE *) 0;
#ifdef SAVEFILE_DEBUGGING
            nhfp->fplog = fopen("create-savefile.log", "w");
#endif
#if defined(MICRO) || defined(WIN32)
            nhfp->fd = open(fq_save, O_WRONLY | O_BINARY | O_CREAT | O_TRUNC,
                            FCMASK);
#else /* !MICRO && !WIN32 */
/* UNIX || MAC68K implied (MAC68K is OS9 or earlier only) */
#ifdef MAC68K
            nhfp->fd = maccreat(fq_save, SAVE_TYPE);
#else
            nhfp->fd = creat(fq_save, FCMASK);
#endif
#endif /* MICRO || WIN32 */
#if defined(MSDOS) || defined(WIN32)
            if (nhfp->fd >= 0)
                (void) setmode(nhfp->fd, O_BINARY);
#endif
        }
    }
#if defined(VMS) && !defined(SECURE)
    /*
       Make sure the save file is owned by the current process.  That's
       the default for non-privileged users, but for priv'd users the
       file will be owned by the directory's owner instead of the user.
    */
#undef getuid
    (void) chown(fq_save, getuid(), getgid());
#define getuid() vms_getuid()
#endif /* VMS && !SECURE */

    nhfp = viable_nhfile(nhfp);
    return nhfp;
}

/* open savefile for reading */
NHFILE *
open_savefile(void)
{
    const char *fq_save;
    NHFILE *nhfp = (NHFILE *) 0;
    boolean do_historical = TRUE;

    fq_save = fqname(gs.SAVEF, SAVEPREFIX, 0);
    nhfp = new_nhfile();
    if (nhfp) {
        nhfp->ftype = NHF_SAVEFILE;
        nhfp->mode = READING;
        if (program_state.in_self_recover || do_historical) {
            do_historical = TRUE;       /* force it */
            nhUse(do_historical);
            nhfp->structlevel = TRUE;
            nhfp->fieldlevel = FALSE;
            nhfp->addinfo = FALSE;
            nhfp->style.deflt = FALSE;
            nhfp->style.binary = TRUE;
            nhfp->fnidx = historical;
            nhfp->fd = -1;
            nhfp->fpdef = (FILE *) 0;
#ifdef SAVEFILE_DEBUGGING
            nhfp->fplog = fopen("open-savefile.log", "w");
#endif
        }
#ifdef MAC68K
        nhfp->fd = macopen(fq_save, O_RDONLY | O_BINARY, SAVE_TYPE);
#else
        nhfp->fd = open(fq_save, O_RDONLY | O_BINARY, 0);
#endif
#if defined(MSDOS) || defined(WIN32)
        if (nhfp->fd >= 0)
            (void) setmode(nhfp->fd, O_BINARY);
#endif
    }
    nhfp = viable_nhfile(nhfp);
    return nhfp;
}

/* delete savefile */
int
delete_savefile(void)
{
    const char *sfname = fqname(gs.SAVEF, SAVEPREFIX, 0);

    (void) unlink(sfname);
    (void) delete_convertedfile(sfname);
    return 0; /* for restore_saved_game() (ex-xxxmain.c) test */
}

/* try to open up a save file and prepare to restore it */
NHFILE *
restore_saved_game(void)
{
    const char *fq_save;
    NHFILE *nhfp = (NHFILE *) 0;
    int sfstatus = 0;

    set_savefile_name(TRUE);
    fq_save = fqname(gs.SAVEF, SAVEPREFIX, 0);

    nh_uncompress(fq_save);
    if ((nhfp = open_savefile()) != 0) {
        if ((sfstatus = validate(nhfp, (char *) 0, TRUE, UTD_QUIETLY))
            != SF_UPTODATE) {
            problematic_savefile(nhfp, sfstatus, fq_save);
        }
    }
    return nhfp;
}

/*
 * This doesn't open any files. It provides a valid (NHFILE *)
 * to provide to functions that take one as a parameter, with
 * only the FREEING bit set.
 *
 * close_nhfile() can, and should, be called on the returned
 * (NHFILE *), and it will handle it correctly.
 */

NHFILE *
get_freeing_nhfile(void)
{
    NHFILE *nhfp = (NHFILE *) 0;

    nhfp = new_nhfile(); /* also sets fd to -1 */
    if (nhfp) {
        nhfp->mode = FREEING;
    }
    return nhfp;
}

/* called if there is no save file for current character */
int
check_panic_save(void)
{
    int result = 0;
#ifdef CHECK_PANIC_SAVE
    FILE *cf;
    const char *savef;

    set_error_savefile();
    savef = fqname(gs.SAVEF, SAVEPREFIX, 0);

    /*
     * This duplicates part of docompress_file().
     * We don't want to start by uncompressing just to check for the
     * file's existence and then have to recompress it.
     */

#ifdef COMPRESS_EXTENSION
    unsigned ln = (unsigned) (strlen(savef) + strlen(COMPRESS_EXTENSION));
    char *cfn = (char *) alloc(ln + 1);

    Strcpy(cfn, savef);
    Strcat(cfn, COMPRESS_EXTENSION);
    if ((cf = fopen(cfn, RDBMODE)) != NULL) {
        (void) fclose(cf);
        result = 1;
    }
    free((genericptr_t) cfn);
#endif /* COMPRESS_EXTENSION */

    if (!result) {
        /* maybe it has already been manually uncompressed */
        if ((cf = fopen(savef, RDBMODE)) != NULL) {
            (void) fclose(cf);
            result = 1;
        }
    }

    set_savefile_name(TRUE); /* reset to normal */
#endif /* CHECK_PANIC_SAVE */
    return result;
}

#if defined(SELECTSAVED)

char *
plname_from_file(
    const char *filename,
    boolean without_wait_synch_per_file, int additional_utd_flags)
{
    NHFILE *nhfp;
    unsigned ln;
    char *result = 0;
    int sfstatus = 0;

    Strcpy(gs.SAVEF, filename);
#ifdef COMPRESS_EXTENSION
    {
        /* if COMPRESS_EXTENSION is present, strip it off */
        int sln = (int) strlen(gs.SAVEF),
            xln = (int) strlen(COMPRESS_EXTENSION);

        if (sln > xln && !strcmp(&gs.SAVEF[sln - xln], COMPRESS_EXTENSION))
            gs.SAVEF[sln - xln] = '\0';
    }
#endif
    nh_uncompress(gs.SAVEF);
    if ((nhfp = open_savefile()) != 0) {
        if ((sfstatus = validate(nhfp, filename,
                                without_wait_synch_per_file,
                                additional_utd_flags)) == SF_UPTODATE) {
            /* room for "name+role+race+gend+algn X" where the space before
               X is actually NUL and X is playmode: one of '-', 'X', or 'D' */
            ln = (unsigned) PL_NSIZ_PLUS;
            result = memset((genericptr_t) alloc(ln), '\0', ln);
            get_plname_from_file(nhfp, result, FALSE);
        }
        close_nhfile(nhfp);
    }
    nh_compress(gs.SAVEF);
    return result; /* file's plname[]+playmode value */
}
#endif /* defined(SELECTSAVED) */

#define SUPPRESS_WAITSYNCH_PERFILE TRUE
#define ALLOW_WAITSYNCH_PERFILE FALSE

/* get list of saved games owned by current user */
char **
get_saved_games(void)
{
    char **result = NULL;
#if defined(SELECTSAVED)
#if defined(WIN32) || defined(UNIX)
    int n;
#endif
    int j = 0;

#ifdef WIN32
    {
        char *foundfile;
        const char *fq_save;
#if 0
        const char *fq_new_save;
        const char *fq_old_save;
#endif
        char **files = 0;
        int i, count_failures = 0, utd_flags_to_pass_downstream = 0;

        Strcpy(svp.plname, "*");
        set_savefile_name(FALSE);
#if defined(ZLIB_COMP)
        Strcat(gs.SAVEF, COMPRESS_EXTENSION);
#endif
        fq_save = fqname(gs.SAVEF, SAVEPREFIX, 0);

        n = 0;
        foundfile = foundfile_buffer();
        if (findfirst((char *) fq_save)) {
            do {
                ++n;
            } while (findnext());
        }

        if (n > 0) {
            files = (char **) alloc((n + 1) * sizeof (char *)); /* at most */
            (void) memset((genericptr_t) files, 0, (n + 1) * sizeof (char *));
            if (findfirst((char *) fq_save)) {
                i = 0;
                do {
                    files[i++] = dupstr(foundfile);
                } while (findnext());
            }
        }

        if (n > 0) {
            result = (char **) alloc((n + 1) * sizeof (char *)); /* at most */
            (void) memset((genericptr_t) result, 0, (n + 1) * sizeof (char *));
            for(i = 0; i < n; i++) {
                char *r;
                if (!wizard)
                    utd_flags_to_pass_downstream = UTD_QUIETLY;
                r = plname_from_file(files[i],
                                     SUPPRESS_WAITSYNCH_PERFILE,
                                     utd_flags_to_pass_downstream);

                if (r) {
                    /* this renaming of the savefile is not compatible
                     * with 1f36b98b,  'selectsaved' extension from
                     * Oct 10, 2024. Disable the renaming for the time
                     * being.
                     */
#if 0
                    /* rename file if it is not named as expected */
                    Strcpy(svp.plname, r);
                    set_savefile_name(TRUE);
                    fq_new_save = fqname(gs.SAVEF, SAVEPREFIX, 0);
                    fq_old_save = fqname(files[i], SAVEPREFIX, 1);

                    if (strcmp(fq_old_save, fq_new_save) != 0
                        && !file_exists(fq_new_save))
                        (void) rename(fq_old_save, fq_new_save);
#endif
                    result[j++] = r;
                } else {
                    count_failures++;
                }
            }
        }

        free_saved_games(files);
        if (count_failures && !(utd_flags_to_pass_downstream & UTD_QUIETLY))
            wait_synch();
    }
#endif /* WIN32 */
#ifdef UNIX
    /* posixly correct version */
    int myuid = getuid();
    DIR *dir;

    if ((dir = opendir(fqname("save", SAVEPREFIX, 0)))) {
        for (n = 0; readdir(dir); n++)
            ;
        closedir(dir);
        if (n > 0) {
            int i;

            if (!(dir = opendir(fqname("save", SAVEPREFIX, 0))))
                return 0;
            result = (char **) alloc((n + 1) * sizeof(char *)); /* at most */
            (void) memset((genericptr_t) result, 0, (n + 1) * sizeof(char *));
            for (i = 0, j = 0; i < n; i++) {
                int uid;
                char name[64]; /* more than PL_NSIZ+1 */
                struct dirent *entry = readdir(dir);

                if (!entry)
                    break;
                if (sscanf(entry->d_name, "%d%63s", &uid, name) == 2) {
                    if (uid == myuid) {
                        char filename[BUFSZ];
                        char *r;

                        Sprintf(filename, "save/%d%s", uid, name);
                        r = plname_from_file(filename,
                                             ALLOW_WAITSYNCH_PERFILE, 0);
                        if (r)
                            result[j++] = r;
                    }
                }
            }
            closedir(dir);
        }
    }
#endif /* UNIX */
#ifdef VMS
    Strcpy(svp.plname, "*");
    set_savefile_name(FALSE);
    j = vms_get_saved_games(gs.SAVEF, &result);
#endif /* VMS */

    if (j > 0) {
        if (j > 1)
            qsort(result, j, sizeof (char *), strcmp_wrap);
        result[j] = (char *) NULL;
    } else if (result) { /* could happen if save files are obsolete */
        free_saved_games(result);
        result = (char **) NULL;
    }
#endif /* SELECTSAVED */

    return result;
}
#undef SUPPRESS_WAITSYNCH_PERFILE
#undef ALLOW_WAITSYNCH_PERFILE

void
free_saved_games(char **saved)
{
    if (saved) {
        int i;

        for (i = 0; saved[i]; ++i)
            free((genericptr_t) saved[i]);
        free((genericptr_t) saved);
    }
}

#endif /* !SFCTOOL */

/* ----------  END SAVE FILE HANDLING ----------- */

/* ----------  BEGIN FILE COMPRESSION HANDLING ----------- */

#ifdef COMPRESS /* external compression */

staticfn void
redirect(
    const char *filename,
    const char *mode,
    FILE *stream,
    boolean uncomp)
{
    if (freopen(filename, mode, stream) == (FILE *) 0) {
        const char *details;

#if defined(NHSTDC) && !defined(NOTSTDC)
        if ((details = strerror(errno)) == 0)
#endif
            details = "";
        (void) fprintf(stderr,
                       "freopen of %s for %scompress failed; (%d) %s\n",
                       filename, uncomp ? "un" : "", errno, details);
        nh_terminate(EXIT_FAILURE);
    }
}

/*
 * using system() is simpler, but opens up security holes and causes
 * problems on at least Interactive UNIX 3.0.1 (SVR3.2), where any
 * setuid is renounced by /bin/sh, so the files cannot be accessed.
 *
 * cf. child() in unixunix.c.
 */
staticfn void
docompress_file(const char *filename, boolean uncomp)
{
    char *cfn = 0;
    const char *xtra;
    FILE *cf;
    const char *args[10];
#ifdef COMPRESS_OPTIONS
    char opts[sizeof COMPRESS_OPTIONS];
#endif
    int i = 0;
    int f, childstatus;
    unsigned ln;
#ifdef TTY_GRAPHICS
    boolean istty = WINDOWPORT(tty);
#endif

#ifdef COMPRESS_EXTENSION
    xtra = COMPRESS_EXTENSION;
#else
    xtra = "";
#endif
#ifdef SFCTOOL
    ln = strlen(filename) + sizeof COMPRESS_EXTENSION;
    cfn = (char *) alloc(ln);
#else /* SFCTOOL */
    ln = (unsigned) (strlen(filename) + strlen(xtra));
    cfn = (char *) alloc(ln + 1);
#endif /* SFCTOOL */

    Strcpy(cfn, filename);
    Strcat(cfn, xtra);

    /* when compressing, we know the file exists */
    if (uncomp) {
        if ((cf = fopen(cfn, RDBMODE)) == (FILE *) 0) {
            free((genericptr_t) cfn);
            return;
        }
        (void) fclose(cf);
    }

    args[0] = COMPRESS;
    if (uncomp)
        args[++i] = "-d"; /* uncompress */
#ifdef COMPRESS_OPTIONS
    {
        /* we can't guarantee there's only one additional option, sigh */
        char *opt;
        boolean inword = FALSE;

        opt = strcpy(opts, COMPRESS_OPTIONS);
        while (*opt) {
            if ((*opt == ' ') || (*opt == '\t')) {
                if (inword) {
                    *opt = '\0';
                    inword = FALSE;
                }
            } else if (!inword) {
                args[++i] = opt;
                inword = TRUE;
            }
            opt++;
        }
    }
#endif
    args[++i] = (char *) 0;

#ifdef TTY_GRAPHICS
    /* If we don't do this and we are right after a y/n question *and*
     * there is an error message from the compression, the 'y' or 'n' can
     * end up being displayed after the error message.
     */
    if (istty) {
        mark_synch();
    }
#endif
    f = fork();
    if (f == 0) { /* child */
#ifdef TTY_GRAPHICS
        /* any error messages from the compression must come out after
         * the first line, because the more() to let the user read
         * them will have to clear the first line.  This should be
         * invisible if there are no error messages.
         */
        if (istty) {
            raw_print("");
        }
#endif
        /* run compressor without privileges, in case other programs
         * have surprises along the line of gzip once taking filenames
         * in GZIP.
         */
        /* assume all compressors will compress stdin to stdout
         * without explicit filenames.  this is true of at least
         * compress and gzip, those mentioned in config.h.
         */
        if (uncomp) {
            redirect(cfn, RDBMODE, stdin, uncomp);
            redirect(filename, WRBMODE, stdout, uncomp);
        } else {
            redirect(filename, RDBMODE, stdin, uncomp);
            redirect(cfn, WRBMODE, stdout, uncomp);
        }
        (void) setgid(getgid());
        (void) setuid(getuid());
        (void) execv(args[0], (char *const *) args);
        perror((char *) 0);
        (void) fprintf(stderr, "Exec to %scompress %s failed.\n",
                       uncomp ? "un" : "", filename);
        free((genericptr_t) cfn);
        nh_terminate(EXIT_FAILURE);
    } else if (f == -1) {
        perror((char *) 0);
        pline("Fork to %scompress %s failed.", uncomp ? "un" : "", filename);
        free((genericptr_t) cfn);
        return;
    }

    /*
     * back in parent...
     */
#ifndef NO_SIGNAL
    childstatus = 1; /* wait() should update this, ideally setting it to 0 */
    (void) signal(SIGINT, SIG_IGN);
    (void) signal(SIGQUIT, SIG_IGN);
    errno = 0; /* avoid stale details if wait() doesn't set errno */
    /* wait() returns child's pid and sets 'childstatus' to child's
       exit status, or returns -1 and leaves 'childstatus' unmodified */
    if ((long) wait((int *) &childstatus) == -1L) {
        char numbuf[40];
        const char *details = strerror(errno);

        if (!details) {
            Sprintf(numbuf, "(%d)", errno);
            details = numbuf;
        }
        raw_printf("Wait when %scompressing %s failed; %s.",
                   uncomp ? "un" : "", filename, details);
    }
    (void) signal(SIGINT, (SIG_RET_TYPE) done1);
    if (wizard)
        (void) signal(SIGQUIT, SIG_DFL);
#else
    /* I don't think we can really cope with external compression
     * without signals, so we'll declare that compress failed and
     * go on.  (We could do a better job by forcing off external
     * compression if there are no signals, but we want this for
     * testing with FailSafeC
     */
    childstatus = 1; /* non-zero => failure */
#endif
    if (childstatus == 0) {
        /* (un)compress succeeded: remove file left behind */
        if (uncomp)
            (void) unlink(cfn);
        else
            (void) unlink(filename);
    } else {
        /* (un)compress failed; remove the new, bad file */
        if (uncomp) {
            raw_printf("Unable to uncompress %s", filename);
            (void) unlink(filename);
        } else {
            /* no message needed for compress case; life will go on */
            (void) unlink(cfn);
        }
#ifdef TTY_GRAPHICS
        /* Give them a chance to read any error messages from the
         * compression--these would go to stdout or stderr and would get
         * overwritten only in tty mode.  It's still ugly, since the
         * messages are being written on top of the screen, but at least
         * the user can read them.
         */
        if (istty && iflags.window_inited) {
            clear_nhwindow(WIN_MESSAGE);
            more();
            /* No way to know if this is feasible */
            /* doredraw(); */
        }
#endif
    }

    free((genericptr_t) cfn);
    return;
}

#endif /* COMPRESS : external compression */

#if defined(COMPRESS) || defined(ZLIB_COMP)
#define UNUSED_if_not_COMPRESS /*empty*/
#else
#define UNUSED_if_not_COMPRESS UNUSED
#endif

/* compress file */
void
nh_compress(const char *filename UNUSED_if_not_COMPRESS)
{
#if defined(COMPRESS) || defined(ZLIB_COMP)
    docompress_file(filename, FALSE);
#endif
}

/* uncompress file if it exists */
void
nh_uncompress(const char *filename UNUSED_if_not_COMPRESS)
{
#if defined(COMPRESS) || defined(ZLIB_COMP)
    docompress_file(filename, TRUE);
#endif
}

#ifdef ZLIB_COMP /* RLC 09 Mar 1999: Support internal ZLIB */
staticfn boolean
make_compressed_name(const char *filename, char *cfn)
{
#ifndef SHORT_FILENAMES
    /* Assume free-form filename with no 8.3 restrictions */
    strcpy(cfn, filename);
    strcat(cfn, COMPRESS_EXTENSION);
    return TRUE;
#else
#ifdef SAVE_EXTENSION
    char *bp = (char *) 0;

    strcpy(cfn, filename);
    if ((bp = strstri(cfn, SAVE_EXTENSION))) {
        strsubst(bp, SAVE_EXTENSION, ".saz");
        return TRUE;
    } else {
        /* find last occurrence of bon */
        bp = eos(cfn);
        while (bp-- > cfn) {
            if (strstri(bp, "bon")) {
                strsubst(bp, "bon", "boz");
                return TRUE;
            }
        }
    }
#endif /* SAVE_EXTENSION */
    return FALSE;
#endif /* SHORT_FILENAMES */
}

staticfn void
docompress_file(const char *filename, boolean uncomp)
{
    gzFile compressedfile;
    FILE *uncompressedfile;
#ifndef SFCTOOL
    char cfn[256];
#else
    char *cfn;
#endif
    char buf[1024];
    unsigned len, len2;

#ifdef SFCTOOL
    cfn = (char *) alloc(strlen(filename) + strlen(COMPRESS_EXTENSION) + 1);
#endif
    if (!make_compressed_name(filename, cfn))
        return;

    if (!uncomp) {
        /* Open the input and output files */
        /* Note that gzopen takes "wb" as its mode, even on systems where
           fopen takes "r" and "w" */

        uncompressedfile = fopen(filename, RDBMODE);
        if (!uncompressedfile) {
            pline("Error in zlib docompress_file %s", filename);
            return;
        }
        compressedfile = gzopen(cfn, "wb");
        if (compressedfile == NULL) {
            if (errno == 0) {
                pline("zlib failed to allocate memory");
            } else {
                panic("Error in docompress_file %d", errno);
            }
#ifdef SFCTOOL
            free(cfn);
#endif
            fclose(uncompressedfile);
            return;
        }

        /* Copy from the uncompressed to the compressed file */

        while (1) {
            len = fread(buf, 1, sizeof(buf), uncompressedfile);
            if (ferror(uncompressedfile)) {
                pline("Failure reading uncompressed file");
                pline("Can't compress %s.", filename);
                fclose(uncompressedfile);
                gzclose(compressedfile);
                (void) unlink(cfn);
#ifdef SFCTOOL
                free(cfn);
#endif
                return;
            }
            if (len == 0)
                break; /* End of file */

            len2 = gzwrite(compressedfile, buf, len);
            if (len2 == 0) {
                pline("Failure writing compressed file");
                pline("Can't compress %s.", filename);
                fclose(uncompressedfile);
                gzclose(compressedfile);
                (void) unlink(cfn);
#ifdef SFCTOOL
                free(cfn);
#endif
                return;
            }
        }

        fclose(uncompressedfile);
        gzclose(compressedfile);

        /* Delete the file left behind */

        (void) unlink(filename);

    } else { /* uncomp */

        /* Open the input and output files */
        /* Note that gzopen takes "rb" as its mode, even on systems where
           fopen takes "r" and "w" */

        compressedfile = gzopen(cfn, "rb");
        if (compressedfile == NULL) {
            if (errno == 0) {
                pline("zlib failed to allocate memory");
            } else if (errno != ENOENT) {
                panic("Error in zlib docompress_file %s, %d", filename,
                      errno);
            }
#ifdef SFCTOOL
            free(cfn);
#endif
            return;
        }
        uncompressedfile = fopen(filename, WRBMODE);
        if (!uncompressedfile) {
            pline("Error in zlib docompress file uncompress %s", filename);
            gzclose(compressedfile);
#ifdef SFCTOOL
            free(cfn);
#endif
            return;
        }

        /* Copy from the compressed to the uncompressed file */

        while (1) {
            len = gzread(compressedfile, buf, sizeof(buf));
            if (len == (unsigned) -1) {
                pline("Failure reading compressed file");
                pline("Can't uncompress %s.", filename);
                fclose(uncompressedfile);
                gzclose(compressedfile);
                (void) unlink(filename);
#ifdef SFCTOOL
                free(cfn);
#endif
                return;
            }
            if (len == 0)
                break; /* End of file */

            fwrite(buf, 1, len, uncompressedfile);
            if (ferror(uncompressedfile)) {
                pline("Failure writing uncompressed file");
                pline("Can't uncompress %s.", filename);
                fclose(uncompressedfile);
                gzclose(compressedfile);
                (void) unlink(filename);
#ifdef SFCTOOL
                free(cfn);
#endif
                return;
            }
        }

        fclose(uncompressedfile);
        gzclose(compressedfile);

        /* Delete the file left behind */
        (void) unlink(cfn);
    }
#ifdef SFCTOOL
    free(cfn);
#endif
}
#endif /* RLC 09 Mar 1999: End ZLIB patch */

#undef UNUSED_if_not_COMPRESS

/* ----------  END FILE COMPRESSION HANDLING ----------- */


/* ----------  BEGIN PROBLEMATIC SAVEFILE HANDLING ----------- */
#ifndef SFCTOOL
static struct sfstatus_to_msg {
    int sfstatus;
    const char *msg;
} sf2msg[] = {
    { SF_UPTODATE, "everything matches" },
    { SF_OUTDATED, "version or revision mismatch" },
    { SF_CRITICAL_BYTE_COUNT_MISMATCH,
        "savefile critical byte-count mismatch" },
    { SF_DM_IL32LLP64_ON_ILP32LL64, "Windows x64 savefile on x86" },
    { SF_DM_I32LP64_ON_ILP32LL64, "Unix 64 savefile on x86" },
    { SF_DM_ILP32LL64_ON_I32LP64, "x86 savefile on Unix 64" },
    { SF_DM_ILP32LL64_ON_IL32LLP64, "x86 savefile on Windows x64" },
    { SF_DM_I32LP64_ON_IL32LLP64, "Unix 64 savefile on Windows x64" },
    { SF_DM_IL32LLP64_ON_I32LP64, "Windows x64 savefile on Unix 64" },
    { SF_DM_MISMATCH, "generic savefile mismatch" },
};

/* keep an existing save after a refusal or an interrupted restore */
staticfn void
preserve_savefile(void)
{
    const char *fq_save = fqname(gs.SAVEF, SAVEPREFIX, 1);

    program_state.something_worth_saving = 0;
#if defined(UNIX) || defined(VMS)
    (void) chmod(fq_save, FCMASK);
#endif
    nh_compress(fq_save);
}

ATTRNORETURN void
refuse_saved_game(const char *why)
{
#ifndef NO_SIGNAL
    (void) signal(SIGINT, SIG_IGN);
#if defined(UNIX) || defined(VMS)
    sethanguphandler((void (*)(int)) SIG_IGN);
#endif
#endif
    preserve_savefile();
    clearlocks();
    pline("%s", why);
    pline("The saved game has been kept; please tell the operator.");
    display_nhwindow(WIN_MESSAGE, TRUE);
    exit_nhwindows((char *) 0);
    nh_terminate(EXIT_FAILURE);
}

ATTRNORETURN staticfn void
problematic_savefile(NHFILE *nhfp, int sfstatus, const char *savefilenm)
{
    int i;
    const char *why = "incompatible save file";
    char buf[BUFSZ];

    for (i = 0; i < SIZE(sf2msg); ++i) {
        if (sf2msg[i].sfstatus == sfstatus) {
            why = sf2msg[i].msg;
            break;
        }
    }
    if (*nhfp->validation_error)
        why = nhfp->validation_error;
    Snprintf(buf, sizeof buf, "Cannot restore %s: %s.", savefilenm, why);
    close_nhfile(nhfp);
    refuse_saved_game(buf);
}
#endif /* !SFCTOOL */

/* ----------  END PROBLEMATIC SAVEFILE HANDLING ----------- */

/* ----------  BEGIN EXTERNAL CONVERSION HANDLING ----------- */

/* static boolean cvtinit = FALSE; */

#ifndef SFCTOOL
static char *unconverted_filename = 0, *converted_filename = 0;

/*
 * Returns non-zero if unconvert was successful
 */
staticfn int
doconvert_file(const char *filename, int sfstatus, boolean unconvert)
{
    nhUse(filename);
    nhUse(sfstatus);
    nhUse(unconvert);
    return 1;
}

/* convert file */
void
nh_sfconvert(const char *filename)
{
    (void) doconvert_file(filename, 0, FALSE);
}

/* unconvert file if it exists */
void
nh_sfunconvert(const char *filename)
{
    (void) doconvert_file(filename, 0, TRUE);
}

#else  /* !SFCTOOL */
/* in sfctool, these are in sfctool.c, not in here */
extern char *unconverted_filename, *converted_filename;
#endif /* !SFCTOOL */

staticfn boolean
make_converted_name(const char *filename)
{
    unsigned ln;
    const char *xtra, *finaldirchar;
    const char *dir = NULL;
    boolean needsep = FALSE;
#if defined(WIN32)
    static char folderbuf[MAX_PATH];
#endif

    if (!filename)
        return FALSE;

    if (unconverted_filename)
        free((genericptr_t) unconverted_filename), unconverted_filename = 0;
    if (converted_filename)
        free((genericptr_t) converted_filename), converted_filename = 0;

#ifndef SHORT_FILENAMES
    /* do we need to do some ms-dos processing here? */
#endif /* SHORT_FILENAMES */

    ln = (unsigned) strlen(filename);
    if (!contains_directory(filename)) {
#if defined(UNIX)
        dir = nh_getenv("NETHACKDIR");
        if (!dir)
            dir = nh_getenv("HACKDIR");
#ifdef HACKDIR
        if (!dir)
            dir = HACKDIR;
#endif
#elif defined(WIN32)
        if (get_user_home_folder(folderbuf, sizeof folderbuf)) {
            size_t sz = strlen(folderbuf);

            Snprintf(eos(folderbuf), sizeof folderbuf - sz,
                     "\\AppData\\Local\\NetHack\\5.0\\");
            dir = (const char *) folderbuf;
        }
#endif /* UNIX || WIN32 */
        if (dir) {
            finaldirchar = c_eos(dir);
                finaldirchar--;
            if (!(*finaldirchar == '/' || *finaldirchar == '\\'
                  || *finaldirchar == ':')) {
                needsep = TRUE;
                ln += 1;
            }
            ln += strlen(dir);
        }
    }
    unconverted_filename = (char *) alloc(ln + 1);
    Snprintf(unconverted_filename, ln + 1, "%s%s%s",
             dir ? dir : "",
             (dir && needsep) ? "/" : "",
             filename);
    xtra = ".exportascii";
    ln += (unsigned) strlen(xtra);
    converted_filename = (char *) alloc(ln + 1);
    Strcpy(converted_filename, unconverted_filename);
    Strcat(converted_filename, xtra);
    return TRUE;
}

/* delete converted savefile as a normal course of action */
int
delete_convertedfile(const char *basefilename)
{
    if (!converted_filename)
        make_converted_name(basefilename);
    if (converted_filename) {
        (void) unlink(converted_filename);
    }
    return 0;
}

void
free_convert_filenames(void)
{
    if (converted_filename)
        free((genericptr_t) converted_filename), converted_filename = 0;
    if (unconverted_filename)
        free((genericptr_t) unconverted_filename), unconverted_filename = 0;
/*    cvtinit = FALSE; */
}

/* return TRUE if s contains a directory, not just a filespec */
boolean
contains_directory(const char *s)
{
    int i, slen = strlen(s);
    const char *cp = s;

    for (i = 0; i < slen; ++i) {
        if (*cp == '\\' || *cp == '/' || *cp == ':')
            return TRUE;
        cp++;
    }
    return FALSE;
}

/* =========================================================================*/

/* ----------  END EXTERNAL CONVERSION HANDLING ----------- */

/* ----------  BEGIN FILE LOCKING HANDLING ----------- */

#ifndef SFCTOOL

#if defined(NO_FILE_LINKS) || defined(USE_FCNTL) /* implies UNIX */
static int lockfd = -1; /* for lock_file() to pass to unlock_file() */
#endif
#ifdef USE_FCNTL
static struct flock sflock; /* for unlocking, same as above */
#endif

#if defined(HANGUPHANDLING)
#define HUP if (!program_state.done_hup)
#else
#define HUP
#endif


#if defined(UNIX) || defined(VMS) || defined(AMIGA) || defined(WIN32) \
    || defined(MSDOS)
#define UNUSED_conditional /*empty*/
#else
#define UNUSED_conditional UNUSED
#endif


#ifndef USE_FCNTL
staticfn char *
make_lockname(const char *filename UNUSED_conditional, char *lockname)
{
#if defined(UNIX) || defined(VMS) || defined(AMIGA) || defined(WIN32) \
    || defined(MSDOS)
#ifdef NO_FILE_LINKS
    Strcpy(lockname, LOCKDIR);
    Strcat(lockname, "/");
    Strcat(lockname, filename);
#else
    Strcpy(lockname, filename);
#endif
#ifdef VMS
    {
        char *semi_colon = strrchr(lockname, ';');
        if (semi_colon)
            *semi_colon = '\0';
    }
    Strcat(lockname, ".lock;1");
#else
    Strcat(lockname, "_lock");
#endif
    return lockname;
#else /* !(UNIX || VMS || AMIGA || WIN32 || MSDOS) */
    lockname[0] = '\0';
    return (char *) 0;
#endif
}
#endif /* !USE_FCNTL */

/* lock a file */
boolean
lock_file(const char *filename, int whichprefix,
          int retryct UNUSED_conditional)
{
#ifndef USE_FCNTL
    char locknambuf[BUFSZ];
    const char *lockname;
#endif

    gn.nesting++;
    if (gn.nesting > 1) {
        impossible("TRIED TO NEST LOCKS");
        return TRUE;
    }

#ifndef USE_FCNTL
    lockname = make_lockname(filename, locknambuf);
#ifndef NO_FILE_LINKS /* LOCKDIR should be subsumed by LOCKPREFIX */
    lockname = fqname(lockname, LOCKPREFIX, 2);
#endif
#endif
    filename = fqname(filename, whichprefix, 0);
#ifdef USE_FCNTL
    lockfd = open(filename, O_RDWR);
    if (lockfd == -1) {
        HUP raw_printf("Cannot open file %s. "
                       " Is NetHack installed correctly?",
                       filename);
        gn.nesting--;
        return FALSE;
    }
    sflock.l_type = F_WRLCK;
    sflock.l_whence = SEEK_SET;
    sflock.l_start = 0;
    sflock.l_len = 0;
#endif

#if defined(UNIX) || defined(VMS)
#ifdef USE_FCNTL
    while (fcntl(lockfd, F_SETLK, &sflock) == -1) {
#else
#ifdef NO_FILE_LINKS
    while ((lockfd = open(lockname, O_RDWR | O_CREAT | O_EXCL, 0666)) == -1) {
#else
    while (link(filename, lockname) == -1) {
#endif
#endif

#ifdef USE_FCNTL
        if (retryct--) {
            HUP raw_printf("Waiting for release of fcntl lock on %s. "
                           " (%d retries left.)",
                           filename, retryct);
            sleep(1);
        } else {
            HUP raw_print("I give up.  Sorry.");
            HUP raw_printf("Some other process has an unnatural grip on %s.",
                           filename);
            gn.nesting--;
            return FALSE;
        }
#else
        int errnosv = errno;

        switch (errnosv) { /* George Barbanis */
        case EEXIST:
            if (retryct--) {
                HUP raw_printf("Waiting for access to %s. "
                               " (%d retries left).",
                               filename, retryct);
#if defined(SYSV) || defined(ULTRIX) || defined(VMS)
                (void)
#endif
                    sleep(1);
            } else {
                HUP raw_print("I give up.  Sorry.");
                HUP raw_printf("Perhaps there is an old %s around?",
                               lockname);
                gn.nesting--;
                return FALSE;
            }

            break;
        case ENOENT:
            HUP raw_printf("Can't find file %s to lock!", filename);
            gn.nesting--;
            return FALSE;
        case EACCES:
            HUP raw_printf("No write permission to lock %s!", filename);
            gn.nesting--;
            return FALSE;
#ifdef VMS /* c__translate(vmsfiles.c) */
        case EPERM:
            /* could be misleading, but usually right */
            HUP raw_printf("Can't lock %s due to directory protection.",
                           filename);
            gn.nesting--;
            return FALSE;
#endif
        case EROFS:
            /* take a wild guess at the underlying cause */
            HUP perror(lockname);
            HUP raw_printf("Cannot lock %s.", filename);
            HUP raw_printf("(Perhaps you are running NetHack from"
                           " inside the distribution package?).");
            gn.nesting--;
            return FALSE;
        default:
            HUP perror(lockname);
            HUP raw_printf("Cannot lock %s for unknown reason (%d).",
                           filename, errnosv);
            gn.nesting--;
            return FALSE;
        }
#endif /* USE_FCNTL */
    }
#endif /* UNIX || VMS */

#if (defined(AMIGA) || defined(WIN32) || defined(MSDOS)) \
    && !defined(USE_FCNTL)
#ifdef AMIGA
#define OPENFAILURE(fd) (!fd)
    gl.lockptr = 0;
#else
#define OPENFAILURE(fd) (fd < 0)
    gl.lockptr = -1;
#endif
    while (--retryct && OPENFAILURE(gl.lockptr)) {
#if defined(WIN32) && !defined(WIN_CE)
        gl.lockptr = sopen(lockname, O_RDWR | O_CREAT, SH_DENYRW, S_IWRITE);
#else
        (void) DeleteFile(lockname); /* in case dead process was here first */
#ifdef AMIGA
        gl.lockptr = Open(lockname, MODE_NEWFILE);
#else
        gl.lockptr = open(lockname, O_RDWR | O_CREAT | O_EXCL, S_IWRITE);
#endif
#endif
        if (OPENFAILURE(gl.lockptr)) {
            raw_printf("Waiting for access to %s.  (%d retries left).",
                       filename, retryct);
            Delay(50);
        }
    }
    if (!retryct) {
        raw_printf("I give up.  Sorry.");
        gn.nesting--;
        return FALSE;
    }
#endif /* AMIGA || WIN32 || MSDOS */
    return TRUE;
}

#ifdef VMS /* for unlock_file, use the unlink() routine in vmsunix.c */
#ifdef unlink
#undef unlink
#endif
#define unlink(foo) vms_unlink(foo)
#endif

/* unlock file, which must be currently locked by lock_file */
void
unlock_file(const char *filename)
{
#ifndef USE_FCNTL
    char locknambuf[BUFSZ];
    const char *lockname;
#endif

    if (gn.nesting == 1) {
#ifdef USE_FCNTL
        sflock.l_type = F_UNLCK;
        if (lockfd >= 0) {
            if (fcntl(lockfd, F_SETLK, &sflock) == -1)
                HUP raw_printf("Can't remove fcntl lock on %s.", filename);
            (void) close(lockfd), lockfd = -1;
        }
#else
        lockname = make_lockname(filename, locknambuf);
#ifndef NO_FILE_LINKS /* LOCKDIR should be subsumed by LOCKPREFIX */
        lockname = fqname(lockname, LOCKPREFIX, 2);
#endif

#if defined(UNIX) || defined(VMS)
        if (unlink(lockname) < 0)
            HUP raw_printf("Can't unlink %s.", lockname);
#ifdef NO_FILE_LINKS
        (void) nhclose(lockfd), lockfd = -1;
#endif

#endif /* UNIX || VMS */

#if defined(AMIGA) || defined(WIN32) || defined(MSDOS)
        if (gl.lockptr)
            Close(gl.lockptr);
        DeleteFile(lockname);
        gl.lockptr = 0;
#endif /* AMIGA || WIN32 || MSDOS */
#endif /* USE_FCNTL */
    }

    gn.nesting--;
}

#undef UNUSED_conditional

/* ----------  END FILE LOCKING HANDLING ----------- */

/* ----------  BEGIN DATA FILE IDENTIFICATION ----------- */

#if defined(DLB) && defined(DLBLIB)
/* data library members that don't affect the game's levels: the help
   texts, and the list of compile-time options (which depends on the
   build, not the data) */
static const char *const datahash_skip[] = {
    "help", "hh", "cmdhelp", "keyhelp", "history", "opthelp", "optmenu",
    "usagehlp", "wizhelp", "options"
};

staticfn int QSORTCALLBACK datahash_cmp(const genericptr, const genericptr);

staticfn int QSORTCALLBACK
datahash_cmp(const genericptr vptr1, const genericptr vptr2)
{
    return strcmp(*(const char *const *) vptr1, *(const char *const *) vptr2);
}
#endif

/* a hash of the game's data files that affect its levels (the level
   scripts, rumors, engravings and so on in nhdat), recorded by seeded
   games: two games with the same seed are only comparable if their data
   files match; it's taken over the files' names and contents in name
   order, so it doesn't depend on how the library was put together */
const char *
data_files_hash(void)
{
    if (!gseed.datahash[0]) {
        Strcpy(gseed.datahash, "unknown");
#if defined(DLB) && defined(DLBLIB)
        {
            uint64 h = 0xcbf29ce484222325ULL;
            const char **names;
            const char *p;
            char buf[BUFSZ];
            int n, count, i, len;
            boolean ok = TRUE;
            dlb *fp;

            for (count = 0; dlb_member_name(count); count++)
                continue;
            if (!count)
                return gseed.datahash;
            names = (const char **) alloc(count * sizeof *names);
            for (n = 0; n < count; n++)
                names[n] = dlb_member_name(n);
            qsort((genericptr_t) names, count, sizeof *names, datahash_cmp);
            for (n = 0; n < count && ok; n++) {
                for (i = 0; i < SIZE(datahash_skip); i++)
                    if (!strcmp(names[n], datahash_skip[i]))
                        break;
                if (i < SIZE(datahash_skip))
                    continue;
                if (!(fp = dlb_fopen(names[n], RDBMODE))) {
                    ok = FALSE;
                    break;
                }
                for (p = names[n]; ; p++) { /* the name, with its '\0' */
                    h ^= (uint64) (unsigned char) *p;
                    h *= 0x100000001b3ULL;
                    if (!*p)
                        break;
                }
                while ((len = dlb_fread(buf, 1, (int) sizeof buf, fp)) > 0)
                    for (i = 0; i < len; i++) {
                        h ^= (uint64) (unsigned char) buf[i];
                        h *= 0x100000001b3ULL;
                    }
                (void) dlb_fclose(fp);
            }
            free((genericptr_t) names);
            if (ok)
                Sprintf(gseed.datahash, "%08lx%08lx",
                        (unsigned long) ((h >> 32) & 0xffffffffUL),
                        (unsigned long) (h & 0xffffffffUL));
        }
#endif
    }
    return gseed.datahash;
}

/* the build this game is running, for seeded games' records: the source
   revision it was built from (marked "-dirty" if it had local changes),
   or failing that the version and the time it was built */
const char *
build_id(void)
{
    static char buf[BUFSZ];

    if (nomakedefs.git_sha && *nomakedefs.git_sha)
        return nomakedefs.git_sha;
    if (!buf[0])
        Snprintf(buf, sizeof buf, "%s-%lu", nomakedefs.version_string,
                 nomakedefs.build_time);
    return buf;
}

/* ----------  END DATA FILE IDENTIFICATION ----------- */

/* ----------  BEGIN GAME RECORDING ----------- */

#ifndef SFCTOOL
/*
 * Recording and replaying seeded games, so that a game played on a server
 * can be checked afterwards.  When sysconf's RECORDFILE is set (or, for a
 * game running without permissions the player lacks, NH_RECORD), a seeded
 * game played with the tty interface writes to that file everything it
 * takes from outside: its command-line arguments, the login name and the
 * options it read, every random seed it draws from the system
 * (init_random(), reseed_random()), every key it reads (tty_nhgetch()),
 * and where it acts on a hangup or an interrupt; and besides those, a
 * digest of its state now and then.  Each session (from starting or
 * restoring the game to saving it or its end) begins with a header saying
 * how it was run.
 *
 * "nethack --replay FILE" plays such a file again: the game takes all of
 * that from the record instead, in a scratch copy of the playground (see
 * nhrec_enter_scratch()) so that nothing of the player's is touched,
 * checks each digest, and shows the game on the terminal at a watchable
 * pace (space pauses, '.' steps one key, '+' and '-' change the speed, '>'
 * runs flat out, 'q' stops); with --verify it runs flat out and only
 * reports.  A record of several sessions (the game was saved and
 * restored) is replayed session by session, each in a fresh process
 * (nhrec_next_session()), as the game was played.  --seed SEED supplies
 * a server's hidden seed.  Unix only.
 *
 * The file is a series of entries, each "TAG LENGTH:PAYLOAD" and a
 * newline, where LENGTH is the number of bytes of PAYLOAD, which can hold
 * anything (an options file's text, with its newlines).  A session begins
 * with its header:
 *   session  new|restore
 *   name     the hero's name
 *   mode     normal|explore|wizard
 *   term     ROWS COLS: the terminal's size
 *   seed     the seed as shown to players ("hidden#..." for the server's)
 *   reseed   yes|no: whether the game reseeds its random numbers when it
 *            changes level (it does if the system has a strong source)
 *   login    the login name the game read for the hero's name, if it did
 *   arg      one for each command-line argument the game handled itself
 *   rcfile   the text of the options file the game read, as read
 *   envopts  NETHACKOPTIONS, when it held options rather than a file name
 * and goes on with its events:
 *   s        HEX: a seed drawn from the system
 *   k        N: a key read from the terminal
 *   u        1|0: whether sysconf's WIZARDS, EXPLORERS or SHELLERS let the
 *            player do what they asked (the replay runs as someone else)
 *   hup      a hangup (the terminal went away, or the signal): with no
 *            payload where the game was to read a key, otherwise the pass
 *            of moveloop_core() (counted from the session's start) at
 *            whose start the game acted on it
 *   intr     an interrupt (^C), placed the same way
 *   c        turn|level TURN DIGEST: a checkpoint, every 100 turns and on
 *            arriving on a level: a digest of the game's state
 *   end      save|done DIGEST: the session ended; the state's digest
 */

enum nhrec_mode { NHREC_UNDECIDED = 0, NHREC_OFF, NHREC_RECORDING,
                  NHREC_REPLAYING };
enum nhrec_outcome { NHREC_VERIFIED, NHREC_FAILED, NHREC_CUT_OFF,
                     NHREC_STOPPED };

#define NHREC_TAGSZ 16            /* longest tag, and its '\0' */
#define NHREC_MAXPAYLOAD 16000000UL /* the most bytes in one entry */

/* a growing buffer of entries */
struct nhrec_buf {
    char *buf;
    size_t len, siz;
};

/* an event of the session being replayed */
struct nhrec_event {
    char kind;          /* 's' seed, 'k' key, 'u' permission, 'h' hangup,
                         * 'i' interrupt, 'c' checkpoint, 'e' end */
    unsigned long val;  /* s: the seed; k: the key; u: allowed or not */
    long pass;          /* h, i: the pass, or -1 where a key was read */
    char *text;         /* c, e: the payload, to compare with */
};

static struct nhrec_state {
    enum nhrec_mode mode;
    FILE *fp;                /* the record being written */
    struct nhrec_buf hdr;    /* header entries noted before the game
                              * starts (login, arguments, options) */
    struct nhrec_buf pending; /* events before it's known whether the game
                               * is recorded */
    struct nhrec_buf rctext; /* the options file's text, while read */
    char *envopts;           /* NETHACKOPTIONS, while the options are read */
    boolean opts_taken;      /* the options have been noted */
    boolean capturing;       /* the options file is being read */
    boolean rc_read;         /* ... and some of it was */
    long passes;             /* passes of moveloop_core() */
    boolean in_intr;         /* acting on an interrupt */
    char saveend[40];        /* the state's digest as a save began */
    /* replaying: the session's header and events */
    int session;
    char *h_kind, *h_name, *h_mode, *h_term, *h_seed, *h_login;
    int h_reseed;            /* -1: not given */
    boolean h_has_login;
    char **args;             /* its command-line arguments */
    int nargs;
    char **argv;             /* ... as argv for process_options() */
    struct nhrec_event *events;
    int nevents, next;
    boolean ends;            /* the session has an end event */
    long checks;             /* checkpoints passed */
    int nsessions;           /* sessions in the record */
    char *h_rcfile;          /* the recorded options file's text... */
    size_t h_rclen;          /* ...and its length (it may hold '\0') */
    char *h_envopts;         /* recorded NETHACKOPTIONS, or Null */
    char *next_kind;         /* the session after this one: new|restore */
    /* replaying: how it was asked for (--replay and friends) */
    boolean verify;          /* --verify: flat out, report only */
    char *record;            /* the record's path (absolute) */
    char *exe;               /* this program, to run the next session */
    char *scratch;           /* the scratch playground */
    char *seedopt;           /* --seed: a server's hidden seed */
    long delay_ms;           /* pause between replayed keys, watching */
    boolean paused;
    char failed[80];         /* why writing the record failed, if it did */
} nhrec;

staticfn void nhrec_init(void);
staticfn void nhrec_buf_put(struct nhrec_buf *, const char *, const char *,
                            size_t);
staticfn void nhrec_buf_free(struct nhrec_buf *);
staticfn void nhrec_event(const char *, const char *);
staticfn void nhrec_write_failed(void);
staticfn void nhrec_refuse(const char *, boolean);
staticfn int nhrec_read_entry(FILE *, char *, char **, size_t *);
staticfn boolean nhrec_number(const char *, int, unsigned long,
                              unsigned long *);
staticfn boolean nhrec_digest_text(const char *, const char *const *);
staticfn boolean nhrec_header_ok(int, const char *, size_t);
staticfn const char *nhrec_load(FILE *, int);
staticfn const char *nhrec_quote(const char *);
staticfn const struct nhrec_event *nhrec_next(const char *, const char *);
staticfn void nhrec_result(const char *, enum nhrec_outcome, boolean);
staticfn void nhrec_finish(const char *, enum nhrec_outcome);
staticfn void nhrec_next_session(enum nhrec_outcome);
staticfn boolean nhrec_hidden_record(void);
#ifdef UNIX
staticfn char *nhrec_self_path(const char *);
staticfn void nhrec_pace(void);
staticfn void nhrec_rmtree(const char *);
#endif
staticfn const char *nhrec_path(void);
staticfn void nhrec_stop(void);
staticfn FILE *nhrec_open(const char *, boolean);
staticfn FILE *nhrec_scratch_file(const char *, const char *);
staticfn void nhrec_interrupt(void);
staticfn uint64 nhrec_digest(void);

/* append an entry to a buffer */
staticfn void
nhrec_buf_put(
    struct nhrec_buf *b,
    const char *tag,
    const char *data,
    size_t n)
{
    char head[NHREC_TAGSZ + 24];
    size_t hn;

    Snprintf(head, sizeof head, "%s %lu:", tag, (unsigned long) n);
    hn = strlen(head);
    if (b->len + hn + n + 1 > b->siz) {
        size_t newsiz = max(b->siz * 2, b->len + hn + n + 256);
        char *newbuf = (char *) alloc((unsigned) newsiz);

        if (b->buf) {
            (void) memcpy((genericptr_t) newbuf, (genericptr_t) b->buf,
                          b->len);
            free((genericptr_t) b->buf);
        }
        b->buf = newbuf, b->siz = newsiz;
    }
    (void) memcpy((genericptr_t) (b->buf + b->len), (genericptr_t) head, hn);
    b->len += hn;
    if (n)
        (void) memcpy((genericptr_t) (b->buf + b->len), (genericptr_t) data,
                      n);
    b->len += n;
    b->buf[b->len++] = '\n';
}

staticfn void
nhrec_buf_free(struct nhrec_buf *b)
{
    if (b->buf)
        free((genericptr_t) b->buf);
    b->buf = (char *) 0;
    b->len = b->siz = 0;
}

/* record an event (or keep it until it's known whether to record) */
staticfn void
nhrec_event(const char *tag, const char *payload)
{
    size_t n = strlen(payload);

    if (nhrec.fp) {
        if (fprintf(nhrec.fp, "%s %lu:", tag, (unsigned long) n) < 0
            || fwrite(payload, 1, n, nhrec.fp) != n
            || fputc('\n', nhrec.fp) == EOF || fflush(nhrec.fp) != 0)
            nhrec_write_failed();
    } else if (!nhrec.failed[0]) {
        nhrec_buf_put(&nhrec.pending, tag, payload, n);
    }
}

/* a write to the record failed: the game can't go on unrecorded, so it
   ends as on a hangup at the next point one is acted on; nhrec_free()
   says why */
staticfn void
nhrec_write_failed(void)
{
    if (!nhrec.failed[0])
        Snprintf(nhrec.failed, sizeof nhrec.failed, "%s", strerror(errno));
    if (nhrec.fp)
        (void) fclose(nhrec.fp), nhrec.fp = (FILE *) 0;
#ifdef HANGUPHANDLING
    program_state.pending_hup = 1;
#else
    nhrec_refuse(nhrec.failed, FALSE);
#endif
}

/* read an entry of a record: its tag, and its payload (allocated, with a
   '\0' after it, though it may also hold others); 1 if one was read, 0 at
   the end of the file, -1 if what's there isn't an entry */
staticfn int
nhrec_read_entry(FILE *fp, char *tag, char **payload, size_t *len)
{
    int c, n = 0, digits = 0;
    unsigned long size = 0;

    if ((c = getc(fp)) == EOF)
        return 0;
    while (c >= 'a' && c <= 'z') {
        if (n >= NHREC_TAGSZ - 1)
            return -1;
        tag[n++] = (char) c;
        c = getc(fp);
    }
    tag[n] = '\0';
    if (!n || c != ' ')
        return -1;
    while ((c = getc(fp)) >= '0' && c <= '9') {
        if (++digits > 9)
            return -1;
        size = size * 10 + (unsigned long) (c - '0');
    }
    if (!digits || c != ':' || size > NHREC_MAXPAYLOAD)
        return -1;
    *payload = (char *) alloc((unsigned) size + 1);
    if ((size && fread(*payload, 1, (size_t) size, fp) != (size_t) size)
        || getc(fp) != '\n') {
        free((genericptr_t) *payload), *payload = (char *) 0;
        return -1;
    }
    (*payload)[size] = '\0';
    *len = (size_t) size;
    return 1;
}

/* a number written in base 10 or 16, with no sign or spaces, and no more
   than max */
staticfn boolean
nhrec_number(const char *s, int base, unsigned long max, unsigned long *out)
{
    unsigned long v = 0, d;

    if (!*s)
        return FALSE;
    for (; *s; s++) {
        if (*s >= '0' && *s <= '9')
            d = (unsigned long) (*s - '0');
        else if (base == 16 && *s >= 'a' && *s <= 'f')
            d = (unsigned long) (*s - 'a' + 10);
        else
            return FALSE;
        if (d > max || v > (max - d) / (unsigned long) base)
            return FALSE;
        v = v * (unsigned long) base + d;
    }
    *out = v;
    return TRUE;
}

/* a checkpoint's or an end's payload: one of the words, and for a
   checkpoint a turn, then a digest of 16 hex digits */
staticfn boolean
nhrec_digest_text(const char *s, const char *const *words)
{
    const char *p;
    char num[12];
    unsigned long v;
    int i, n = 0;

    for (i = 0; words[i]; i++) {
        n = (int) strlen(words[i]);
        if (!strncmp(s, words[i], n) && s[n] == ' ')
            break;
    }
    if (!words[i])
        return FALSE;
    s += n + 1;
    if (!strcmp(words[0], "turn")) {
        if (!(p = strchr(s, ' ')) || p == s || p - s >= (int) sizeof num)
            return FALSE;
        (void) strncpy(num, s, p - s);
        num[p - s] = '\0';
        if (!nhrec_number(num, 10, 0x7fffffffUL, &v))
            return FALSE;
        s = p + 1;
    }
    for (n = 0; s[n]; n++)
        if (!((s[n] >= '0' && s[n] <= '9') || (s[n] >= 'a' && s[n] <= 'f')))
            return FALSE;
    return n == 16;
}

/* the header entries a session can have (each once, but arg), and those
   it must have */
static const char *const nhrec_hdrtags[] = {
    "name", "mode", "term", "seed", "reseed", "login", "rcfile", "envopts",
    "arg"
};
#define NHREC_HDR_NEEDED 0x1f /* name, mode, term, seed and reseed */

/* check a header entry's payload; its index in nhrec_hdrtags[] */
staticfn boolean
nhrec_header_ok(int idx, const char *payload, size_t len)
{
    unsigned long v;
    const char *p;

    if (idx != 6 && memchr(payload, '\0', len))
        return FALSE; /* only an options file's text can hold a '\0' */
    switch (idx) {
    case 1:
        return (!strcmp(payload, "normal") || !strcmp(payload, "explore")
                || !strcmp(payload, "wizard"));
    case 2:
        if (!(p = strchr(payload, ' ')) || p == payload || p - payload > 4
            || !nhrec_number(p + 1, 10, 9999UL, &v))
            return FALSE;
        for (; payload < p; payload++)
            if (*payload < '0' || *payload > '9')
                return FALSE;
        return TRUE;
    case 4:
        return (!strcmp(payload, "yes") || !strcmp(payload, "no"));
    default:
        return TRUE;
    }
}

/* replaying: read the whole record, checking every entry, and keep the
   header and events of the session to replay; Null if all is well, or else
   what's wrong with it */
staticfn const char *
nhrec_load(FILE *fp, int want)
{
    static const char *const words_c[] = { "turn", "level", 0 },
                          *const words_e[] = { "save", "done", 0 };
    static char why[BUFSZ];
    char tag[NHREC_TAGSZ], **field;
    char *payload = (char *) 0;
    size_t len;
    unsigned long v;
    long entry = 0;
    int r, i, session = 0, seen = 0;
    boolean in_events = FALSE, mine, bad = FALSE;
    struct nhrec_event ev;

    while (!bad && (r = nhrec_read_entry(fp, tag, &payload, &len)) > 0) {
        ++entry;
        mine = (session == want);
        (void) memset((genericptr_t) &ev, 0, sizeof ev);
        for (i = 0; i < SIZE(nhrec_hdrtags); i++)
            if (!strcmp(tag, nhrec_hdrtags[i]))
                break;
        if (strlen(payload) != len && strcmp(tag, "rcfile")) {
            bad = TRUE; /* only an options file's text may hold '\0' */
        } else if (!strcmp(tag, "session")) {
            if ((session && (seen & NHREC_HDR_NEEDED) != NHREC_HDR_NEEDED)
                || (strcmp(payload, "new") && strcmp(payload, "restore"))) {
                bad = TRUE;
            } else {
                ++session;
                if (session == want)
                    nhrec.h_kind = payload, payload = (char *) 0;
                else if (session == want + 1)
                    nhrec.next_kind = payload, payload = (char *) 0;
            }
            in_events = FALSE;
            seen = 0;
        } else if (!session) {
            bad = TRUE;
        } else if (i < SIZE(nhrec_hdrtags)) {
            /* the header */
            if (in_events || (i < 8 && (seen & (1 << i)))
                || !nhrec_header_ok(i, payload, len)) {
                bad = TRUE;
            } else {
                seen |= (1 << i);
                field = (i == 0) ? &nhrec.h_name : (i == 1) ? &nhrec.h_mode
                        : (i == 2) ? &nhrec.h_term : (i == 3) ? &nhrec.h_seed
                        : (i == 5) ? &nhrec.h_login
                        : (i == 6) ? &nhrec.h_rcfile
                        : (i == 7) ? &nhrec.h_envopts : (char **) 0;
                if (mine && field) {
                    *field = payload, payload = (char *) 0;
                    if (i == 5)
                        nhrec.h_has_login = TRUE;
                    if (i == 6)
                        nhrec.h_rclen = len;
                } else if (mine && i == 4) {
                    nhrec.h_reseed = !strcmp(payload, "yes");
                } else if (mine && i == 8) {
                    if (nhrec.nargs % 16 == 0) {
                        char **more = (char **) alloc((unsigned)
                                         ((nhrec.nargs + 16) * sizeof (char *)));

                        if (nhrec.args) {
                            (void) memcpy((genericptr_t) more,
                                          (genericptr_t) nhrec.args,
                                          nhrec.nargs * sizeof (char *));
                            free((genericptr_t) nhrec.args);
                        }
                        nhrec.args = more;
                    }
                    nhrec.args[nhrec.nargs++] = payload,
                        payload = (char *) 0;
                }
            }
        } else {
            in_events = TRUE;
            ev.pass = -1L;
            if (!strcmp(tag, "s")) {
                ev.kind = 's';
                bad = (len > 16 || !nhrec_number(payload, 16, ~0UL, &ev.val));
            } else if (!strcmp(tag, "k")) {
                ev.kind = 'k';
                bad = !nhrec_number(payload, 10, 255UL, &ev.val);
            } else if (!strcmp(tag, "u")) {
                ev.kind = 'u';
                bad = !nhrec_number(payload, 10, 1UL, &ev.val);
            } else if (!strcmp(tag, "hup") || !strcmp(tag, "intr")) {
                ev.kind = (tag[0] == 'h') ? 'h' : 'i';
                if (*payload) {
                    bad = (!nhrec_number(payload, 10, 0x7fffffffUL, &v)
                           || !v);
                    ev.pass = (long) v;
                }
            } else if (!strcmp(tag, "c") || !strcmp(tag, "end")) {
                ev.kind = (tag[0] == 'c') ? 'c' : 'e';
                bad = !nhrec_digest_text(payload, (ev.kind == 'c') ? words_c
                                                                  : words_e);
                if (!bad && mine) {
                    ev.text = payload, payload = (char *) 0;
                    if (ev.kind == 'e')
                        nhrec.ends = TRUE;
                }
            } else {
                bad = TRUE; /* not an entry a record has */
            }
            if (!bad && mine) {
                if (nhrec.nevents % 1024 == 0) {
                    struct nhrec_event *more = (struct nhrec_event *) alloc(
                        (unsigned) ((nhrec.nevents + 1024) * sizeof ev));

                    if (nhrec.events) {
                        (void) memcpy((genericptr_t) more,
                                      (genericptr_t) nhrec.events,
                                      nhrec.nevents * sizeof ev);
                        free((genericptr_t) nhrec.events);
                    }
                    nhrec.events = more;
                }
                nhrec.events[nhrec.nevents++] = ev;
            } else if (ev.text) {
                free((genericptr_t) ev.text);
            }
        }
        if (payload)
            free((genericptr_t) payload), payload = (char *) 0;
    }
    if (!bad && r < 0)
        bad = TRUE;
    if (!bad && session
        && (seen & NHREC_HDR_NEEDED) != NHREC_HDR_NEEDED)
        bad = TRUE, ++entry; /* the last session's header is incomplete */
    if (bad) {
        Sprintf(why, "the record is malformed (at entry %ld)",
                r < 0 ? entry + 1 : entry);
        return why;
    }
    if (session < want)
        return "the record has no such session";
    nhrec.nsessions = session;
    return (const char *) 0;
}

/* text from the record for a message: printable, and not too long */
staticfn const char *
nhrec_quote(const char *s)
{
    static char buf[64];
    int n = 0;

    for (; *s && n < (int) sizeof buf - 4; s++)
        buf[n++] = (*s >= ' ' && *s < '\177') ? *s : '?';
    if (*s)
        buf[n++] = '.', buf[n++] = '.', buf[n++] = '.';
    buf[n] = '\0';
    return buf;
}

/* first use (when not replaying, see nhrec_replay_args()): gather events
   until it's known whether this game is recorded */
staticfn void
nhrec_init(void)
{
    if (nhrec.mode != NHREC_UNDECIDED)
        return;
    nhrec.mode = NHREC_RECORDING; /* gathering, until nhrec_game_start() */
}

/* TRUE if the record being replayed is of a game with a server's hidden
   seed (shown as "hidden#..."), which the replay has to be given */
staticfn boolean
nhrec_hidden_record(void)
{
    return nhrec.h_seed && !strncmp(nhrec.h_seed, "hidden#", 7);
}

#ifdef UNIX
/* this program's path, to run the next session of a replay with */
staticfn char *
nhrec_self_path(const char *argv0)
{
    char buf[BUFSZ], cand[BUFSZ];
    const char *p, *e;
    size_t len;
#ifdef __linux__
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);

    if (n > 0) {
        buf[n] = '\0';
        return dupstr(buf);
    }
#endif
    if (strchr(argv0, '/'))
        return dupstr(realpath(argv0, buf) ? buf : argv0);
    for (p = getenv("PATH"); p && *p; p = e ? e + 1 : (const char *) 0) {
        e = strchr(p, ':');
        len = e ? (size_t) (e - p) : strlen(p);
        if (len + strlen(argv0) + 2 >= sizeof cand)
            continue;
        Snprintf(cand, sizeof cand, "%.*s/%s", (int) len, len ? p : ".",
                 argv0);
        if (access(cand, X_OK) == 0 && realpath(cand, buf))
            return dupstr(buf);
    }
    return dupstr(argv0);
}
#endif /* UNIX */

/* the command line, before anything else: "--replay RECORD" makes this a
   replay of that record, with "--verify" (flat out, report only), "--seed
   SEED" (a server's hidden seed), and, for the sessions after the first,
   which nhrec_next_session() starts, "--session N" and "--scratch DIR";
   those arguments are removed, the rest is left for the ports */
void
nhrec_replay_args(int *argcp, char ***argvp)
{
#ifdef UNIX
    int i, j, used, argc = *argcp;
    char **argv = *argvp, buf[BUFSZ];
    const char *arg, *val, *record = (const char *) 0, *why;
    unsigned long n;
    boolean bad = FALSE;
    FILE *fp;

    nhrec.session = 1;
    nhrec.delay_ms = 100L;
    nhrec.h_reseed = -1;
    for (i = 1; i < argc;) {
        arg = argv[i];
        val = (i + 1 < argc) ? argv[i + 1] : (const char *) 0;
        used = 0;
        if (!strcmp(arg, "--replay")) {
            used = 2;
            if (!val)
                bad = TRUE;
            else
                record = val;
        } else if (!strcmp(arg, "--verify")) {
            used = 1;
            nhrec.verify = TRUE;
        } else if (!strcmp(arg, "--seed")) {
            used = 2;
            if (!val)
                bad = TRUE;
            else
                nhrec.seedopt = dupstr(val);
        } else if (!strcmp(arg, "--session")) {
            used = 2;
            if (!val || !nhrec_number(val, 10, 1000000UL, &n) || !n)
                bad = TRUE;
            else
                nhrec.session = (int) n;
        } else if (!strcmp(arg, "--scratch")) {
            used = 2;
            if (!val)
                bad = TRUE;
            else
                nhrec.scratch = dupstr(val);
        }
        if (!used) {
            ++i;
            continue;
        }
        if (bad)
            break;
        for (j = i; j + used <= argc; j++) /* (argv[argc] is Null) */
            argv[j] = argv[j + used];
        argc -= used;
    }
    *argcp = argc;
    if (bad || (!record && (nhrec.verify || nhrec.seedopt || nhrec.scratch
                            || nhrec.session != 1))) {
        (void) fprintf(stderr,
                       "usage: nethack --replay RECORD [--verify]"
                       " [--seed SEED]\n");
        exit(EXIT_FAILURE);
    }
    if (!record)
        return;
    /* replaying reads and writes files the player names, so not with
       permissions the player lacks */
    if (getuid() != geteuid() || getgid() != getegid()) {
        (void) fprintf(stderr, "nethack: --replay needs the player's own"
                               " permissions.\n");
        exit(EXIT_FAILURE);
    }
    nhrec.mode = NHREC_REPLAYING;
    program_state.recorded_input = 1;
    if (!realpath(record, buf) || !(fp = fopen(buf, "r"))) {
        (void) fprintf(stderr, "nethack: can't read the record %s: %s\n",
                       record, strerror(errno));
        exit(EXIT_FAILURE);
    }
    nhrec.record = dupstr(buf);
    nhrec.exe = nhrec_self_path(argv[0]);
    why = nhrec_load(fp, nhrec.session);
    (void) fclose(fp);
    if (why) {
        (void) fprintf(stderr, "nethack: %s.\n", why);
        exit(EXIT_FAILURE);
    }
#else
    nhUse(argcp);
    nhUse(argvp);
#endif /* UNIX */
}

/* a replay runs in a scratch playground, so that its saves, level files
   and logs can't touch the player's: the playground's read-only files by
   reference (symbolic links), and fresh empty ones for the rest; the
   sessions after the first reuse it (they restore the save the one before
   made).  Called once the game has changed to the playground. */
void
nhrec_enter_scratch(void)
{
#ifdef UNIX
    static const char *const varfiles[] = {
        "record", "logfile", "xlogfile", "livelog", "paniclog", "perm"
    };
    char here[BUFSZ], path[BUFSZ], link[BUFSZ];
    const char *tmp, *nm;
    DIR *dp;
    struct dirent *de;
    struct stat st;
    int i, fd;

    if (nhrec.mode != NHREC_REPLAYING)
        return;
    program_state.recorded_input = 1; /* (program_state was reset since) */
    if (!nhrec.scratch) {
        if (!getcwd(here, sizeof here) || !(dp = opendir("."))) {
            (void) fprintf(stderr, "nethack: can't read the playground.\n");
            exit(EXIT_FAILURE);
        }
        tmp = getenv("TMPDIR");
        if (!tmp || !*tmp)
            tmp = "/tmp";
        Snprintf(path, sizeof path, "%s/nethack-replay-XXXXXX", tmp);
        if (!mkdtemp(path)) {
            (void) fprintf(stderr, "nethack: can't make a scratch"
                                   " playground in %s: %s\n",
                           tmp, strerror(errno));
            exit(EXIT_FAILURE);
        }
        nhrec.scratch = dupstr(path);
        (void) chmod(path, 0700); /* (the game's umask took the x bits) */
        while ((de = readdir(dp)) != 0) {
            nm = de->d_name;
            /* not what games write: the files below, lock files, and
               level files (their names start with a digit) */
            if (nm[0] == '.' || digit(nm[0]) || strstr(nm, "lock"))
                continue;
            /* nor the files a replay writes here (nhrec_scratch_file()) */
            if (!strncmp(nm, "replay.", 7) || !strcmp(nm, "seedfuzz.txt"))
                continue;
            for (i = 0; i < SIZE(varfiles); i++)
                if (!strcmp(nm, varfiles[i]))
                    break;
            if (i < SIZE(varfiles) || stat(nm, &st) || !S_ISREG(st.st_mode))
                continue;
            Snprintf(path, sizeof path, "%s/%s", here, nm);
            Snprintf(link, sizeof link, "%s/%s", nhrec.scratch, nm);
            (void) symlink(path, link);
        }
        (void) closedir(dp);
        for (i = 0; i < SIZE(varfiles); i++) {
            Snprintf(path, sizeof path, "%s/%s", nhrec.scratch, varfiles[i]);
            if ((fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600)) >= 0)
                (void) close(fd);
        }
        Snprintf(path, sizeof path, "%s/save", nhrec.scratch);
        (void) mkdir(path, 0700);
        (void) chmod(path, 0700);
    }
    if (chdir(nhrec.scratch) < 0) {
        (void) fprintf(stderr, "nethack: can't use the scratch playground"
                               " %s: %s\n", nhrec.scratch, strerror(errno));
        exit(EXIT_FAILURE);
    }
#endif /* UNIX */
}

/* a file the replay writes in its scratch playground ("wb" or "a"), never
   through a symbolic link */
staticfn FILE *
nhrec_scratch_file(const char *name, const char *mode)
{
    FILE *fp;
    int fd, oflags = O_WRONLY | O_CREAT
                     | ((*mode == 'a') ? O_APPEND : O_TRUNC);

#ifdef O_NOFOLLOW
    oflags |= O_NOFOLLOW;
#endif
    if ((fd = open(name, oflags, 0600)) < 0)
        return (FILE *) 0;
    if (!(fp = fdopen(fd, mode)))
        (void) close(fd);
    return fp;
}

/* replaying: the recorded options file, written to the scratch playground
   for the game to read as it read the original; Null if the recorded game
   read none */
const char *
nhrec_options_file(void)
{
    static const char name[] = "replay.nethackrc";
    FILE *fp;

    if (nhrec.mode != NHREC_REPLAYING || !nhrec.h_rcfile)
        return (const char *) 0;
    if ((fp = nhrec_scratch_file(name, "wb")) != 0) {
        (void) fwrite(nhrec.h_rcfile, 1, nhrec.h_rclen, fp);
        (void) fclose(fp);
    }
    return name;
}

/* replaying: the recorded NETHACKOPTIONS, or Null */
const char *
nhrec_envopts(void)
{
    return (nhrec.mode == NHREC_REPLAYING) ? nhrec.h_envopts
                                           : (const char *) 0;
}

/* replaying: whether a sysconf setting is ignored: what makes the game
   write outside its playground or act on this system's setup, and, for
   a record of a player's own seed (or when --seed gives the server's),
   the server's SEED */
boolean
nhrec_sysconf_ignored(const char *setting)
{
    if (nhrec.mode != NHREC_REPLAYING)
        return FALSE;
    if (!strcmp(setting, "SEED"))
        return !nhrec_hidden_record() || nhrec.seedopt != 0;
    return TRUE;
}

/* replaying: the recorded terminal's size, which the tty port uses instead
   of the real one's (where messages break decides which keys are read) */
boolean
nhrec_term_size(int *rows, int *cols)
{
    int r, c;

    if (nhrec.mode != NHREC_REPLAYING || !nhrec.h_term
        || sscanf(nhrec.h_term, "%d %d", &r, &c) != 2)
        return FALSE;
    *rows = r, *cols = c;
    return TRUE;
}

/* TRUE while replaying flat out (--verify): no delays for anyone to see */
boolean
nhrec_verifying(void)
{
    return nhrec.mode == NHREC_REPLAYING && nhrec.verify;
}

#ifdef UNIX
/* watching a replay: wait between keys, and act on what the watcher types
   meanwhile (nothing is drawn for it: that would take recorded keys) */
staticfn void
nhrec_pace(void)
{
    struct pollfd pfd;
    unsigned char c;
    ssize_t n;
    int wait;
    char buf[BUFSZ];

    for (;;) {
        wait = nhrec.paused ? -1 : (int) nhrec.delay_ms;
        pfd.fd = fileno(stdin);
        pfd.events = POLLIN;
        pfd.revents = 0;
        if (poll(&pfd, 1, wait) <= 0) {
            if (nhrec.paused)
                continue;
            return; /* the pause is over */
        }
        n = read(fileno(stdin), (genericptr_t) &c, 1);
        if (n < 0 && errno == EINTR)
            continue;
        if (n != 1) /* the terminal went away */
            c = 'q';
        switch (c) {
        case ' ':
            nhrec.paused = !nhrec.paused;
            if (!nhrec.paused)
                return;
            break;
        case '.': /* one key, then pause */
            nhrec.paused = TRUE;
            return;
        case '+':
        case '=':
            nhrec.delay_ms = (nhrec.delay_ms > 20L) ? nhrec.delay_ms / 2 : 0L;
            break;
        case '-':
            nhrec.delay_ms = nhrec.delay_ms ? min(nhrec.delay_ms * 2, 4000L)
                                            : 20L;
            break;
        case '>':
            nhrec.delay_ms = 0L;
            nhrec.paused = FALSE;
            return;
        case 'q':
        case 'Q':
        case '\033':
            Sprintf(buf, "stopped by the watcher at turn %ld (%ld"
                         " checkpoints matched)", svm.moves, nhrec.checks);
            nhrec_result(buf, NHREC_STOPPED, FALSE);
            /*NOTREACHED*/
            break;
        default:
            break;
        }
    }
}

/* remove the scratch playground: what the replay made in it (level and
   save files, logs, the links to the real playground), then the directory */
staticfn void
nhrec_rmtree(const char *dir)
{
    char path[BUFSZ], sub[BUFSZ];
    DIR *dp, *sdp;
    struct dirent *de, *sde;

    if (!(dp = opendir(dir)))
        return;
    while ((de = readdir(dp)) != 0) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        Snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
        if (!strcmp(de->d_name, "save") && (sdp = opendir(path)) != 0) {
            while ((sde = readdir(sdp)) != 0) {
                if (sde->d_name[0] == '.')
                    continue;
                Snprintf(sub, sizeof sub, "%s/%s", path, sde->d_name);
                (void) unlink(sub);
            }
            (void) closedir(sdp);
            (void) rmdir(path);
        } else {
            (void) unlink(path); /* (a link is removed, not followed) */
        }
    }
    (void) closedir(dp);
    (void) rmdir(dir);
}
#endif /* UNIX */

/* the outcome of this session's replay: noted in the scratch playground
   (for the last session to show), then either the next session is started
   (chain: after a save, or a session that was cut off, whose level files
   the next one recovers, as the recorded game did) or the replay ends */
staticfn void
nhrec_result(const char *what, enum nhrec_outcome outcome, boolean chain)
{
    char line[BUFSZ], path[BUFSZ];
    FILE *fp;

    Snprintf(line, sizeof line, "session %d: %s%s", nhrec.session,
             (outcome == NHREC_FAILED) ? "failed: "
             : (outcome == NHREC_CUT_OFF) ? "cut off: "
             : (outcome == NHREC_STOPPED) ? "stopped: " : "",
             what);
    if (nhrec.scratch) {
        Snprintf(path, sizeof path, "%s/replay.results", nhrec.scratch);
        if ((fp = nhrec_scratch_file(path, "a")) != 0) {
            (void) fprintf(fp, "%s\n", line);
            (void) fclose(fp);
        }
    }
    if (chain && nhrec.session < nhrec.nsessions)
        nhrec_next_session(outcome); /* doesn't return */
    nhrec_finish(line, outcome);
    /*NOTREACHED*/
}

/* run the next session of the record, in this process's place, in the
   same scratch playground; after a session that was cut off (the recorded
   game was killed there), first do what its server did before the next
   session: recover the checkpoint files into a save file (the recover
   utility, or the game's own recovery) if that session restored a game,
   otherwise clear them (the game does that itself when the old process
   is gone; here it would find this process's id in the lock, since
   execv() keeps it) */
staticfn void
nhrec_next_session(enum nhrec_outcome outcome)
{
#ifdef UNIX
    const char *args[16];
    char sess[20];
    int n = 0;

    if (outcome == NHREC_CUT_OFF) {
        if (nhrec.next_kind && !strcmp(nhrec.next_kind, "restore")) {
#ifdef SELF_RECOVER
            if (!recover_savefile())
                nhrec_finish("the next session restores the game, but its"
                             " checkpoint files couldn't be recovered",
                             NHREC_FAILED);
#else
            nhrec_finish("the next session restores a recovered game,"
                         " which this build can't do (no SELF_RECOVER)",
                         NHREC_FAILED);
#endif
        } else {
            clearlocks();
        }
    }
    nhrec.mode = NHREC_OFF;
    if (iflags.window_inited)
        exit_nhwindows((char *) 0); /* (the terminal back as it was) */
    Sprintf(sess, "%d", nhrec.session + 1);
    args[n++] = nhrec.exe;
    args[n++] = "-d";
    args[n++] = nhrec.scratch;
    args[n++] = "--replay";
    args[n++] = nhrec.record;
    args[n++] = "--session";
    args[n++] = sess;
    args[n++] = "--scratch";
    args[n++] = nhrec.scratch;
    if (nhrec.verify)
        args[n++] = "--verify";
    if (nhrec.seedopt) {
        args[n++] = "--seed";
        args[n++] = nhrec.seedopt;
    }
    args[n] = (const char *) 0;
    feed_replay_next();
    (void) execv(nhrec.exe, (char *const *) args);
    (void) fprintf(stderr, "nethack: can't run the next session (%s): %s\n",
                   nhrec.exe, strerror(errno));
    exit(EXIT_FAILURE);
#endif
}

/* the replay is over: show every session's outcome, tidy up, and exit
   with 0 if the whole record checked out, 1 if a session failed, 2 if one
   was cut off or the watcher stopped it */
staticfn void
nhrec_finish(const char *line, enum nhrec_outcome outcome)
{
    char path[BUFSZ], buf[BUFSZ];
    FILE *fp;
    int status = (outcome == NHREC_FAILED) ? 1
                 : (outcome == NHREC_VERIFIED) ? 0 : 2;
    boolean shown = FALSE;

    nhrec.mode = NHREC_OFF;
    if (iflags.window_inited)
        exit_nhwindows((char *) 0);
    if (outcome != NHREC_CUT_OFF && *gl.lock)
        clearlocks();
    if (nhrec.scratch) {
        Snprintf(path, sizeof path, "%s/replay.results", nhrec.scratch);
        if ((fp = fopen(path, "r")) != 0) {
            while (fgets(buf, sizeof buf, fp)) {
                (void) fputs(buf, stderr);
                shown = TRUE;
                if (strstr(buf, ": failed: "))
                    status = 1;
                else if (status == 0 && (strstr(buf, ": cut off: ")
                                         || strstr(buf, ": stopped: ")))
                    status = 2;
            }
            (void) fclose(fp);
        }
    }
    if (!shown)
        (void) fprintf(stderr, "%s\n", line);
    (void) fprintf(stderr, "replay %s\n",
                   (status == 0) ? "verified"
                   : (status == 1) ? "NOT verified"
                     : "incomplete (not every session could be checked)");
#ifdef UNIX
    if (nhrec.scratch) {
        if (status == 1)
            (void) fprintf(stderr, "(the scratch playground %s is kept)\n",
                           nhrec.scratch);
        else
            nhrec_rmtree(nhrec.scratch);
    }
#endif
    nh_terminate(status);
}

/* replaying: the next event, which should be of one of the given kinds;
   what: what the game wants */
staticfn const struct nhrec_event *
nhrec_next(const char *what, const char *kinds)
{
    static const char *const kindnames[] = {
        "s", "a system seed", "k", "a key", "u", "a permission check",
        "h", "a hangup", "i", "an interrupt", "c", "a checkpoint",
        "e", "the session's end",
    };
    const struct nhrec_event *ev;
    char buf[BUFSZ];
    const char *name = "?";
    int i;

    if (nhrec.next >= nhrec.nevents) {
        if (!nhrec.ends) {
            Sprintf(buf, "replayed every recorded event, %ld checkpoints"
                         " (the session ended without a save or the end of"
                         " the game)", nhrec.checks);
            nhrec_result(buf, NHREC_CUT_OFF, TRUE);
        }
        Sprintf(buf, "the record ends where the game wants %s", what);
        nhrec_result(buf, NHREC_FAILED, FALSE);
    }
    ev = &nhrec.events[nhrec.next];
    if (!strchr(kinds, ev->kind)
        || ((ev->kind == 'h' || ev->kind == 'i') && ev->pass >= 0)) {
        for (i = 0; i < SIZE(kindnames); i += 2)
            if (*kindnames[i] == ev->kind)
                name = kindnames[i + 1];
        Sprintf(buf, "diverged at event %d: the game wants %s, the record"
                     " has %s%s", nhrec.next + 1, what, name,
                (ev->pass >= 0) ? " between turns" : "");
        nhrec_result(buf, NHREC_FAILED, FALSE);
    }
    nhrec.next++;
    return ev;
}

/* TRUE while a game is being recorded or replayed */
boolean
nhrec_active(void)
{
    nhrec_init();
    return (nhrec.mode == NHREC_REPLAYING
            || (nhrec.mode == NHREC_RECORDING && nhrec.fp));
}

/* TRUE while replaying */
boolean
nhrec_replaying(void)
{
    nhrec_init();
    return nhrec.mode == NHREC_REPLAYING;
}

/* a seed drawn from the system: recorded, or when replaying, the recorded
   one instead */
unsigned long
nhrec_seed(unsigned long seed)
{
    char buf[40];

    nhrec_init();
    if (nhrec.mode == NHREC_REPLAYING)
        return nhrec_next("a system seed", "s")->val;
    if (nhrec.mode == NHREC_RECORDING) {
        Sprintf(buf, "%lx", seed);
        nhrec_event("s", buf);
    }
    return seed;
}

/* whether the game reseeds its random numbers when it changes level: as
   the system allows (strong), or when replaying, as the recorded game
   did */
boolean
nhrec_strong_seed(boolean strong)
{
    nhrec_init();
    if (nhrec.mode == NHREC_REPLAYING)
        return nhrec.h_reseed > 0;
    return strong;
}

/* act on an interrupt at the point the record gives */
staticfn void
nhrec_interrupt(void)
{
    nhrec.in_intr = TRUE;
    done_interrupt();
    nhrec.in_intr = FALSE;
}

/* the next key for a recorded or replayed game: read with readkey (which
   gives EOF if the terminal went away, or a signal is waiting) and
   recorded, or when replaying, the recorded one; EOF means a hangup, which
   the caller acts on; an interrupt is acted on here */
int
nhrec_key(int (*readkey)(void))
{
    const struct nhrec_event *ev;
    char buf[40];
    int key;

    nhrec_init();
    for (;;) {
        if (nhrec.mode == NHREC_REPLAYING) {
            ev = nhrec_next("a key", "khi");
            if (ev->kind == 'k') {
#ifdef UNIX
                if (!nhrec.verify)
                    nhrec_pace();
#endif
                return (int) ev->val;
            }
            if (ev->kind == 'h')
                return EOF;
            nhrec_interrupt();
            continue;
        }
        key = (*readkey)();
        if (key == EOF && program_state.pending_intr) {
            program_state.pending_intr = 0;
            /* (another ^C while the first is being answered is dropped) */
            if (!nhrec.in_intr) {
                nhrec_event("intr", "");
                nhrec_interrupt();
            }
            continue;
        }
        if (key == EOF) {
            program_state.pending_hup = 0;
            nhrec_event("hup", "");
            return EOF;
        }
        Sprintf(buf, "%d", key);
        nhrec_event("k", buf);
        return key;
    }
}

/* the start of a pass of moveloop_core(): a hangup or an interrupt that
   arrived meanwhile is acted on here, and recorded; when replaying, one
   recorded for this pass is */
void
nhrec_poll(void)
{
    const struct nhrec_event *ev;
    char buf[40];

    nhrec_init();
    ++nhrec.passes;
    if (nhrec.mode == NHREC_REPLAYING) {
        while (nhrec.next < nhrec.nevents
               && ((ev = &nhrec.events[nhrec.next])->kind == 'h'
                   || ev->kind == 'i')
               && ev->pass == nhrec.passes) {
            nhrec.next++;
            if (ev->kind == 'h') {
#ifdef HANGUPHANDLING
                hangup(0);
#endif
                return;
            }
            nhrec_interrupt();
        }
        return;
    }
    if (nhrec.mode != NHREC_RECORDING)
        return;
    Sprintf(buf, "%ld", nhrec.passes);
    if (program_state.pending_hup) {
        program_state.pending_hup = 0;
        nhrec_event("hup", buf);
#ifdef HANGUPHANDLING
        hangup(0);
#endif
        return;
    }
    if (program_state.pending_intr) {
        program_state.pending_intr = 0;
        nhrec_event("intr", buf);
        nhrec_interrupt();
    }
}

/* the command-line arguments that process_options() handles: noted, or
   when replaying, replaced by the recorded ones */
void
nhrec_args(int *argcp, char ***argvp)
{
    int i;

    nhrec_init();
    if (nhrec.mode == NHREC_REPLAYING) {
        if (!nhrec.argv) {
            nhrec.argv = (char **) alloc((unsigned) ((nhrec.nargs + 2)
                                                     * sizeof (char *)));
            nhrec.argv[0] = (*argvp)[0];
            for (i = 0; i < nhrec.nargs; i++)
                nhrec.argv[i + 1] = nhrec.args[i];
            nhrec.argv[nhrec.nargs + 1] = (char *) 0;
        }
        *argvp = nhrec.argv;
        *argcp = nhrec.nargs + 1;
        return;
    }
    if (nhrec.mode != NHREC_RECORDING)
        return;
    for (i = 1; i < *argcp; i++)
        nhrec_buf_put(&nhrec.hdr, "arg", (*argvp)[i], strlen((*argvp)[i]));
}

/* the login name the hero's name is taken from: noted, or when replaying,
   the recorded one */
const char *
nhrec_login(const char *login)
{
    nhrec_init();
    if (nhrec.mode == NHREC_REPLAYING)
        return nhrec.h_has_login ? nhrec.h_login : (const char *) 0;
    if (nhrec.mode == NHREC_RECORDING && !nhrec.fp)
        nhrec_buf_put(&nhrec.hdr, "login", login ? login : "",
                      login ? strlen(login) : 0);
    return login;
}

/* whether sysconf lets the player do something it allows only some users
   (WIZARDS, EXPLORERS, SHELLERS): the answer, recorded, or when replaying,
   the recorded one, since whoever replays the game isn't the player */
boolean
nhrec_permission(boolean allowed)
{
    nhrec_init();
    if (nhrec.mode == NHREC_REPLAYING)
        return nhrec_next("a permission check", "u")->val != 0;
    if (nhrec.mode == NHREC_RECORDING)
        nhrec_event("u", allowed ? "1" : "0");
    return allowed;
}

/* the player's options are about to be read (rcfile(); envopts:
   NETHACKOPTIONS, when it holds options): the first time, they're noted as
   read (the options file's text by nhrec_options_text()), so the record
   holds what the game parsed; and from now on (sysconf has been read), if
   this game may be recorded, keys are noted, and the signal handlers only
   note a signal (see nhrec_key() and nhrec_poll()) */
void
nhrec_options_begin(const char *envopts)
{
    nhrec_init();
    if (nhrec.mode != NHREC_RECORDING || nhrec.fp || nhrec.opts_taken)
        return;
    if (nhrec_path() && WINDOWPORT(tty))
        program_state.recorded_input = 1;
    nhrec.capturing = TRUE;
    nhrec.rc_read = FALSE;
    nhrec.rctext.len = 0;
    /* (a copy: parsing takes the string apart) */
    if (envopts)
        nhrec.envopts = dupstr(envopts);
}

void
nhrec_options_text(const char *text, size_t len)
{
    size_t need;

    if (!nhrec.capturing)
        return;
    need = nhrec.rctext.len + len + 1;
    if (need > nhrec.rctext.siz) {
        size_t newsiz = max(nhrec.rctext.siz * 2, need + 1024);
        char *newbuf = (char *) alloc((unsigned) newsiz);

        if (nhrec.rctext.buf) {
            (void) memcpy((genericptr_t) newbuf,
                          (genericptr_t) nhrec.rctext.buf, nhrec.rctext.len);
            free((genericptr_t) nhrec.rctext.buf);
        }
        nhrec.rctext.buf = newbuf, nhrec.rctext.siz = newsiz;
    }
    if (len)
        (void) memcpy((genericptr_t) (nhrec.rctext.buf + nhrec.rctext.len),
                      (genericptr_t) text, len);
    nhrec.rctext.len += len;
    nhrec.rc_read = TRUE;
}

/* the options have been read: note them; when replaying a new game,
   check its seed is the recorded game's */
void
nhrec_options_end(void)
{
    char buf[BUFSZ];

    nhrec_init();
    if (nhrec.mode == NHREC_REPLAYING) {
        /* --seed: the server's hidden seed the recorded game had */
        if (nhrec.seedopt)
            nh_set_server_seed(nhrec.seedopt);
        if (!nhrec.opts_taken && nhrec_hidden_record() && !nh_seed_hidden()) {
            Snprintf(buf, sizeof buf,
                     "the record's seed is the server's, %s: give it with"
                     " --seed, or in sysconf's SEED", nhrec_quote(nhrec.h_seed));
            nhrec_result(buf, NHREC_FAILED, FALSE);
        }
        if (!nhrec.opts_taken && !strcmp(nhrec.h_kind, "new")
            && strcmp(nh_seed_display(FALSE), nhrec.h_seed)) {
            Snprintf(buf, sizeof buf,
                     "the record's seed is %s, but the replay's is %s",
                     nhrec_quote(nhrec.h_seed),
                     gseed.server_seed_bad ? "invalid (sysconf SEED)"
                     : !nh_seeded() ? "unset" : nh_seed_display(FALSE));
            nhrec_result(buf, NHREC_FAILED, FALSE);
        }
        nhrec.opts_taken = TRUE;
        return;
    }
    if (!nhrec.capturing)
        return;
    nhrec.capturing = FALSE;
    nhrec.opts_taken = TRUE;
    /* the options may have chosen the interface: only a tty game is
       recorded, and one is from now on, however it began */
    if (!WINDOWPORT(tty)) {
        nhrec_stop();
        return;
    }
    if (nhrec_path())
        program_state.recorded_input = 1;
    if (nhrec.rc_read)
        nhrec_buf_put(&nhrec.hdr, "rcfile", nhrec.rctext.buf,
                      nhrec.rctext.len);
    nhrec_buf_free(&nhrec.rctext);
    if (nhrec.envopts) {
        nhrec_buf_put(&nhrec.hdr, "envopts", nhrec.envopts,
                      strlen(nhrec.envopts));
        free((genericptr_t) nhrec.envopts), nhrec.envopts = (char *) 0;
    }
}

/* where a game is to be recorded (with DUMPLOGFILE's placeholders), if
   anywhere: sysconf's RECORDFILE, or, for a game that doesn't have
   permissions the player lacks, NH_RECORD */
staticfn const char *
nhrec_path(void)
{
    const char *path = (const char *) 0;

#ifdef DUMPLOG
    path = sysopt.recordfile;
#ifdef UNIX
    if (!path && getuid() == geteuid() && getgid() == getegid())
        path = getenv("NH_RECORD");
#endif
#endif
    return (path && *path) ? path : (const char *) 0;
}

/* the game won't be recorded: forget what was gathered, and go back to
   acting on signals when they arrive; a hangup that was waiting is acted
   on now, an interrupt is dropped (the game ignores them while starting) */
staticfn void
nhrec_stop(void)
{
    nhrec.mode = NHREC_OFF;
    nhrec_buf_free(&nhrec.hdr);
    nhrec_buf_free(&nhrec.pending);
    nhrec_buf_free(&nhrec.rctext);
    if (nhrec.envopts)
        free((genericptr_t) nhrec.envopts), nhrec.envopts = (char *) 0;
    if (program_state.recorded_input) {
        program_state.recorded_input = 0;
        program_state.pending_intr = 0;
        if (program_state.pending_hup) {
            program_state.pending_hup = 0;
#ifdef HANGUPHANDLING
            hangup(0);
#endif
        }
    }
}

/* open the record: a new game's must not exist yet (so an existing one is
   never overwritten), a restored one's is appended to; never through a
   symbolic link */
staticfn FILE *
nhrec_open(const char *fname, boolean restoring)
{
    FILE *fp;
    int fd, oflags = O_WRONLY | O_CREAT | (restoring ? O_APPEND : O_EXCL);

#ifdef O_NOFOLLOW
    oflags |= O_NOFOLLOW;
#endif
    if ((fd = open(fname, oflags, FCMASK)) < 0)
        return (FILE *) 0;
    if (!(fp = fdopen(fd, restoring ? "a" : "w")))
        (void) close(fd);
    return fp;
}

/* the game has started or been restored: record it if it's a seeded game
   played with the tty interface and sysconf's RECORDFILE is set (or, for
   a game that doesn't have permissions the player lacks, NH_RECORD); a new
   game is recorded from before anything about it is shown; when
   replaying, check the session is the recorded one */
void
nhrec_game_start(boolean restoring)
{
    char fname[BUFSZ], buf[BUFSZ];
    const char *path = (const char *) 0,
               *gmode = wizard ? "wizard" : discover ? "explore" : "normal";
    int rows = 0, cols = 0;

    nhrec_init();
#ifdef TTY_GRAPHICS
    if (ttyDisplay)
        rows = ttyDisplay->rows, cols = ttyDisplay->cols;
#endif
    Sprintf(buf, "%d %d", rows, cols);
    if (nhrec.mode == NHREC_REPLAYING) {
        const char *differs = !strcmp(nhrec.h_kind, restoring ? "restore"
                                                               : "new")
                                  ? (const char *) 0
                                  : "whether the game was restored";

        if (!differs && strcmp(nhrec.h_name, svp.plname))
            differs = "the hero's name";
        if (!differs && strcmp(nhrec.h_mode, gmode))
            differs = "the play mode";
        if (!differs && strcmp(nhrec.h_seed, nh_seed_display(FALSE)))
            differs = "the seed";
        if (differs) {
            Snprintf(fname, sizeof fname,
                     "the replay isn't the recorded session: %s differs",
                     differs);
            nhrec_result(fname, NHREC_FAILED, FALSE);
        }
        return;
    }
    if (nhrec.mode != NHREC_RECORDING || nhrec.fp)
        return;
    if (!(path = nhrec_path()) || !nh_seeded() || !WINDOWPORT(tty)) {
        nhrec_stop();
        return;
    }
#ifdef DUMPLOG
    (void) dump_fmtstr(path, fname, TRUE);
#else
    Strcpy(fname, path); /* (not reached: nhrec_path() needs DUMPLOG) */
#endif
    if (!(nhrec.fp = nhrec_open(fname, restoring))) {
        nhrec_refuse(strerror(errno), restoring);
        return;
    }
    nhrec_event("session", restoring ? "restore" : "new");
    nhrec_event("name", svp.plname);
    nhrec_event("mode", gmode);
    nhrec_event("term", buf);
    nhrec_event("seed", nh_seed_display(FALSE));
    nhrec_event("reseed", has_strong_rngseed ? "yes" : "no");
    if (nhrec.fp && nhrec.hdr.len
        && fwrite(nhrec.hdr.buf, 1, nhrec.hdr.len, nhrec.fp)
               != nhrec.hdr.len)
        nhrec_write_failed();
    if (nhrec.fp && nhrec.pending.len
        && fwrite(nhrec.pending.buf, 1, nhrec.pending.len, nhrec.fp)
               != nhrec.pending.len)
        nhrec_write_failed();
    nhrec_buf_free(&nhrec.hdr);
    nhrec_buf_free(&nhrec.pending);
    if (nhrec.fp && fflush(nhrec.fp) != 0)
        nhrec_write_failed();
    if (nhrec.failed[0]) {
        /* the header: nothing has been shown yet, so end now */
        Strcpy(buf, nhrec.failed);
        nhrec.failed[0] = '\0';
        program_state.pending_hup = 0;
        nhrec_refuse(buf, restoring);
    }
}

/* a game that can't be recorded isn't played (an unrecorded game couldn't
   be told from one whose record was kept back): say why and end it; a
   restored game is saved again as it was */
staticfn void
nhrec_refuse(const char *why, boolean restoring)
{
    nhrec_stop();
    pline("This game can't be recorded (%s), so it can't go on.", why);
    if (restoring)
        pline("It's saved as it was; please tell the operator.");
    else
        pline("Please tell the operator.");
    display_nhwindow(WIN_MESSAGE, TRUE);
#ifdef HANGUPHANDLING
    if (restoring) {
#ifdef UNIX
        if (discover || wizard) {
            /* the restore kept the save file, unreadable until the keep
               prompt (unixmain.c): keep it as it is */
            preserve_savefile();
        }
#endif
        end_of_input(); /* saves it again, and exits */
        return;
    }
#endif
    exit_nhwindows((char *) 0);
    clearlocks();
    nh_terminate(EXIT_FAILURE);
}

/* a digest of the game's state: turn, where the hero is, their vital
   statistics and inventory, and the current level's contents */
staticfn uint64
nhrec_digest(void)
{
    uint64 h = 0xcbf29ce484222325ULL, parts[NUM_LEVELHASH];
    long vals[14];
    struct obj *o;
    int i;

#define NHREC_MIX(v) (h ^= (uint64) (v), h *= 0x100000001b3ULL)
    vals[0] = svm.moves, vals[1] = u.uz.dnum, vals[2] = u.uz.dlevel;
    vals[3] = u.ux, vals[4] = u.uy, vals[5] = u.uhp, vals[6] = u.uhpmax;
    vals[7] = u.uen, vals[8] = u.uenmax, vals[9] = u.ulevel;
    vals[10] = u.uexp, vals[11] = money_cnt(gi.invent);
    vals[12] = u.uhunger, vals[13] = u.uluck;
    for (i = 0; i < SIZE(vals); i++)
        NHREC_MIX(vals[i]);
    for (o = gi.invent; o; o = o->nobj) {
        NHREC_MIX(o->otyp), NHREC_MIX(o->quan), NHREC_MIX(o->spe);
        NHREC_MIX(o->invlet), NHREC_MIX(o->blessed), NHREC_MIX(o->cursed);
        NHREC_MIX(o->owornmask);
    }
    level_fingerprint(parts);
    for (i = 0; i < NUM_LEVELHASH; i++)
        NHREC_MIX(parts[i]);
#undef NHREC_MIX
    return h;
}

/* a checkpoint (why: "turn" every 100 turns, "level" on arriving on a
   level): record the state's digest, or when replaying, check it, so that
   a replay shows the game stayed the same all along, and where it didn't */
void
nhrec_checkpoint(const char *why)
{
    char buf[BUFSZ], msg[BUFSZ];
    const struct nhrec_event *ev;
    uint64 h;

    if (!nhrec_active())
        return;
    h = nhrec_digest();
    Snprintf(buf, sizeof buf, "%s %ld %08lx%08lx", why, svm.moves,
             (unsigned long) ((h >> 32) & 0xffffffffUL),
             (unsigned long) (h & 0xffffffffUL));
    if (nhrec.mode == NHREC_RECORDING) {
        nhrec_event("c", buf);
        return;
    }
    ev = nhrec_next("a checkpoint", "c");
    if (strcmp(ev->text, buf)) {
        Snprintf(msg, sizeof msg, "diverged at a checkpoint (record \"%s\",",
                 ev->text);
        Snprintf(eos(msg), sizeof msg - strlen(msg), " replay \"%s\")", buf);
        nhrec_result(msg, NHREC_FAILED, FALSE);
    }
    nhrec.checks++;
}

/* a save is starting: note the state's digest now, since saving takes the
   game apart; nhrec_session_end("save") uses it if the save succeeds */
void
nhrec_save_begin(void)
{
    uint64 h;

    if (!nhrec_active())
        return;
    h = nhrec_digest();
    Sprintf(nhrec.saveend, "%08lx%08lx",
            (unsigned long) ((h >> 32) & 0xffffffffUL),
            (unsigned long) (h & 0xffffffffUL));
}

/* a session has ended (how: "save", once the save has succeeded, or
   "done"): record the state's digest, or when replaying, check it; a
   replay ends at the end of the game, before anything is written about
   it (the xlogfile, the high score list, a dumplog) */
void
nhrec_session_end(const char *how)
{
    char buf[BUFSZ], msg[BUFSZ];
    const struct nhrec_event *ev;
    uint64 h;

    if (!nhrec_active())
        return;
    if (!strcmp(how, "save") && nhrec.saveend[0]) {
        Snprintf(buf, sizeof buf, "%s %s", how, nhrec.saveend);
    } else {
        h = nhrec_digest();
        Snprintf(buf, sizeof buf, "%s %08lx%08lx", how,
                 (unsigned long) ((h >> 32) & 0xffffffffUL),
                 (unsigned long) (h & 0xffffffffUL));
    }
    nhrec.saveend[0] = '\0';
    if (nhrec.mode == NHREC_RECORDING) {
        nhrec_event("end", buf);
        return;
    }
    ev = nhrec_next("the session's end", "e");
    if (strcmp(ev->text, buf)) {
        Snprintf(msg, sizeof msg, "the %s state differs (record \"%s\",",
                 how, ev->text);
        Snprintf(eos(msg), sizeof msg - strlen(msg), " replay \"%s\")", buf);
        nhrec_result(msg, NHREC_FAILED, FALSE);
    }
    Snprintf(msg, sizeof msg, "verified (%s, turn %ld, %ld checkpoints)",
             how, svm.moves, nhrec.checks);
    nhrec_result(msg, NHREC_VERIFIED, !strcmp(how, "save"));
    /*NOTREACHED*/
}

/* the game is about to end without a save or the end of the game (how);
   a replay stops here as the recorded game did, and can't check it */
void
nhrec_game_crashed(const char *how)
{
    char buf[BUFSZ];

    if (!nhrec_replaying())
        return;
    Snprintf(buf, sizeof buf, "%s, as in the record (%ld checkpoints)",
             how, nhrec.checks);
    nhrec_result(buf, NHREC_CUT_OFF, TRUE);
}

/* release what the recorder holds, and close the record */
void
nhrec_free(void)
{
    int i;

    if (nhrec.failed[0]) {
        /* (the windows are gone by now) */
        (void) fprintf(stderr, "\nnethack: this game's record couldn't be"
                               " written (%s),\nso the game ended as on a"
                               " hangup.  Please tell the operator.\n",
                       nhrec.failed);
        nhrec.failed[0] = '\0';
    }
    if (nhrec.fp)
        (void) fclose(nhrec.fp), nhrec.fp = (FILE *) 0;
    nhrec_buf_free(&nhrec.hdr);
    nhrec_buf_free(&nhrec.pending);
    nhrec_buf_free(&nhrec.rctext);
    if (nhrec.envopts)
        free((genericptr_t) nhrec.envopts), nhrec.envopts = (char *) 0;
    for (i = 0; i < nhrec.nevents; i++)
        if (nhrec.events[i].text)
            free((genericptr_t) nhrec.events[i].text);
    if (nhrec.events)
        free((genericptr_t) nhrec.events), nhrec.events = 0;
    nhrec.nevents = nhrec.next = 0;
    for (i = 0; i < nhrec.nargs; i++)
        free((genericptr_t) nhrec.args[i]);
    if (nhrec.args)
        free((genericptr_t) nhrec.args), nhrec.args = 0;
    nhrec.nargs = 0;
    if (nhrec.argv)
        free((genericptr_t) nhrec.argv), nhrec.argv = 0;
    if (nhrec.h_kind)
        free((genericptr_t) nhrec.h_kind), nhrec.h_kind = 0;
    if (nhrec.h_name)
        free((genericptr_t) nhrec.h_name), nhrec.h_name = 0;
    if (nhrec.h_mode)
        free((genericptr_t) nhrec.h_mode), nhrec.h_mode = 0;
    if (nhrec.h_term)
        free((genericptr_t) nhrec.h_term), nhrec.h_term = 0;
    if (nhrec.h_seed)
        free((genericptr_t) nhrec.h_seed), nhrec.h_seed = 0;
    if (nhrec.h_login)
        free((genericptr_t) nhrec.h_login), nhrec.h_login = 0;
    if (nhrec.h_rcfile)
        free((genericptr_t) nhrec.h_rcfile), nhrec.h_rcfile = 0;
    if (nhrec.h_envopts)
        free((genericptr_t) nhrec.h_envopts), nhrec.h_envopts = 0;
    if (nhrec.next_kind)
        free((genericptr_t) nhrec.next_kind), nhrec.next_kind = 0;
    if (nhrec.record)
        free((genericptr_t) nhrec.record), nhrec.record = 0;
    if (nhrec.exe)
        free((genericptr_t) nhrec.exe), nhrec.exe = 0;
    if (nhrec.scratch)
        free((genericptr_t) nhrec.scratch), nhrec.scratch = 0;
    if (nhrec.seedopt)
        free((genericptr_t) nhrec.seedopt), nhrec.seedopt = 0;
}
#endif /* !SFCTOOL */

/* ----------  END GAME RECORDING ----------- */

/* ----------  BEGIN WIZKIT FILE HANDLING ----------- */

staticfn FILE *
fopen_wizkit_file(void)
{
    FILE *fp;
#if defined(VMS) || defined(UNIX)
    char tmp_wizkit[BUFSZ];
#endif
    char *envp;

    envp = nh_getenv("WIZKIT");
    if (envp && *envp)
        (void) strncpy(gw.wizkit, envp, WIZKIT_MAX - 1);
    if (!gw.wizkit[0])
        return (FILE *) 0;

#ifdef UNIX
    if (access(gw.wizkit, 4) == -1) {
        /* 4 is R_OK on newer systems */
        /* nasty sneaky attempt to read file through
         * NetHack's setuid permissions -- this is a
         * place a file name may be wholly under the player's
         * control
         */
        raw_printf("Access to %s denied (%d).", gw.wizkit, errno);
        wait_synch();
        /* fall through to standard names */
    } else
#endif
        if ((fp = fopen(gw.wizkit, "r")) != (FILE *) 0) {
        return fp;
#if defined(UNIX) || defined(VMS)
    } else {
        /* access() above probably caught most problems for UNIX */
        raw_printf("Couldn't open requested wizkit file %s (%d).", gw.wizkit,
                   errno);
        wait_synch();
#endif
    }

#if defined(MICRO) || defined(MAC68K) || defined(__BEOS__) || defined(WIN32)
    if ((fp = fopen(fqname(gw.wizkit, CONFIGPREFIX, 0), "r")) != (FILE *) 0)
        return fp;
#else
#ifdef VMS
    envp = nh_getenv("HOME");
    if (envp)
        Sprintf(tmp_wizkit, "%s%s", envp, gw.wizkit);
    else
        Sprintf(tmp_wizkit, "%s%s", "sys$login:", gw.wizkit);
    if ((fp = fopen(tmp_wizkit, "r")) != (FILE *) 0)
        return fp;
#else /* should be only UNIX left */
    envp = nh_getenv("HOME");
    if (envp)
        Sprintf(tmp_wizkit, "%s/%s", envp, gw.wizkit);
    else
        Strcpy(tmp_wizkit, gw.wizkit);
    if ((fp = fopen(tmp_wizkit, "r")) != (FILE *) 0)
        return fp;
    else if (errno != ENOENT) {
        /* e.g., problems when setuid NetHack can't search home
         * directory restricted to user */
        raw_printf("Couldn't open default gw.wizkit file %s (%d).",
                   tmp_wizkit, errno);
        wait_synch();
    }
#endif
#endif
    return (FILE *) 0;
}

/* add to hero's inventory if there's room, otherwise put item on floor */
staticfn void
wizkit_addinv(struct obj *obj)
{
    if (!obj || obj == &hands_obj)
        return;

    /* subset of starting inventory pre-ID */
    observe_object(obj);
    if (Role_if(PM_CLERIC))
        obj->bknown = 1; /* ok to bypass set_bknown() */
    /* same criteria as lift_object()'s check for available inventory slot */
    if (obj->oclass != COIN_CLASS && inv_cnt(FALSE) >= invlet_basic
        && !merge_choice(gi.invent, obj)) {
        /* inventory overflow; can't just place & stack object since
           hero isn't in position yet, so schedule for arrival later */
        add_to_migration(obj);
        obj->ox = 0; /* index of main dungeon */
        obj->oy = 1; /* starting level number */
        obj->owornmask =
            (long) (MIGR_WITH_HERO | MIGR_NOBREAK | MIGR_NOSCATTER);
    } else {
        (void) addinv(obj);
    }
}

boolean
proc_wizkit_line(char *buf)
{
    struct obj *otmp;

    if (strlen(buf) >= BUFSZ)
        buf[BUFSZ - 1] = '\0';
    otmp = readobjnam(buf, (struct obj *) 0);

    if (otmp) {
        if (otmp != &hands_obj) {
            wish_history_add(buf);
            wizkit_addinv(otmp);
        }
    } else {
        /* .60 limits output line width to 79 chars */
        config_error_add("Bad wizkit item: \"%.60s\"", buf);
        return FALSE;
    }
    return TRUE;
}

void
read_wizkit(void)
{
    FILE *fp;

    if (!wizard || !(fp = fopen_wizkit_file()))
        return;

    program_state.wizkit_wishing = 1;
    config_error_init(TRUE, "WIZKIT", FALSE);

    parse_conf_file(fp, proc_wizkit_line);
    (void) fclose(fp);

    config_error_done();
    program_state.wizkit_wishing = 0;

    return;
}

/* ----------  END WIZKIT FILE HANDLING ----------- */

/* ----------  BEGIN SYMSET FILE HANDLING ----------- */

extern const char *const known_handling[];     /* symbols.c */
extern const char *const known_restrictions[]; /* symbols.c */

staticfn FILE *
fopen_sym_file(void)
{
    FILE *fp;

    fp = fopen_datafile(SYMBOLS, "r",
#ifdef WIN32
                            SYSCONFPREFIX
#else
                            HACKPREFIX
#endif
                       );

    return fp;
}

/*
 * Returns 1 if the chosen symset was found and loaded.
 *         0 if it wasn't found in the sym file or other problem.
 */
int
read_sym_file(int which_set)
{
    FILE *fp;

    gs.symset[which_set].explicitly = FALSE;
    if (!(fp = fopen_sym_file()))
        return 0;

    gs.symset[which_set].explicitly = TRUE;
    gc.chosen_symset_start = gc.chosen_symset_end = FALSE;
    gs.symset_which_set = which_set;
    gs.symset_count = 0;

    config_error_init(TRUE, "symbols", FALSE);

    parse_conf_file(fp, proc_symset_line);
    (void) fclose(fp);

    if (!gc.chosen_symset_start && !gc.chosen_symset_end) {
        /* name caller put in symset[which_set].name was not found;
           if it looks like "Default symbols", null it out and return
           success to use the default; otherwise, return failure */
        if (gs.symset[which_set].name
            && (fuzzymatch(gs.symset[which_set].name, "Default symbols",
                           " -_", TRUE)
                || !strcmpi(gs.symset[which_set].name, "default")))
            clear_symsetentry(which_set, TRUE);
        config_error_done();

        /* If name was defined, it was invalid. Then we're loading fallback */
        if (gs.symset[which_set].name) {
            gs.symset[which_set].explicitly = FALSE;
            return 0;
        }

        return 1;
    }
    if (!gc.chosen_symset_end)
        config_error_add("Missing finish for symset \"%s\"",
                         gs.symset[which_set].name ? gs.symset[which_set].name
                                                : "unknown");
    config_error_done();
    return 1;
}

/* ----------  END SYMSET FILE HANDLING ----------- */

/* ----------  BEGIN SCOREBOARD CREATION ----------- */

#ifdef OS2_CODEVIEW
#define UNUSED_if_not_OS2_CODEVIEW /*empty*/
#else
#define UNUSED_if_not_OS2_CODEVIEW UNUSED
#endif

/* verify that we can write to scoreboard file; if not, try to create one */
/*ARGUSED*/
void
check_recordfile(const char *dir UNUSED_if_not_OS2_CODEVIEW)
{
    const char *fq_record;
    int fd;

#if defined(UNIX) || defined(VMS)
    fq_record = fqname(RECORD, SCOREPREFIX, 0);
    fd = open(fq_record, O_RDWR, 0);
    if (fd >= 0) {
#ifdef VMS /* must be stream-lf to use UPDATE_RECORD_IN_PLACE */
        if (!file_is_stmlf(fd)) {
            raw_printf("Warning: scoreboard file '%s'"
                       " is not in stream_lf format",
                       fq_record);
            wait_synch();
        }
#endif
        (void) nhclose(fd); /* RECORD is accessible */
    } else if ((fd = open(fq_record, O_CREAT | O_RDWR, FCMASK)) >= 0) {
        (void) nhclose(fd); /* RECORD newly created */
#if defined(VMS) && !defined(SECURE)
        /* Re-protect RECORD with world:read+write+execute+delete access. */
        (void) chmod(fq_record, FCMASK | 007);
#endif /* VMS && !SECURE */
    } else {
        raw_printf("Warning: cannot write scoreboard file '%s'", fq_record);
        wait_synch();
    }
#endif /* !UNIX && !VMS */
#if defined(MICRO) || defined(WIN32)
    char tmp[PATHLEN];

#ifdef OS2_CODEVIEW /* explicit path on opening for OS/2 */
    /* how does this work when there isn't an explicit path or fopen
     * for later access to the file via fopen_datafile? ? */
    (void) strncpy(tmp, dir, PATHLEN - 1);
    tmp[PATHLEN - 1] = '\0';
    if ((strlen(tmp) + 1 + strlen(RECORD)) < (PATHLEN - 1)) {
        append_slash(tmp);
        Strcat(tmp, RECORD);
    }
    fq_record = tmp;
#else
    Strcpy(tmp, RECORD);
    fq_record = fqname(RECORD, SCOREPREFIX, 0);
#endif
#ifdef WIN32
    /* If dir is NULL it indicates create but
       only if it doesn't already exist */
    if (!dir) {
        char buf[BUFSZ];

        buf[0] = '\0';
        fd = open(fq_record, O_RDWR);
        if (!(fd == -1 && errno == ENOENT)) {
            if (fd >= 0) {
                (void) nhclose(fd);
            } else {
                /* explanation for failure other than missing file */
                Sprintf(buf, "error   \"%s\", (errno %d).",
                        fq_record, errno);
                paniclog("scorefile", buf);
            }
            return;
        }
        Sprintf(buf, "missing \"%s\", creating new scorefile.",
                fq_record);
        paniclog("scorefile", buf);
    }
#endif

    if ((fd = open(fq_record, O_RDWR)) < 0) {
        /* try to create empty 'record' */
#if defined(AZTEC_C) || defined(_DCC) \
    || (defined(__GNUC__) && defined(__AMIGA__))
        /* Aztec doesn't use the third argument */
        /* DICE doesn't like it */
        fd = open(fq_record, O_CREAT | O_RDWR);
#else
        fd = open(fq_record, O_CREAT | O_RDWR, S_IREAD | S_IWRITE);
#endif
        if (fd <= 0) {
            raw_printf("Warning: cannot write record '%s'", tmp);
            wait_synch();
        } else {
            (void) nhclose(fd);
        }
    } else {
        /* open succeeded => 'record' exists */
        (void) nhclose(fd);
    }
#else /* MICRO || WIN32*/

#ifdef MAC68K
    /* Create the "record" file, if necessary */
    fq_record = fqname(RECORD, SCOREPREFIX, 0);
    fd = macopen(fq_record, O_RDWR | O_CREAT, TEXT_TYPE);
    if (fd != -1)
        macclose(fd);
#endif /* MAC68K */

#endif /* MICRO || WIN32*/
}

#undef UNUSED_if_not_OS2_CODEVIEW

/* ----------  END SCOREBOARD CREATION ----------- */

/* ----------  BEGIN PANIC/IMPOSSIBLE/TESTING LOG ----------- */

/*ARGSUSED*/
void
paniclog(
    const char *type,   /* panic, impossible, trickery, [lua] */
    const char *reason) /* explanation */
{
#ifdef PANICLOG
    FILE *lfile;

    if (!program_state.in_paniclog) {
        program_state.in_paniclog = 1;
        lfile = fopen_datafile(PANICLOG, "a", TROUBLEPREFIX);
        if (lfile) {
#ifdef PANICLOG_FMT2
            (void) fprintf(lfile, "%ld %s: %s %s\n",
                           ubirthday, (svp.plname[0] ? svp.plname : "(none)"),
                           type, reason);
#else
            char buf[BUFSZ];
            time_t now = getnow();
            int uid = getuid();
            char playmode = wizard ? 'D' : discover ? 'X' : '-';

            (void) fprintf(lfile, "%s %08ld %06ld %d %c: %s %s\n",
                           version_string(buf, sizeof buf),
                           yyyymmdd(now), hhmmss(now),
                           uid, playmode, type, reason);
#endif /* !PANICLOG_FMT2 */
            (void) fclose(lfile);
        }
        program_state.in_paniclog = 0;
    }
#endif /* PANICLOG */
    return;
}

void
testinglog(const char *filenm,   /* ad hoc file name */
           const char *type,
           const char *reason)   /* explanation */
{
    FILE *lfile;
    char fnbuf[BUFSZ];

    if (!filenm)
        return;
    Strcpy(fnbuf, filenm);
    if (strchr(fnbuf, '.') == 0)
        Strcat(fnbuf, ".log");
    lfile = fopen_datafile(fnbuf, "a", TROUBLEPREFIX);
    if (lfile) {
        (void) fprintf(lfile, "%s\n%s\n", type, reason);
        (void) fclose(lfile);
    }
    return;
}

/* ----------  END PANIC/IMPOSSIBLE/TESTING LOG ----------- */

#ifdef SELF_RECOVER

/* ----------  BEGIN INTERNAL RECOVER ----------- */

extern uchar critical_sizes[], cscbuf[];  /* version.c */

boolean
recover_savefile(void)
{
    NHFILE *gnhfp, *lnhfp, *snhfp;
    int i, lev, savelev, hpid,
        pltmpsiz, cscount = get_critical_size_count();
    xint8 levc;
    struct version_info version_data;
    int processed[256];
    char savename[SAVESIZE], errbuf[BUFSZ], indicator, file_cscount;
    char tmpplbuf[PL_NSIZ_PLUS];
    const char *savewrite_failure = (const char *) 0;
    off_t filesz = 0;

    for (lev = 0; lev < 256; lev++)
        processed[lev] = 0;

    /* level 0 file contains:
     *  pid of creating process (ignored here)
     *  level number for current level of save file
     *  name of save file nethack would have created
     *  player name
     *  and game state
     */

    gnhfp = open_levelfile(0, errbuf);
    if (!gnhfp) {
        raw_printf("%s\n", errbuf);
        return FALSE;
    }
    filesz = lseek(gnhfp->fd, 0L, SEEK_END);
    (void) lseek(gnhfp->fd, 0L, SEEK_SET);
    if ((size_t) filesz <  (sizeof hpid
                   + sizeof lev
                   + sizeof savename
                   + sizeof indicator
                   + sizeof file_cscount
                   + sizeof version_data + sizeof pltmpsiz)) {
        const char *fq_save;

        /* this indicates a .0 file that was created as part of
         * recover that did not complete. There could be an intact
         * savefile already there. Check for that and return TRUE
         * if there is. */
        set_savefile_name(TRUE);
        fq_save = fqname(gs.SAVEF, SAVEPREFIX, 0);
        if (access(fq_save, F_OK) == 0) {
            close_nhfile(gnhfp);
            delete_levelfile(0);
            return TRUE;
        } else {
            /* savefile doesn't exist, so fall through */
        }
    }
    if (read(gnhfp->fd, (genericptr_t) &hpid, sizeof hpid) != sizeof hpid) {
        raw_printf("\n%s\n%s\n",
                   "Checkpoint data incompletely written"
                   " or subsequently clobbered.",
                   "Recovery impossible.");
        close_nhfile(gnhfp);
        return FALSE;
    }
    if (read(gnhfp->fd, (genericptr_t) &savelev, sizeof(savelev))
        != sizeof(savelev)) {
        raw_printf("\n%s %s %s\n",
                   "Checkpointing was not in effect for",
                   gl.lock,
                   "-- recovery impossible.");
        close_nhfile(gnhfp);
        return FALSE;
    }
    if ((read(gnhfp->fd, (genericptr_t) savename, sizeof savename)
         != sizeof savename)
        || (read(gnhfp->fd, (genericptr_t) &indicator, sizeof indicator)
            != sizeof indicator)
        || indicator != 'h'
        || (read(gnhfp->fd, (genericptr_t) &file_cscount, sizeof file_cscount)
            != sizeof file_cscount)
        || (unsigned char) file_cscount != (unsigned) cscount
        || (read(gnhfp->fd, (genericptr_t) &cscbuf,
                 (unsigned char) file_cscount)
            != (unsigned char) file_cscount)
        || (read(gnhfp->fd, (genericptr_t) &version_data, sizeof version_data)
            != sizeof version_data)
        || !recovery_header_compatible(cscbuf, &version_data)
        || (read(gnhfp->fd, (genericptr_t) &pltmpsiz, sizeof pltmpsiz)
            != sizeof pltmpsiz) || pltmpsiz < 0 || pltmpsiz > PL_NSIZ_PLUS
        || (read(gnhfp->fd, (genericptr_t) &tmpplbuf, pltmpsiz)
            != pltmpsiz)) {
        raw_printf("\nError reading %s -- can't recover.\n", gl.lock);
        close_nhfile(gnhfp);
        return FALSE;
    }

    /* save file should contain:
     *  format indicator (1 byte)
     *  n = count of critical size list (1 byte)
     *  n bytes of critical sizes (n bytes)
     *  version info
     *  plnametmp = player name size (int, 2 bytes)
     *  player name (PL_NSIZ_PLUS)
     *  current level (including pets)
     *  (non-level-based) game state
     *  other levels
     */

    /*
     *
     * Set a flag for the savefile routines to know the
     * circumstances and act accordingly:
     *    program_state.in_self_recover
     */
    program_state.in_self_recover = TRUE;
    set_savefile_name(TRUE);
    snhfp = create_savefile();
    if (!snhfp) {
        raw_printf("\nCannot recover savefile %s.\n", gs.SAVEF);
        close_nhfile(gnhfp);
        return FALSE;
    }

    lnhfp = open_levelfile(savelev, errbuf);
    if (!lnhfp) {
        raw_printf("\n%s\n", errbuf);
        close_nhfile(gnhfp);
        close_nhfile(snhfp);
        delete_savefile();
        return FALSE;
    }

    /* the checkpoint's own version info, which says among other things
       whether the game was seeded (and so what the rest holds), whatever
       this session's options say */
    if (snhfp->structlevel)
        bufoff(snhfp->fd);
    Sfo_char(snhfp, &indicator, "indicate-format", 1);
    Sfo_char(snhfp, &file_cscount, "count-critical_sizes", 1);
    for (i = 0; i < (int) (unsigned char) file_cscount; ++i)
        Sfo_uchar(snhfp, &cscbuf[i], "critical_sizes");
    Sfo_version_info(snhfp, &version_data, "version_info");

    if (savewrite_failure)
        goto cleanup;

    /* TODO: this is not a single byte, so a big-endian byte swap
     * might be necessary here, if anyone is concerned about big-endian */
    Sfo_int(snhfp, &pltmpsiz, "plname-size");
    savewrite_failure = (const char *) 0;
    if (savewrite_failure)
        goto cleanup;

    Sfo_char(snhfp, tmpplbuf, "plname", pltmpsiz);
    savewrite_failure = (const char *) 0;
    if (savewrite_failure)
        goto cleanup;

    if (!copy_bytes(lnhfp->fd, snhfp->fd)) {
        close_nhfile(gnhfp);
        close_nhfile(snhfp);
        close_nhfile(lnhfp);
        delete_savefile();
        return FALSE;
    }
    close_nhfile(lnhfp);
    processed[savelev] = 1;

    if (!copy_bytes(gnhfp->fd, snhfp->fd)) {
        close_nhfile(gnhfp);
        close_nhfile(snhfp);
        delete_savefile();
        return FALSE;
    }
    close_nhfile(gnhfp);
    processed[0] = 1;

    for (lev = 1; lev < 256; lev++) {
        /* level numbers are kept in xint8's in save.c, so the
         * maximum level number (for the endlevel) must be < 256
         */
        if (lev != savelev) {
            lnhfp = open_levelfile(lev, (char *) 0);
            if (lnhfp) {
                /* any or all of these may not exist */
                levc = (xint8) lev;
                (void) write(snhfp->fd, (genericptr_t) &levc, sizeof(levc));
                if (!copy_bytes(lnhfp->fd, snhfp->fd)) {
                    close_nhfile(lnhfp);
                    close_nhfile(snhfp);
                    delete_savefile();
                    return FALSE;
                }
                close_nhfile(lnhfp);
                processed[lev] = 1;
            }
        }
    }
    if (snhfp->structlevel)
        bufon(snhfp->fd);
    close_nhfile(snhfp);
    /*
     * We have a successful savefile!
     * Only now do we erase the level files.
     */
    for (lev = 0; lev < 256; lev++) {
        if (processed[lev]) {
            const char *fq_lock;

            set_levelfile_name(gl.lock, lev);
            fq_lock = fqname(gl.lock, LEVELPREFIX, 3);
            (void) unlink(fq_lock);
        }
    }
 cleanup:
    if (savewrite_failure) {
        raw_printf("\nError writing %s; recovery failed (%s).\n",
                   gs.SAVEF, savewrite_failure);
        close_nhfile(gnhfp);
        close_nhfile(snhfp);
        close_nhfile(lnhfp);
        program_state.in_self_recover = FALSE;
        delete_savefile();
        return FALSE;
    }
    /* we don't clear program_state.in_self_recover here, we
       leave it as a flag to reload the structlevel savefile
       in the caller. The caller should then clear it. */
    return TRUE;
}

/* ----------  END INTERNAL RECOVER ----------- */
#endif /*SELF_RECOVER*/

/* ----------  OTHER ----------- */

ATTRNORETURN void
do_deferred_showpaths(int code)
{
    gd.deferred_showpaths = FALSE;
    reveal_paths(code);

    /* cleanup before heading to an exit */
    freedynamicdata();
    dlb_cleanup();
    l_nhcore_done();

#ifdef UNIX
    after_opt_showpaths(gd.deferred_showpaths_dir);
#else
#ifndef WIN32
#ifdef CHDIR
    chdirx(gd.deferred_showpaths_dir, 0);
#endif
#endif
#if defined(WIN32) || defined(MICRO) || defined(OS2)
    nethack_exit(EXIT_SUCCESS);
#else
    exit(EXIT_SUCCESS);
#endif
#endif
}
#endif /* !SFCTOOL */

#ifdef DEBUG
/* used by debugpline() to decide whether to issue a message
 * from a particular source file; caller passes __FILE__ and we check
 * whether it is in the source file list supplied by SYSCF's DEBUGFILES
 *
 * pass FALSE to override wildcard matching; useful for files
 * like dungeon.c and questpgr.c, which generate a ridiculous amount of
 * output if DEBUG is defined and effectively block the use of a wildcard */
boolean
debugcore(const char *filename, boolean wildcards)
{
    const char *debugfiles, *p;

    /* debugpline() messages might disclose information that the player
       doesn't normally get to see, so only display them in wizard mode */
    if (!wizard)
        return FALSE;

    if (!filename || !*filename)
        return FALSE; /* sanity precaution */

    debugfiles = sysopt.debugfiles;
    /* usual case: sysopt.debugfiles will be empty */
    if (!debugfiles || !*debugfiles)
        return FALSE;

    /* strip filename's path if present */
    filename = nh_basename(filename, TRUE);

    /*
     * Wildcard match will only work if there's a single pattern (which
     * might be a single file name without any wildcarding) rather than
     * a space-separated list.
     * [to NOT do: We could step through the space-separated list and
     * attempt a wildcard match against each element, but that would be
     * overkill for the intended usage.]
     */
    if (wildcards && pmatch(debugfiles, filename))
        return TRUE;

    /* check whether filename is an element of the list */
    if ((p = strstr(debugfiles, filename)) != 0) {
        int l = (int) strlen(filename);

        if ((p == debugfiles || p[-1] == ' ' || p[-1] == '/')
            && (p[l] == ' ' || p[l] == '\0'))
            return TRUE;
    }
    return FALSE;
}

#endif /*DEBUG*/

#ifndef SFCTOOL

#define SYSCONFFILE "system configuration file"

void
reveal_paths(int code)
{
#if defined(SYSCF)
    boolean skip_sysopt = FALSE;
#endif
    const char *fqn, *nodumpreason;

    char buf[BUFSZ];
#if defined(SYSCF) || !defined(UNIX) || defined(DLB)
    const char *filep;
#ifdef SYSCF
    const char *gamename = (gh.hname && *gh.hname) ? gh.hname : "NetHack";
#endif
#endif
#if defined(PREFIXES_IN_USE)
    const char *cstrp;
#endif
#ifdef UNIX
    char *endp, *envp, cwdbuf[PATH_MAX];
#endif
#ifdef PREFIXES_IN_USE
    int i, maxlen = 0;

    raw_print("Variable playground locations:");
    for (i = 0; i < PREFIX_COUNT; i++)
        raw_printf("    [%-10s]=\"%s\"", fqn_prefix_names[i],
                   gf.fqn_prefix[i] ? gf.fqn_prefix[i] : "not set");
#endif

    /* sysconf file */

#ifdef SYSCF
#ifdef PREFIXES_IN_USE
    cstrp = fqn_prefix_names[SYSCONFPREFIX];
    maxlen = BUFSZ - sizeof " (in )";
    if (cstrp && (int) strlen(cstrp) < maxlen)
        Sprintf(buf, " (in %s)", cstrp);
#else
    buf[0] = '\0';
#endif
    raw_printf("%s %s%s:", s_suffix(gamename),
               SYSCONFFILE, buf);
#ifdef SYSCF_FILE
    filep = SYSCF_FILE;
#else
    filep = "sysconf";
#endif
    fqn = fqname(filep, SYSCONFPREFIX, 0);
    if (fqn) {
        set_configfile_name(fqn);
        filep = get_configfile();
    }
    raw_printf("    \"%s\"", filep);
    if (code == 1) {
        raw_printf("NOTE: The %s above is missing or inaccessible!",
                   SYSCONFFILE);
        skip_sysopt = TRUE;
    }
#else /* !SYSCF */
    raw_printf("No system configuration file.");
#endif /* ?SYSCF */

    /* symbols file */

    buf[0] = '\0';
#ifndef UNIX
#ifdef PREFIXES_IN_USE
#ifdef WIN32
    cstrp = fqn_prefix_names[SYSCONFPREFIX];
#else
    cstrp = fqn_prefix_names[HACKPREFIX];
#endif /* WIN32 */
    maxlen = BUFSZ - sizeof " (in )";
    if (cstrp && (int) strlen(cstrp) < maxlen)
        Sprintf(buf, " (in %s)", cstrp);
#endif /* PREFIXES_IN_USE */
    raw_printf("The loadable symbols file%s:", buf);
#endif /* UNIX */

#ifdef UNIX
    envp = getcwd(cwdbuf, PATH_MAX);
    if (envp) {
        raw_print("The loadable symbols file:");
        raw_printf("    \"%s/%s\"", envp, SYMBOLS);
    }
#else /* UNIX */
    filep = SYMBOLS;
#ifdef PREFIXES_IN_USE
#ifdef WIN32
    fqn = fqname(filep, SYSCONFPREFIX, 1);
#else
    fqn = fqname(filep, HACKPREFIX, 1);
#endif /* WIN32 */
    if (fqn)
        filep = fqn;
#endif /* PREFIXES_IN_USE */
    raw_printf("    \"%s\"", filep);
#endif /* UNIX */

    /* dlb vs non-dlb */

    buf[0] = '\0';
#ifdef PREFIXES_IN_USE
    cstrp = fqn_prefix_names[DATAPREFIX];
    maxlen = BUFSZ - sizeof " (in )";
    if (cstrp && (int) strlen(cstrp) < maxlen)
        Sprintf(buf, " (in %s)", cstrp);
#endif
#ifdef DLB
    raw_printf("Basic data files%s are collected inside:", buf);
    filep = DLBFILE;
#ifdef VERSION_IN_DLB_FILENAME
    Strcpy(buf, build_dlb_filename((const char *) 0));
#ifdef PREFIXES_IN_USE
    fqn = fqname(buf, DATAPREFIX, 1);
    if (fqn)
        filep = fqn;
#endif /* PREFIXES_IN_USE */
#endif
    raw_printf("    \"%s\"", filep);
#ifdef DLBFILE2
    filep = DLBFILE2;
    raw_printf("    \"%s\"", filep);
#endif
#else /* !DLB */
    raw_printf("Basic data files%s are in many separate files.", buf);
#endif /* ?DLB */

    /* dumplog */

    fqn = (char *) 0;
#ifndef DUMPLOG
    nodumpreason = "not supported";
#else
    nodumpreason = "disabled";
#ifdef SYSCF
    if (!skip_sysopt) {
        fqn = sysopt.dumplogfile;
        if (!fqn)
            nodumpreason = "DUMPLOGFILE is not set in " SYSCONFFILE;
    } else {
        nodumpreason = SYSCONFFILE " is missing; no DUMPLOGFILE setting";
    }
#else  /* !SYSCF */
#ifdef DUMPLOG_FILE
    fqn = DUMPLOG_FILE;
#endif
#endif /* ?SYSCF */
    if (fqn && *fqn) {
        raw_print("Your end-of-game disclosure file:");
        (void) dump_fmtstr(fqn, buf, FALSE);
        buf[sizeof buf - sizeof "    \"\""] = '\0';
        raw_printf("    \"%s\"", buf);
    } else {
        raw_printf("No end-of-game disclosure file (%s).", nodumpreason);
    }
#endif /* ?DUMPLOG */

#ifdef SYSCF
#ifdef WIN32
    if (!skip_sysopt) {
        if (sysopt.portable_device_paths) {
            const char *pd = get_portable_device();

            /* an empty value for pd indicates that portable_device_paths
               got set TRUE in a sysconf file other than the one containing
               the executable; disregard it */
            if (strlen(pd) > 0) {
                raw_printf("portable_device_paths (set in sysconf):");
                raw_printf("    \"%s\"", pd);
            }
        }
    }
#endif
#endif

    /* personal configuration file */

    buf[0] = '\0';
#ifdef PREFIXES_IN_USE
    cstrp = fqn_prefix_names[CONFIGPREFIX];
    maxlen = BUFSZ - sizeof " (in )";
    if (cstrp && (int) strlen(cstrp) < maxlen)
        Sprintf(buf, " (in %s)", cstrp);
#endif /* PREFIXES_IN_USE */
    raw_printf("Your personal configuration file%s:", buf);

#ifdef UNIX
    buf[0] = '\0';
    if ((envp = nh_getenv("HOME")) != 0) {
        copynchars(buf, envp, (int) sizeof buf - 1 - 1);
        Strcat(buf, "/");
    }
    endp = eos(buf);
    copynchars(endp, get_default_configfile(),
               (int) (sizeof buf - 1 - strlen(buf)));
#if defined(__APPLE__) /* UNIX+__APPLE__ => MacOSX aka OSX aka macOS */
    if (envp) {
        if (access(buf, 4) == -1) { /* 4: R_OK, -1: failure */
            /* read access to default failed; might be protected excessively
               but more likely it doesn't exist; try first alternate:
               "$HOME/Library/Pref..."; 'endp' points past '/' */
            copynchars(endp, "Library/Preferences/NetHack Defaults",
                       (int) (sizeof buf - 1 - strlen(buf)));
            if (access(buf, 4) == -1) {
                /* first alternate failed, try second:
                   ".../NetHack Defaults.txt"; no 'endp', just append */
                copynchars(eos(buf), ".txt",
                           (int) (sizeof buf - 1 - strlen(buf)));
                if (access(buf, 4) == -1) {
                    /* second alternate failed too, so revert to the
                       original default ("$HOME/.nethackrc") for message */
                    copynchars(endp, get_default_configfile(),
                               (int) (sizeof buf - 1 - strlen(buf)));
                }
            }
        }
    }
#endif /* __APPLE__ */
    raw_printf("    \"%s\"", buf);
#else /* !UNIX */
    fqn = (const char *) 0;
#ifdef PREFIXES_IN_USE
    fqn = fqname(get_default_configfile(), CONFIGPREFIX, 2);
#endif
    raw_printf("    \"%s\"", fqn ? fqn : get_default_configfile());
#endif  /* ?UNIX */

    raw_print("");
#if defined(WIN32) && !defined(WIN32CON)
    wait_synch();
#endif
#ifndef DUMPLOG
#ifdef SYSCF
    nhUse(skip_sysopt);
#endif
    nhUse(nodumpreason);
#endif
}

/* ----------  BEGIN TRIBUTE ----------- */

/* 3.6 tribute code
 */

#define SECTIONSCOPE 1
#define TITLESCOPE 2
#define PASSAGESCOPE 3

#define MAXPASSAGES SIZE(svc.context.novel.pasg) /* 20 */

staticfn int choose_passage(int, unsigned);

/* choose a random passage that hasn't been chosen yet; once all have
   been chosen, reset the tracking to make all passages available again */
staticfn int
choose_passage(int passagecnt, /* total of available passages */
               unsigned oid)   /* book.o_id, used to determine whether
                                  re-reading same book */
{
    int idx, res;

    if (passagecnt < 1)
        return 0;

    /* if a different book or we've used up all the passages already,
       reset in order to have all 'passagecnt' passages available */
    if (oid != svc.context.novel.id || svc.context.novel.count == 0) {
        int i, range = passagecnt, limit = MAXPASSAGES;

        svc.context.novel.id = oid;
        if (range <= limit) {
            /* collect all of the N indices */
            svc.context.novel.count = passagecnt;
            for (idx = 0; idx < MAXPASSAGES; idx++)
                svc.context.novel.pasg[idx] = (xint16) ((idx < passagecnt)
                                                   ? idx + 1 : 0);
        } else {
            /* collect MAXPASSAGES of the N indices */
            svc.context.novel.count = MAXPASSAGES;
            for (idx = i = 0; i < passagecnt; ++i, --range)
                if (range > 0 && rn2(range) < limit) {
                    svc.context.novel.pasg[idx++] = (xint16) (i + 1);
                    --limit;
                }
        }
    }

    idx = rn2(svc.context.novel.count);
    res = (int) svc.context.novel.pasg[idx];
    /* move the last slot's passage index into the slot just used
       and reduce the number of passages available */
    svc.context.novel.pasg[idx]
                          = svc.context.novel.pasg[--svc.context.novel.count];
    return res;
}

/* Returns True if you were able to read something. */
boolean
read_tribute(const char *tribsection, const char *tribtitle,
             int tribpassage, char *nowin_buf, int bufsz,
             unsigned oid) /* book identifier */
{
    dlb *fp;
    char line[BUFSZ], lastline[BUFSZ];

    int scope = 0;
    int linect = 0, passagecnt = 0, targetpassage = 0;
    const char *badtranslation = "an incomprehensible foreign translation";
    boolean matchedsection = FALSE, matchedtitle = FALSE;
    winid tribwin = WIN_ERR;
    boolean grasped = FALSE;
    boolean foundpassage = FALSE;

    if (nowin_buf)
        *nowin_buf = '\0';

    /* check for mandatories */
    if (!tribsection || !tribtitle) {
        if (!nowin_buf)
            pline("It's %s of \"%s\"!", badtranslation, tribtitle);
        return grasped;
    }

    debugpline3("read_tribute %s, %s, %d.", tribsection, tribtitle,
                tribpassage);

    fp = dlb_fopen(TRIBUTEFILE, "r");
    if (!fp) {
        /* this is actually an error - cannot open tribute file! */
        if (!nowin_buf)
            You_feel("too overwhelmed to continue!");
        return grasped;
    }

    /*
     * Syntax (not case-sensitive):
     *  %section books
     *
     * In the books section:
     *    %title booktitle (n)
     *          where booktitle=book title without quotes
     *          (n)= total number of passages present for this title
     *    %passage k
     *          where k=sequential passage number
     *
     * %e ends the passage/book/section
     *    If in a passage, it marks the end of that passage.
     *    If in a book, it marks the end of that book.
     *    If in a section, it marks the end of that section.
     *
     *  %section death
     */

    *line = *lastline = '\0';
    while (dlb_fgets(line, sizeof line, fp) != 0) {
        linect++;
        (void) strip_newline(line);
        switch (line[0]) {
        case '%':
            if (!strncmpi(&line[1], "section ", sizeof "section " - 1)) {
                char *st = &line[9]; /* 9 from "%section " */

                scope = SECTIONSCOPE;
                matchedsection = !strcmpi(st, tribsection) ? TRUE : FALSE;
            } else if (!strncmpi(&line[1], "title ", sizeof "title " - 1)) {
                char *st = &line[7]; /* 7 from "%title " */
                char *p1, *p2;

                if ((p1 = strchr(st, '(')) != 0) {
                    *p1++ = '\0';
                    (void) mungspaces(st);
                    if ((p2 = strchr(p1, ')')) != 0) {
                        *p2 = '\0';
                        passagecnt = atoi(p1);
                        scope = TITLESCOPE;
                        if (matchedsection && !strcmpi(st, tribtitle)) {
                            matchedtitle = TRUE;
                            targetpassage = !tribpassage
                                             ? choose_passage(passagecnt, oid)
                                             : (tribpassage <= passagecnt)
                                                ? tribpassage : 0;
                        } else {
                            matchedtitle = FALSE;
                        }
                    }
                }
            } else if (!strncmpi(&line[1], "passage ",
                                 sizeof "passage " - 1)) {
                int passagenum = 0;
                char *st = &line[9]; /* 9 from "%passage " */

                mungspaces(st);
                passagenum = atoi(st);
                if (passagenum > 0 && passagenum <= passagecnt) {
                    scope = PASSAGESCOPE;
                    if (matchedtitle && passagenum == targetpassage) {
                        foundpassage = TRUE;
                        if (!nowin_buf) {
                            tribwin = create_nhwindow(NHW_MENU);
                            if (tribwin == WIN_ERR)
                                goto cleanup;
                        }
                    }
                }
            } else if (!strncmpi(&line[1], "e ", sizeof "e " - 1)) {
                if (foundpassage)
                    goto cleanup;
                if (scope == TITLESCOPE)
                    matchedtitle = FALSE;
                if (scope == SECTIONSCOPE)
                    matchedsection = FALSE;
                if (scope)
                    --scope;
            } else {
                debugpline1("tribute file error: bad %% command, line %d.",
                            linect);
            }
            break;
        case '#':
            /* comment only, next! */
            break;
        default:
            if (foundpassage) {
                if (!nowin_buf) {
                    /* outputting multi-line passage to text window */
                    putstr(tribwin, 0, line);
                    if (*line)
                        Strcpy(lastline, line);
                } else {
                    /* fetching one-line passage into buffer */
                    copynchars(nowin_buf, line, bufsz - 1);
                    goto cleanup; /* don't wait for "%e passage" */
                }
            }
        }
    }

 cleanup:
    (void) dlb_fclose(fp);
    if (nowin_buf) {
        /* one-line buffer */
        grasped = *nowin_buf ? TRUE : FALSE;
    } else {
        if (tribwin != WIN_ERR) { /* implies 'foundpassage' */
            /* multi-line window, normal case;
               if lastline is empty, there were no non-empty lines between
               "%passage n" and "%e passage" so we leave 'grasped' False */
            if (*lastline) {
                char *p;

                display_nhwindow(tribwin, FALSE);
                /* put the final attribution line into message history,
                   analogous to the summary line from long quest messages */
                if (strchr(lastline, '['))
                    mungspaces(lastline); /* to remove leading spaces */
                else /* construct one if necessary */
                    Sprintf(lastline, "[%s, by Terry Pratchett]", tribtitle);
                if ((p = strrchr(lastline, ']')) != 0)
                    Sprintf(p, "; passage #%d]", targetpassage);
                putmsghistory(lastline, FALSE);
                grasped = TRUE;
            }
            destroy_nhwindow(tribwin);
        }
        if (!grasped)
            /* multi-line window, problem */
            pline("It seems to be %s of \"%s\"!", badtranslation, tribtitle);
    }
    return grasped;
}

boolean
Death_quote(char *buf, int bufsz)
{
    unsigned death_oid = 1; /* chance of oid #1 being a novel is negligible */

    return read_tribute("Death", "Death Quotes", 0, buf, bufsz, death_oid);
}

/* ----------  END TRIBUTE ----------- */
#endif /* !SFCTOOL */

#ifdef LIVELOG
#define LLOG_SEP "\t" /* livelog field separator, as a string literal */
#define LLOG_EOL "\n" /* end-of-line, for abstraction consistency */

/* Locks the live log file and writes 'buffer'
 * iff the ll_type matches sysopt.livelog mask.
 * lltype is included in LL entry for post-process filtering also.
 */
void
livelog_add(long ll_type, const char *str)
{
    FILE *livelogfile;
    time_t now;
    int gindx, aindx;

    if (!(ll_type & sysopt.livelog))
        return;

    if (lock_file(LIVELOGFILE, SCOREPREFIX, 10)) {
        if (!(livelogfile = fopen_datafile(LIVELOGFILE, "a", SCOREPREFIX))) {
            pline("Cannot open live log file!");
            unlock_file(LIVELOGFILE);
            return;
        }

        now = getnow();
        gindx = flags.female ? 1 : 0;
        /* note on alignment designation:
               aligns[] uses [0] lawful, [1] neutral, [2] chaotic;
               u.ualign.type uses -1 chaotic, 0 neutral, 1 lawful;
           so subtracting from 1 converts from either to the other */
        aindx = 1 - u.ualign.type;
        /* format relies on STD C's implicit concatenation of
           adjacent string literals */
        (void) fprintf(livelogfile,
                       "lltype=%ld"  LLOG_SEP  "name=%s"       LLOG_SEP
                       "role=%s"     LLOG_SEP  "race=%s"       LLOG_SEP
                       "gender=%s"   LLOG_SEP  "align=%s"      LLOG_SEP
                       "turns=%ld"   LLOG_SEP  "starttime=%ld" LLOG_SEP
                       "curtime=%ld" LLOG_SEP  "message=%s"    LLOG_EOL,
                       (ll_type & sysopt.livelog), svp.plname,
                       gu.urole.filecode, gu.urace.filecode,
                       genders[gindx].filecode, aligns[aindx].filecode,
                       svm.moves, timet_to_seconds(ubirthday),
                       timet_to_seconds(now), str);
        (void) fclose(livelogfile);
        unlock_file(LIVELOGFILE);
    }
}
#undef LLOG_SEP
#undef LLOG_EOL

#else

void
livelog_add(long ll_type UNUSED, const char *str UNUSED)
{
    /* nothing here */
}

#endif /* !LIVELOG */

/*files.c*/
