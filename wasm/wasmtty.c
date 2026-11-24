/* NetHack's tty port in a browser: keys and pauses yield to the page.
 * The page supplies Module.nhKey(), a promise of the next key's byte
 * (or -1 when the terminal is gone), and draws what the game writes to
 * stdout in a terminal emulator. */

#include <stdio.h>
#include <emscripten.h>

EM_ASYNC_JS(int, nhwasm_wait_key, (void), {
    return await Module.nhKey();
});

int nhwasm_getch(void);
void nhwasm_sleep(unsigned);
void tputs(const char *, int, int (*)(int));

/* what a termcap library would supply: the ANSI sequences need no padding */
short ospeed = 0;

void
tputs(const char *s, int affcnt, int (*outc)(int))
{
    (void) affcnt;
    while (*s) {
        if (s[0] == '$' && s[1] == '<') { /* a padding delay: skip it */
            while (*s && *s != '>')
                s++;
            if (*s)
                s++;
            continue;
        }
        (void) outc((unsigned char) *s++);
    }
}

int
nhwasm_getch(void)
{
    int c;

    (void) fflush(stdout);
    c = nhwasm_wait_key();
    return (c < 0) ? EOF : c;
}

void
nhwasm_sleep(unsigned ms)
{
    (void) fflush(stdout);
    emscripten_sleep(ms);
}
