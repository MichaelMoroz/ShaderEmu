# Ports: upstream's network and system programs

`linux/ports/` builds the programs of upstream's ROM that nothing else replaces, in place of its
dynamic ELF programs, which do not run on the machine without an MMU. Each script fetches a
pinned release, checks its sha256 and applies its patch (`fetch.sh`); all are among
`linux/nommu/build.sh`'s (`dropbear.sh` before `sftp.sh`).

- Dropbear 2025.88: server, client, scp and the key tools as one program, `dropbearmulti`, with
  `ssh`, `dbclient`, `scp`, `dropbear`, `dropbearkey` and `dropbearconvert` as links to it.
- OpenSSH 9.9p2's `sftp` and `/usr/libexec/sftp-server`, without OpenSSL (`sftp` runs Dropbear's `ssh`).
- curl 8.16.0 with Mbed TLS 3.6.4.
- socat 1.8.0.3 with filan and procan.
- thttpd 2.29 with makeweb and htpasswd.
- pcre 8.45's `pcregrep` and `pcretest` (no JIT).
- liblockfile 1.17's `dotlockfile` and lockfile-progs 0.1.19.
- at 3.2.5: `at` and `atd`.
- iproute2 6.16.0, in `/sbin`: `ss`, `bridge`, `tc`, `genl`, `ifstat`, `lnstat`, `nstat`, `rtacct`,
  `rtmon` (`ip` is BusyBox's).

They go in at upstream's paths, which hides upstream's.

A fork here is a vfork: the child shares the parent's memory until it execs. Every program that
forked a child and let it go on is changed. The Dropbear server, thttpd and atd stay in the
foreground (start them with `&`). Each Dropbear connection and each at job is the program run
again; thttpd writes a listing itself and runs a CGI through itself started again (`thttpd -H`),
which feeds it the POST data and parses its headers. socat has no fork option, EXEC, SYSTEM or
SHELL, and says so. The kernel's random pool is never ready, so Dropbear and Mbed TLS read
`/dev/urandom` instead of waiting in `getrandom()`. Dropbear's X25519 and Ed25519 are
Monocypher's and its post-quantum key exchanges are left out, for a login's time: `ssh` to the machine itself with a new host key, one command run, takes
6.5 s (calibrated, 31 October 2026).

Not working: `tc`, which needs `CONFIG_NET_SCHED`; `rtacct`, which maps its history file shared
and needs route realms, neither of which the kernel has; https without `-k` or `--cacert`, since
the image has no CA bundle; at's mail of a job's output, since there is no sendmail. `sshd`,
`ssh-keygen`, `ssh-add`, `ssh-agent` and `ssh-keyscan` are not in the ROM (`docs/system/nommu.md`,
"The ROM"). sudo and its tools are left out because the machine has only root, and GLib's gio,
gsettings, gdbus, gresource and gapplication because nothing uses them.

`python tools/test_all.py ports-pcre ports-lockfile ports-socat ports-thttpd-curl ports-dropbear
ports-at ports-iproute2` (the `programs` tag): ssh, scp and sftp to the machine itself, curl
against thttpd with a CGI, and the rest.
