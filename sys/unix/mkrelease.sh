#!/bin/sh
# mkrelease.sh NAME PLAYGROUND OUTDIR
#
# Pack an installed playground (the result of "make install" with the Linux
# or macOS hints, which build with DLB) as a relocatable release:
# OUTDIR/NAME.tar.gz holding
#   NAME/nethack         a wrapper that runs the game from wherever the
#                        tarball was unpacked (with -d; the game then finds
#                        sysconf in the playground, see sysconf_file())
#   NAME/playground/     the game, recover, the data library, symbols and
#                        license from PLAYGROUND, the default sysconf from
#                        sys/unix (never the installed one, which may set
#                        a race's SEED), and empty record, log and save
#                        files
#   NAME/README, Seeding, license
# and OUTDIR/NAME.tar.gz.sha256.  Only the files named below go in;
# anything else in the playground is reported and left out.  Run from the
# top of the source tree.
set -eu

name=${1:?usage: mkrelease.sh NAME PLAYGROUND OUTDIR}
playground=${2:?usage: mkrelease.sh NAME PLAYGROUND OUTDIR}
outdir=${3:?usage: mkrelease.sh NAME PLAYGROUND OUTDIR}

shipped="nethack recover nhdat symbols license"
varfiles="record logfile xlogfile perm paniclog livelog" # shipped empty

for f in $shipped; do
    test -f "$playground/$f" || { echo "no $f in $playground" >&2; exit 1; }
done
test -x "$playground/nethack" || { echo "no game in $playground" >&2; exit 1; }

stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
top="$stage/$name"
mkdir -p "$top/playground/save" "$outdir"

for f in $shipped; do
    cp -p "$playground/$f" "$top/playground/"
done
cp -p sys/unix/sysconf "$top/playground/"
for f in $varfiles; do
    : > "$top/playground/$f"
done
for f in "$playground"/* "$playground"/.[!.]*; do
    test -e "$f" || continue
    b=$(basename "$f")
    case " $shipped $varfiles sysconf save " in
    *" $b "*) ;;
    *) echo "mkrelease.sh: leaving out $f" >&2 ;;
    esac
done
strip "$top/playground/nethack" "$top/playground/recover" 2>/dev/null || true
chmod 755 "$top/playground/nethack" "$top/playground/recover"
chmod 644 "$top/playground/sysconf"

cp -p README Seeding dat/license "$top/"

cat > "$top/nethack" <<'WRAP'
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
WRAP
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
