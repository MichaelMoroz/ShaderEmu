#!/usr/bin/env bash
# The machine's programs for gpu_vm (gpu_vm/userland/apps/README.md): the RISC-V machine's own build scripts
# (linux/*/build.sh), run unchanged from a copy of the repository whose linux/userland/toolchain.sh is gpu_vm's
# (toolchain.sh beside this file: rv32-cc and the rest over gpuvm-linux-cc). The sources are the repository's: what
# differs for gpu_vm is in them, behind __gpuvm__. What a script must do otherwise for gpu_vm is a sed on the copy here.
#   bash gpu_vm/userland/apps/build.sh [script ...]     (paths under linux/, default all; a line each)
# -> build/gvm_apps/root (what the scripts put in build/images/linux/root), logs in ~/gpu_vm/apps-logs.
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
G=$HOME/gpu_vm
# (GVM_APPS_REPO, GVM_APPS_WORK, GVM_APPS_LOGS: another copy, work folder and logs, for builds side by side)
R=${GVM_APPS_REPO:-$G/apps-repo}
export GVM_APPS_WORK=${GVM_APPS_WORK:-$G/apps-work}
L=${GVM_APPS_LOGS:-$G/apps-logs}; mkdir -p "$L" "$R"
exec 9>"$R.lock"; flock 9          # one build of a copy at a time
rsync -a --delete --exclude /build --exclude /.git --exclude /experiments --exclude /logs --exclude /unity --exclude /rvc \
    --exclude /linux/prebuilt --exclude /bin --exclude /tools/fxc2 "$REPO/" "$R/"
mkdir -p "$R/build/images/linux/root"
# the toolchain: gpu_vm's
printf '#!/usr/bin/env bash\nsource "%s/gpu_vm/userland/apps/toolchain.sh"\n' "$R" > "$R/linux/userland/toolchain.sh"
# Tiberian Dawn's build writes the RISC-V machine's C++ driver over ours: not here
sed -i '/^cat > "\$WORK\/bin\/rv32-c++" <<EOF$/,/^chmod +x "\$WORK\/bin\/rv32-c++"$/d; s/^CXXINC=.*$//' "$R/linux/tdawn/build.sh"
# the copy's changes for no MMU (linux/nommu/build.sh): Quake III's pools are all memory there
sed -i -e 's/^+#define DEF_COMHUNKMEGS \t32$/+#define DEF_COMHUNKMEGS \t8/' -e 's/^+#define MIN_COMHUNKMEGS\t\t16$/+#define MIN_COMHUNKMEGS\t\t8/' \
    -e 's/^+#define DEF_COMZONEMEGS\t\t16$/+#define DEF_COMZONEMEGS\t\t4/' \
    -e 's/^+#define SERVER_ZONE_BYTES (8 \* 1024 \* 1024)$/+#define SERVER_ZONE_BYTES (6 * 1024 * 1024)/' "$R/linux/quake3/quake3.patch"
sed -i 's/^exec quake3.bin +set fs_basepath/exec quake3.bin +set com_hunkMegs 8 +set com_zoneMegs 4 +set fs_basepath/' "$R/linux/quake3/build.sh"
# what the RISC-V builds downloaded once (games' demos, meshes), taken from their folders when this one has none
mkdir -p "$GVM_APPS_WORK/src"
for d in pathtrace-data doom1.wad doom-shareware.zip quake-data tdawn-demo tdawn-demo.zip ralert-demo ralert-demo.zip          ra-demo ra95demo.zip quake3-data halflife-data nes-roms openttd-data micropolis-data classicube-data; do
    [ -e "$GVM_APPS_WORK/src/$d" ] && continue
    for from in "$HOME/nm/work/src" "$HOME/shaderemu-linux/src"; do
        [ -e "$from/$d" ] && { cp -a "$from/$d" "$GVM_APPS_WORK/src/"; break; }
    done
done
for f in "$HERE"/fix/*.sh; do [ -f "$f" ] && R="$R" bash "$f"; done
# (not here: openttd/build.sh, whose C++ needs exceptions and the whole of libstdc++, which gpu_vm has not; tcc/build.sh,
# a compiler of RISC-V code; README.md, "Not ported")
ALL="userland/build.sh nanox/build.sh libmedia/build.sh apps/build.sh play/build.sh video/build.sh
     raytrace/build.sh imgui/build.sh pathtrace/build.sh physics/build.sh quickjs/build.sh micropython/build.sh ide/build.sh
     nanox/doom.sh quake/build.sh quake3/build.sh tdawn/build.sh ralert/build.sh classicube/build.sh nes/build.sh
     micropolis/build.sh halflife/build.sh terminal/build.sh ../gpu_vm/userland/apps/mc.sh ../gpu_vm/userland/apps/hlsl.sh"
for s in ${*:-$ALL}; do
    t=$(date +%s)
    log=$L/$(echo "$s" | tr / -).log
    # the games whose math is double take libmfloat's (their double sin, pow... at float precision: gpu_vm/userland/README.md)
    case $s in quake/*|halflife/*) export GVM_LDLIBS=-lmfloat ;; *) export GVM_LDLIBS= ;; esac
    ( cd "$R" && bash "$R/linux/$s" ) > "$log" 2>&1
    rc=$?
    echo "$s: exit $rc, $(( $(date +%s) - t )) s$( [ $rc = 0 ] || { echo; grep -E 'rror|undefined|cannot|No such' "$log" | head -6 | sed 's/^/    /'; } )"
done
# (added to what is there: builds in other copies put theirs there too)
mkdir -p "$REPO/build/gvm_apps/root"
rsync -a "$R/build/images/linux/root/" "$REPO/build/gvm_apps/root/"
