#!/usr/bin/env bash
# Every build script of the repository copy (setup.sh) with the flat drivers, in order; a line each.
#   bash linux/nommu/build.sh [script ...]     (paths under linux/, default all)
set -uo pipefail
NM=$HOME/nm
mkdir -p "$NM"; exec 9>"$NM/build.lock"; flock 9   # one build of the shared copy at a time (sessions and agents share ~/nm)
R=$NM/repo
export WORK=$NM/work
L=$NM/logs; mkdir -p "$L"
# the sources as they are here (the built root stays: setup.sh made it), the toolchain's drivers renamed as setup.sh does
REPO=$(cd "$(dirname "$0")/../.." && pwd)
rsync -a --delete --exclude /build --exclude /.git --exclude /experiments --exclude /logs --exclude /unity --exclude /rvc --exclude /.claude "$REPO/" "$R/"
sed -i 's|cat > "\$WORK/bin/rv32-cc" <<EOF|cat > "$WORK/bin/rv32-cc.mmu" <<EOF|; s|chmod +x "\$WORK/bin/rv32-cc"$|chmod +x "$WORK/bin/rv32-cc.mmu"|' "$R/linux/userland/toolchain.sh"
sed -i 's|cat > "\$WORK/bin/rv32-c++" <<EOF|cat > "$WORK/bin/rv32-c++.mmu" <<EOF|; s|chmod +x "\$WORK/bin/rv32-c++"$|chmod +x "$WORK/bin/rv32-c++.mmu"|' "$R/linux/tdawn/build.sh"
# the copy's changes for no MMU: Quake III's pools are all memory there, so they are what q3dm17 takes
# (meminfo: the hunk 5.3 MB at most, the zone 42 KB, the server's 3.2 MB with seven bots)
sed -i -e 's/^+#define DEF_COMHUNKMEGS \t32$/+#define DEF_COMHUNKMEGS \t8/' -e 's/^+#define MIN_COMHUNKMEGS\t\t16$/+#define MIN_COMHUNKMEGS\t\t8/' \
    -e 's/^+#define DEF_COMZONEMEGS\t\t16$/+#define DEF_COMZONEMEGS\t\t4/' \
    -e 's/^+#define SERVER_ZONE_BYTES (8 \* 1024 \* 1024)$/+#define SERVER_ZONE_BYTES (6 * 1024 * 1024)/' "$R/linux/quake3/quake3.patch"
# and over a q3config.cfg in /persistence that the MMU world wrote (32 and 16 MB, one run each here)
sed -i 's/^exec quake3.bin +set fs_basepath/exec quake3.bin +set com_hunkMegs 8 +set com_zoneMegs 4 +set fs_basepath/' "$R/linux/quake3/build.sh"
# busybox for a machine without an MMU
grep -q 'set_option NOMMU y' "$R/linux/userland/busybox.sh" || sed -i 's/^set_option STATIC y$/set_option STATIC y\nset_option NOMMU y/' "$R/linux/userland/busybox.sh"
ALL="userland/build.sh userland/busybox.sh nanox/build.sh libmedia/build.sh apps/build.sh play/build.sh video/build.sh
     raytrace/build.sh imgui/build.sh pathtrace/build.sh physics/build.sh quickjs/build.sh micropython/build.sh ide/build.sh
     quake/build.sh quake3/build.sh tdawn/build.sh ralert/build.sh classicube/build.sh nes/build.sh micropolis/build.sh openttd/build.sh
     nanox/doom.sh halflife/build.sh terminal/build.sh
     ports/dropbear.sh ports/sftp.sh ports/curl.sh ports/socat.sh ports/thttpd.sh ports/pcre.sh ports/lockfile.sh ports/iproute2.sh
     ports/at.sh tcc/build.sh"
for s in ${*:-$ALL}; do
    t=$(date +%s)
    log=$L/$(echo "$s" | tr / -).log
    ( cd "$R" && PATH="$WORK/bin:$PATH" bash "$R/linux/$s" ) > "$log" 2>&1
    rc=$?
    echo "$s: exit $rc, $(( $(date +%s) - t )) s$( [ $rc = 0 ] || { echo; grep -E 'rror|undefined|relocation|cannot' "$log" | head -4 | sed 's/^/    /'; } )"
done
