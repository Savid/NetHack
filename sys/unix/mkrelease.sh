#!/bin/sh
# mkrelease.sh NAME PLAYGROUND OUTDIR
#
# Pack an installed playground (the result of "make install" with the Linux
# or macOS hints) as a relocatable release: OUTDIR/NAME.tar.gz holding
#   NAME/nethack         a wrapper that runs the game from wherever the
#                        tarball was unpacked (with -d; the game then finds
#                        sysconf in the playground, see sysconf_file())
#   NAME/playground/     the game, recover, data library, sysconf, and
#                        empty record, log and save files
#   NAME/README, Seeding, license
# and OUTDIR/NAME.tar.gz.sha256.  Run from the top of the source tree.
set -eu

name=${1:?usage: mkrelease.sh NAME PLAYGROUND OUTDIR}
playground=${2:?usage: mkrelease.sh NAME PLAYGROUND OUTDIR}
outdir=${3:?usage: mkrelease.sh NAME PLAYGROUND OUTDIR}

test -x "$playground/nethack" || { echo "no game in $playground" >&2; exit 1; }
test -f "$playground/sysconf" || { echo "no sysconf in $playground" >&2; exit 1; }

stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
top="$stage/$name"
mkdir -p "$top/playground/save" "$outdir"

# the game and its data; nothing written by games played on the build host
for f in "$playground"/*; do
    case "$(basename "$f")" in
    save|*lock*|paniclog|livelog) ;;
    record|logfile|xlogfile|perm) : > "$top/playground/$(basename "$f")" ;;
    *) cp -p "$f" "$top/playground/" ;;
    esac
done
: > "$top/playground/paniclog"
: > "$top/playground/livelog"
strip "$top/playground/nethack" "$top/playground/recover" 2>/dev/null || true
chmod 755 "$top/playground/nethack" "$top/playground/recover"
chmod 644 "$top/playground/sysconf"

cp -p README Seeding dat/license "$top/"

cat > "$top/nethack" <<'EOF'
#!/bin/sh
# run this NetHack from wherever this directory was unpacked (a symlink to
# this script from a directory on PATH works too); -d (first, as the game
# requires) names the playground: the NETHACKDIR environment variable
# would do too, but the game ignores one longer than 128 bytes
self=$0
while [ -L "$self" ]; do
    link=$(readlink "$self")
    case $link in /*) self=$link ;; *) self=$(dirname "$self")/$link ;; esac
done
here=$(cd "$(dirname "$self")" && pwd)
exec "$here/playground/nethack" -d "$here/playground" "$@"
EOF
chmod 755 "$top/nethack"

tar -C "$stage" -czf "$outdir/$name.tar.gz" "$name"
(
    cd "$outdir"
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$name.tar.gz" > "$name.tar.gz.sha256"
    else
        shasum -a 256 "$name.tar.gz" > "$name.tar.gz.sha256"
    fi
)
echo "wrote $outdir/$name.tar.gz"
