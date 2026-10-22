#!/bin/bash
# busybox for gpu_vm's Linux (gpu_vm/userland/README.md, "The root file system"). In WSL:
#   bash gpu_vm/userland/busybox.sh            -> $BB/busybox (a bFLT program) and $BB/busybox.links
# Downloads the release into ~/gpu_vm/dl/busybox (checked by its sha256), unpacks it into a clean $BB
# (~/gpu_vm/busybox), applies busybox/*.patch, configures it from defconfig for a machine without an MMU and builds it with gpuvm-linux-cc.
# Needs build.sh's sysroot. BB_STACK: the program's stack (bytes, default 131072: a hush function recursing calls
# itself about 300 deep in it, 170 in 64 KB; see the README).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VER=1.37.0
SHA=3311dff32e746499f4df0d5df04d7eb396382d7e108bb9250e7b519b837043a4
DL="$HOME/gpu_vm/dl/busybox"
BB="${BB:-$HOME/gpu_vm/busybox}"
export PATH="$HERE/bin:$HOME/gpu_vm/llvm-build/bin:$PATH"

mkdir -p "$DL"
TAR="$DL/busybox-$VER.tar.bz2"
[ -f "$TAR" ] || curl -sSL "https://busybox.net/downloads/busybox-$VER.tar.bz2" -o "$TAR"
echo "$SHA  $TAR" | sha256sum -c --quiet
rm -rf "$BB"; mkdir -p "$BB"
tar -xjf "$TAR" -C "$BB" --strip-components=1
cd "$BB"
for p in "$HERE"/busybox/*.patch; do patch -s -p1 < "$p"; done

make -s defconfig > /dev/null
on()  { sed -i "s/^# CONFIG_$1 is not set\$/CONFIG_$1=y/" .config; grep -q "^CONFIG_$1=" .config || echo "CONFIG_$1=y" >> .config; }
off() { sed -i "s/^CONFIG_$1=.*\$/# CONFIG_$1 is not set/" .config; }
str() { sed -i "/^CONFIG_$1=/d; /^# CONFIG_$1 is not set/d" .config; echo "CONFIG_$1=\"$2\"" >> .config; }

# no MMU: no fork (vfork, and hush re-executes itself for a subshell), so hush is the shell (ash needs fork)
on NOMMU; on STATIC
str CROSS_COMPILER_PREFIX gpuvm-linux-
str BUSYBOX_EXEC_PATH /bin/busybox
# clang hoists a load of the "const" pointer to the globals over its one store (hush_main read G as NULL)
str EXTRA_CFLAGS -DBB_GLOBAL_CONST=
off SH_IS_ASH; on SH_IS_HUSH; off BASH_IS_ASH; off BASH_IS_HUSH; on BASH_IS_NONE
for o in HUSH HUSH_BASH_COMPAT HUSH_BRACE_EXPANSION HUSH_INTERACTIVE HUSH_SAVEHISTORY HUSH_JOB HUSH_TICK HUSH_IF \
         HUSH_LOOPS HUSH_CASE HUSH_FUNCTIONS HUSH_LOCAL HUSH_RANDOM_SUPPORT HUSH_MODE_X HUSH_ECHO HUSH_PRINTF \
         HUSH_TEST HUSH_HELP HUSH_EXPORT HUSH_EXPORT_N HUSH_READONLY HUSH_KILL HUSH_WAIT HUSH_COMMAND HUSH_TRAP \
         HUSH_TYPE HUSH_TIMES HUSH_READ HUSH_SET HUSH_UNSET HUSH_ULIMIT HUSH_UMASK HUSH_GETOPTS HUSH_MEMLEAK \
         SHELL_HUSH FEATURE_SH_MATH FEATURE_SH_MATH_64 FEATURE_SH_EXTRA_QUIET FEATURE_SH_STANDALONE \
         FEATURE_PREFER_APPLETS FEATURE_EDITING FEATURE_TAB_COMPLETION FEATURE_EDITING_SAVEHISTORY \
         FEATURE_EDITING_FANCY_PROMPT FEATURE_EDITING_VI; do on $o; done
# what this kernel or machine has not got: networking, modules, a VT, mail and printers, SELinux, PAM, utmp
for d in networking modutils mailutils printutils console-tools selinux runit; do
    for o in $(grep -rhoE '^//config:config [A-Z0-9_]+|^config [A-Z0-9_]+' "$d" | awk '{print $2}'); do off $o; done
done
for o in FEATURE_UTMP FEATURE_WTMP PAM SELINUX HWCLOCK FEATURE_HAVE_RPC FEATURE_INETD_RPC UBIATTACH UBIDETACH \
         UBIMKVOL UBIRMVOL UBIRSVOL UBIUPDATEVOL UBIRENAME NANDWRITE NANDDUMP SHA1_HWACCEL SHA256_HWACCEL FEATURE_MOUNT_NFS FEATURE_MOUNT_CIFS \
         FEATURE_SYSLOG_INFO SYSLOGD KLOGD LOGGER LOGREAD FEATURE_IPC_SYSLOG FEATURE_DEVFS FEATURE_MOUNT_NFS; do
    off $o
done
# a few that the networking folder holds but that need no network
for o in HOSTNAME DNSDOMAINNAME; do on $o; done
{ yes "" || true; } | make -s oldconfig > /dev/null 2>&1 || true

# objects are joined by "ld.lld -r"; the link is the driver's (ld.lld -q, then elf2flt)
GVM_STACK="${BB_STACK:-131072}" make -s -j"$(nproc)" CROSS_COMPILE=gpuvm-linux- LD=ld.lld HOSTCC=gcc \
    SKIP_STRIP=y busybox 2>&1 | grep -vE "^  (CC|LD|AR|GEN|HOSTCC|SPLIT|HOSTLD|AS)\b" | grep -E "error|undefined|warning: (implicit|incompatible)" || true
[ -f busybox_unstripped ] || { echo "busybox.sh: busybox was not built"; exit 1; }
cp busybox_unstripped busybox
make -s busybox.links > /dev/null 2>&1 || true
ls -l busybox
echo "busybox.sh: $(wc -l < busybox.links) applets in $BB/busybox.links"
