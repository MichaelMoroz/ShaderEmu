#!/usr/bin/env bash
# ClassiCube's sounds for gpu_vm: the pack is data, the same on both machines. Where this computer's Python has no numpy
# (sound/make_sound.py needs it), the RISC-V image's pack and tune are taken (linux/prebuilt/root/usr/share).
# Run by gpu_vm/userland/apps/build.sh on its copy $R.
set -euo pipefail
PRE=$(cd "$(dirname "$0")/../../../.." && pwd)/linux/prebuilt/root/usr/share
f=$R/linux/classicube/build.sh
grep -q 'python3 "$HERE/sound/make_sound.py"' "$f" || { echo "fix/classicube.sh: make_sound.py's line not found" >&2; exit 1; }
sed -i "s|^    python3 \"\$HERE/sound/make_sound.py\" \(.*\)$|    if python3 -c 'import numpy' 2>/dev/null; then python3 \"\$HERE/sound/make_sound.py\" \1; else cp \"$PRE/classicube-sound.pak\" \"$PRE/classicube-music.mp3\" \"\$OUT/usr/share/\"; fi|" "$f"
grep -q "import numpy" "$f" || { echo "fix/classicube.sh: not changed" >&2; exit 1; }
