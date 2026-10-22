#!/usr/bin/env bash
# The terminal programs' build wants meson and ninja from a Python venv (pip); where this computer's Python can make
# none (no ensurepip), meson 1.2.3's release (pure Python, run by python3) and ninja 1.11.1's binary stand in its place.
# Run by gpu_vm/userland/apps/build.sh on its copy, $R.
set -euo pipefail
python3 - "$R/linux/terminal/build.sh" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
old = '[ -x "$MV/bin/meson" ] || { python3 -m venv "$MV" && "$MV/bin/pip" install -q meson==1.2.3 ninja==1.11.1.1; }\n'
new = r'''if [ ! -x "$MV/bin/meson" ] && ! python3 -c "import ensurepip" 2>/dev/null; then
    # (gpu_vm/userland/apps/fix/terminal.sh: no venv here)
    mkdir -p "$MV/bin" "$T/meson-dl"
    [ -f "$T/meson-dl/meson-1.2.3/meson.py" ] || curl -sSfL https://github.com/mesonbuild/meson/releases/download/1.2.3/meson-1.2.3.tar.gz | tar -xz -C "$T/meson-dl"
    [ -f "$T/meson-dl/ninja" ] || { curl -sSfL -o "$T/meson-dl/ninja.zip" https://github.com/ninja-build/ninja/releases/download/v1.11.1/ninja-linux.zip && (cd "$T/meson-dl" && python3 -m zipfile -e ninja.zip . && chmod +x ninja); }
    printf '#!/bin/sh\nexec python3 %s/meson-dl/meson-1.2.3/meson.py "$@"\n' "$T" > "$MV/bin/meson"; chmod +x "$MV/bin/meson"
    ln -sf "$T/meson-dl/ninja" "$MV/bin/ninja"
fi
'''
if 'meson-dl' in s: sys.exit(0)
if old not in s: sys.exit('fix/terminal.sh: the venv line moved')
open(p, 'w').write(s.replace(old, new + old))
PY
# nano's gnulib asks whether threads are linked by weak references to pthread's functions, and under -fPIE an
# undefined weak function's address is the program's load address, not 0 (bugs/weak_undef.c): nano called it and
# died of SIGILL. They are linked in (musl has them in libc.a), so they are not undefined.
python3 - "$R/linux/terminal/build.sh" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = 'conf nano --disable-nls --disable-libmagic --enable-utf8 CPPFLAGS="$CURSES_CFLAGS -D_GL_CONFIG_H_INCLUDED=1" LDFLAGS="-L$ST/usr/lib"'
if 'thrd_exit' in s: sys.exit(0)
if old not in s: sys.exit('fix/terminal.sh: nano\'s conf line moved')
u = ','.join('-u,' + n for n in ('pthread_mutex_destroy', 'pthread_mutex_init', 'pthread_mutex_lock', 'pthread_mutex_unlock',
                                  'pthread_mutexattr_gettype', 'thrd_exit'))
open(p, 'w').write(s.replace(old, old[:-1] + ' -Wl,' + u + '"'))
PY
