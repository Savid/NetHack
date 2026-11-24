#!/bin/sh
# Build NetHack's tty port for the browser with emscripten:
#   wasm/build.sh PLAYGROUND [OUT]
# PLAYGROUND is a native install of the same commit (make all install),
# whose nhdat, symbols and license the browser build carries as they are.
# Writes OUT (default wasm/build): nethack.mjs, nethack.wasm, nethack.data
# and BUILD, the commit it was built from.
#
# The C code sees 64-bit longs and pointers (emscripten's MEMORY64=2,
# lowered to ordinary wasm32, so every browser runs it): the data model of
# the Linux x86_64 and aarch64 builds, so their save files, snapshots
# included (Seeding B15), restore here as they are, and its saves there.
# nethack.mjs is an ES module (MODULARIZE, EXPORT_ES6) exporting the
# factory; see wasm/README for what a page passes it.
set -eu
cd "$(dirname "$0")/.."
playground=$1
out=${2:-wasm/build}
obj=$out/obj
data=$out/data
m64=-sMEMORY64=2
mkdir -p "$obj" "$data/save"

sha=$(git rev-parse HEAD)
branch=$(git rev-parse --abbrev-ref HEAD)
CFLAGS="-O2 $m64 -I../include -I../nhlua/lua/src -Wall -Wextra
 -Wno-missing-field-initializers
 -DDLB -DHACKDIR=\"/nethack\" -DDEFAULT_WINDOW_SYS=\"tty\"
 -DSYSCF -DSYSCF_FILE=\"/nethack/sysconf\" -DSECURE -DTIMED_DELAY -DDUMPLOG
 -DCONFIG_ERROR_SECURE=FALSE -DSELF_RECOVER -D_DEFAULT_SOURCE
 -D_XOPEN_SOURCE=600 -DNO_FILE_LINKS -DNO_TERMCAP_HEADERS -DNOMAIL
 -DDEV_RANDOM=\"/dev/urandom\" -DZLIB_COMP -sUSE_ZLIB=1"
export CFLAGS

# Lua, as the game's own Makefile builds it (no lua.c/luac.c)
: > "$obj/jobs"
for f in nhlua/lua/src/*.c; do
    b=$(basename "$f" .c)
    case $b in lua|luac|onelua|ltests) continue ;; esac
    echo "emcc -O2 $m64 -DLUA_USE_POSIX -c -o $obj/lua-$b.o $f 2>&1" \
        >> "$obj/jobs"
done
for f in src/*.c sys/share/posixregex.c sys/share/ioctl.c \
    sys/share/unixtty.c sys/unix/unixmain.c sys/unix/unixunix.c \
    sys/unix/unixres.c win/tty/getline.c win/tty/termcap.c win/tty/topl.c \
    win/tty/wintty.c wasm/wasmtty.c; do
    b=$(basename "$f" .c)
    case $b in tile) continue ;; esac
    extra=
    [ "$b" = date ] && extra="-DNETHACK_GIT_SHA=\\\"$sha\\\" -DNETHACK_GIT_BRANCH=\\\"$branch\\\""
    echo "cd src && emcc \$CFLAGS $extra -c -o ../$obj/$b.o ../$f 2>&1" \
        >> "$obj/jobs"
done
xargs -d '\n' -P "$(nproc)" -I{} sh -c '{}' < "$obj/jobs" \
    | tee "$obj/compile.log"
if grep -n 'warning:\|error:' "$obj/compile.log"; then
    exit 1
fi

# the playground the game sees at /nethack: a browser has one player and
# no other users, so any save the page gives it is the player's
cp "$playground/nhdat" "$playground/symbols" "$playground/license" "$data/"
for f in perm record logfile xlogfile livelog; do : > "$data/$f"; done
cat > "$data/sysconf" <<'EOF'
WIZARDS=
EXPLORERS=*
SHELLERS=
MAXPLAYERS=1
CHECK_SAVE_UID=0
EOF

emcc -O2 $m64 -Wno-deprecated -o "$out/nethack.mjs" "$obj"/*.o \
    -sASYNCIFY -sASYNCIFY_STACK_SIZE=262144 -sSTACK_SIZE=4MB \
    -sALLOW_MEMORY_GROWTH -sINITIAL_MEMORY=64MB -sUSE_ZLIB=1 \
    -sMODULARIZE -sEXPORT_ES6=1 -sEXPORT_NAME=NetHack \
    -sINVOKE_RUN=0 -sEXIT_RUNTIME=1 -sENVIRONMENT=web,node \
    -sFORCE_FILESYSTEM -sEXPORTED_RUNTIME_METHODS=FS,ENV,TTY,callMain \
    --preload-file "$data@/nethack"
echo "$sha" > "$out/BUILD"
ls -l "$out/nethack.mjs" "$out/nethack.wasm" "$out/nethack.data" "$out/BUILD"
