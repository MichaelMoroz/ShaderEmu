#!/usr/bin/env bash
# Tiberian Dawn's and Red Alert's builds for gpu_vm (run by gpu_vm/userland/apps/build.sh on its copy, $R): the counts
# the RISC-V builds give gcc (tdawn.profile, ralert.profile: gcc's .gcda files) mean nothing to clang, so no PGO.
set -euo pipefail
sed -i 's|^elif \[ -f "\$HERE/tdawn.profile" \]; then$|elif false; then|' "$R/linux/tdawn/build.sh"
sed -i 's|^elif \[ -f "\$HERE/ralert.profile" \]; then$|elif false; then|' "$R/linux/ralert/build.sh"
grep -q '^elif false; then$' "$R/linux/tdawn/build.sh" && grep -q '^elif false; then$' "$R/linux/ralert/build.sh" \
    || { echo "fix/cnc.sh: the profile lines moved"; exit 1; }
