# Terminal programs

The terminal programs are built by `linux/terminal/build.sh` in place of upstream's, which are
dynamic ELF programs and do not run on the machine without an MMU: nano 8.2, htop 3.3.0, sl
5.02, dialog 1.3-20240619, mg 3.7 (Troglobit's), file 5.41 and Midnight Commander 4.8.27, all
at upstream's paths, each from its release's tarball checked by its sha256.

    wsl -- bash linux/nommu/build.sh terminal/build.sh

They are linked static against one ncurses 6.5 with wide characters, which reads the terminal
descriptions already in the ROM (`/usr/share/terminfo`; the console says `linux`). file is the
ROM's magic database's version, and mc the version of its `/etc/mc` and `/usr/share/mc`. file
also reads the database as deflate in segments, inflated on the workers (`file-mdz.patch`): the
image stores it so (`docs/system/nommu.md`, "The ROM"). mc's glib is 2.70.5, only libglib built
with meson (no gobject or gio), on pcre 8.45; its atomic operations are its locked ones, since
the compiler makes no atomic instructions.

A fork is a vfork here, so a child may only exec or exit, and only in the function that forked.
mc's `my_systemv` is changed so that it forks itself (`mc-vfork.patch`), and copies and moves in
the background are left out (their child goes on copying). mc's subshell needs nothing: its
child sets up the pseudo-terminal and execs the shell, and a command line typed in mc runs in it.

zsh, screen and tmux are not built. zsh runs on in the child of every fork (an external command,
a pipeline, `$(...)`), so the first such command ends or kills the shell; `TERMINAL_ZSH=1` builds
it for trying. screen and tmux start their server as a forked child; both have a foreground
server (`screen -D -m`, tmux 3.2's `-D`), but their panes' spawning (musl's forkpty is a real
fork) would have to become openpty and vfork first.

Two things the cross build is told: ncurses that `poll` works (`cf_cv_working_poll=yes`; without
it a timed key read never returns a key, and htop ignores keys), and autoconf that `select`
exists (`nommu_fork.h`, included in every file, declares it through `<sys/types.h>`, so the
check fails to compile).

`python tools/test_all.py term-nano term-mg term-htop term-dialog term-sl term-file term-mc
term-mcedit` (the `programs` tag) types into each and checks what it wrote or said.
