#!/usr/bin/env bash
# Tiberian Dawn's and Red Alert's builds for gpu_vm (run by gpu_vm/userland/apps/build.sh on its copy, $R): the counts
# the RISC-V builds give gcc (tdawn.profile, ralert.profile: gcc's .gcda files) mean nothing to clang, so no PGO.
set -euo pipefail
sed -i 's|^elif \[ -f "\$HERE/tdawn.profile" \]; then$|elif false; then|' "$R/linux/tdawn/build.sh"
sed -i 's|^elif \[ -f "\$HERE/ralert.profile" \]; then$|elif false; then|' "$R/linux/ralert/build.sh"
grep -q '^elif false; then$' "$R/linux/tdawn/build.sh" && grep -q '^elif false; then$' "$R/linux/ralert/build.sh" \
    || { echo "fix/cnc.sh: the profile lines moved"; exit 1; }
# No debug information: Vanilla Conquer's CMake adds -g to Release, and with it the gpu_vm compiler crashes on deleting
# destructors (gpu_vm/userland/apps/bugs/dtor_debug.cpp). -g0 in its place, before the tree is copied for the build.
for f in tdawn ralert; do
    sed -i 's|^rsync -rc --delete --exclude .git "\$VC/" "\$SRC/"$|sed -i "s/ -g\\")/ -g0\\")/" "$VC/cmake/Compilers.cmake"\n&|' "$R/linux/$f/build.sh"
    grep -q 'g0' "$R/linux/$f/build.sh" || { echo "fix/cnc.sh: $f's rsync line moved"; exit 1; }
done
