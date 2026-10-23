#!/usr/bin/env bash
# Doom's build for gpu_vm (run by gpu_vm/userland/apps/build.sh on its copy, $R): R_InitSpriteDefs reads each sprite's
# name as *(int *) of a string literal, which lies at any address; a word gpu_vm loads stays in its word, so most names
# matched no lump (no monsters, the wrong weapon). The copy reads it by bytes.
set -euo pipefail
python3 - "$R/linux/nanox/doom.sh" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
old = "grep -q 'check - namelist < NUMSPRITES' r_things_bounded.c"
new = ("sed -i 's/intname = \\*(int \\*)namelist\\[i\\];/__builtin_memcpy(\\&intname, namelist[i], 4);/' r_things_bounded.c\n"
       "grep -q '__builtin_memcpy(&intname' r_things_bounded.c || { echo \"r_things.c: the sprite name's read was not found\"; exit 1; }\n"
       + old)
if '__builtin_memcpy' in s:
    sys.exit(0)
if s.count(old) != 1:
    sys.exit("fix/doom.sh: doom.sh's r_things.c step moved")
open(p, 'w').write(s.replace(old, new))
PY
