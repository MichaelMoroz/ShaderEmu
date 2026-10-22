#!/bin/bash
# gpu_vm's root file system as the kernel's initramfs, and the kernel with it (gpu_vm/userland/README.md, "The root
# file system"). In WSL:
#   bash gpu_vm/userland/rootfs.sh [busybox] [apps] [kernel]       (default: all three; busybox only when missing)
# busybox: busybox.sh (BB, ~/gpu_vm/busybox). apps: lua, QuickJS, minigzip, coremark, xtime and test/ programs, built with
# gpuvm-linux-cc into $R (~/gpu_vm/rootfs-build). kernel: the cpio list $R/initramfs.list (rootfs/ for /etc, busybox's
# links, the programs) and a kernel with it in $KOUT (~/gpu_vm/linux-build-rootfs: the first run configures and builds
# it whole, later ones relink), copied with its device tree to build/gvm_rootfs. Boot it: systest.sh, or
#   build/gvm_cpu/gvm_cpu --dtb build/gvm_rootfs/gvm.dtb build/gvm_rootfs/vmlinux
# Sources: ~/gpu_vm/third/{lua,zlib} (gpu_vm/llvm/test/apps/run.sh clones them), ~/gpu_vm/coremark-src/coremark
# (gpu_vm/llvm/test/bench/run.sh). Needs build.sh's sysroot.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
R="${R:-$HOME/gpu_vm/rootfs-build}"
BB="${BB:-$HOME/gpu_vm/busybox}"
KOUT="${KOUT:-$HOME/gpu_vm/linux-build-rootfs}"
THIRD="${THIRD:-$HOME/gpu_vm/third}"
COREMARK="${COREMARK:-$HOME/gpu_vm/coremark-src/coremark}"
CC="$HERE/bin/gpuvm-linux-cc"
OPT="${OPT:--O2}"
steps=("$@")
[ ${#steps[@]} -gt 0 ] || steps=(busybox apps kernel)
mkdir -p "$R"

for s in "${steps[@]}"; do
    case "$s" in
    busybox)
        if [ ! -f "$BB/busybox" ] || [ "${FORCE:-0}" = 1 ]; then BB="$BB" bash "$HERE/busybox.sh"; fi ;;
    apps)
        A="$R/apps"; mkdir -p "$A/zlib"
        [ -d "$THIRD/zlib" ] || git clone -q --depth 1 https://github.com/madler/zlib.git "$THIRD/zlib"
        [ -d "$THIRD/lua" ] || git clone -q --depth 1 https://github.com/lua/lua.git "$THIRD/lua"
        [ -d "$COREMARK" ] || git clone -q --depth 1 https://github.com/eembc/coremark.git "$COREMARK"
        # Lua: the interpreter with every library (POSIX: popen, mkstemp; no dlopen)
        L="$THIRD/lua"
        GVM_STACK=262144 "$CC" $OPT -w -DLUA_USE_POSIX -I"$L" -o "$A/lua" \
            $(for f in lapi lcode lctype ldebug ldo ldump lfunc lgc llex lmem lobject lopcodes lparser lstate lstring \
                       ltable ltm lundump lvm lzio lauxlib lbaselib lcorolib ldblib liolib lmathlib loadlib loslib \
                       lstrlib ltablib lutf8lib linit lua; do echo "$L/$f.c"; done) -lm
        # zlib's minigzip (the clone may leave test/ out of its checkout: taken from git)
        Z="$THIRD/zlib"
        git -C "$Z" show HEAD:test/minigzip.c > "$A/zlib/minigzip.c"
        "$CC" $OPT -w -DHAVE_UNISTD_H -I"$Z" -o "$A/minigzip" "$A/zlib/minigzip.c" \
            $(for f in adler32 compress crc32 deflate gzclose gzlib gzread gzwrite infback inffast inflate inftrees \
                       trees uncompr zutil; do echo "$Z/$f.c"; done)
        # CoreMark, the POSIX port: coremark 0x0 0x0 0x66 ITERATIONS
        "$CC" $OPT -w -I"$COREMARK" -I"$COREMARK/posix" -DFLAGS_STR="\"$OPT\"" -DMULTITHREAD=1 -DUSE_CLOCK=0 \
            -o "$A/coremark" "$COREMARK"/core_{list_join,main,matrix,state,util}.c "$COREMARK/posix/core_portme.c"
        # QuickJS (bellard.org's release, unpacked into $QJS): its REPL compiled to bytecode by the host's qjsc
        QV=2025-04-26; QSHA=2f20074c25166ef6f781f381c50d57b502cb85d470d639abccebbef7954c83bf
        QDL="$HOME/gpu_vm/dl/quickjs"; QJS="${QJS:-$HOME/gpu_vm/quickjs}"; mkdir -p "$QDL" "$A/qjs-build"
        [ -f "$QDL/quickjs-$QV.tar.xz" ] || curl -sSL "https://bellard.org/quickjs/quickjs-$QV.tar.xz" -o "$QDL/quickjs-$QV.tar.xz"
        echo "$QSHA  $QDL/quickjs-$QV.tar.xz" | sha256sum -c --quiet
        if [ ! -f "$QJS/VERSION" ]; then mkdir -p "$QJS"; tar -xJf "$QDL/quickjs-$QV.tar.xz" -C "$QJS" --strip-components=1; fi
        make -s -C "$QJS" qjsc > /dev/null 2>&1
        "$QJS/qjsc" -s -c -o "$A/qjs-build/repl.c" -m "$QJS/repl.js"
        GVM_STACK=262144 "$CC" $OPT -w -D_GNU_SOURCE -DCONFIG_VERSION="\"$(cat "$QJS/VERSION")\"" -I"$QJS" -o "$A/qjs" \
            "$QJS"/{qjs,quickjs,dtoa,libregexp,libunicode,cutils,quickjs-libc}.c "$A/qjs-build/repl.c" -lm
        # libc tests of test.sh that need no path of their own
        for p in libc signals threads mathf badptr; do
            out=$p; [ $p = mathf ] && out=math
            extra=; [ $p = mathf ] && extra=-lm; [ $p = threads ] && extra=-pthread
            "$CC" $OPT -w $extra "$HERE/test/$p.c" -o "$A/test-$out"
        done
        "$CC" $OPT -w "$HERE/test/hello.c" -o "$A/hello"
        "$CC" $OPT -w "$HERE/test/xtime.c" -o "$A/xtime"
        ls -l "$A" | grep -v '^d' ;;
    kernel)
        L="$R/initramfs.list"
        {
            echo "dir /dev 0755 0 0"
            echo "nod /dev/console 0600 0 0 c 5 1"
            echo "nod /dev/null 0666 0 0 c 1 3"
            echo "nod /dev/hvc0 0600 0 0 c 229 0"
            for d in /bin /sbin /usr /usr/bin /usr/sbin /usr/lib /usr/lib/gvm-tests /etc /etc/init.d /proc /sys \
                     /root /home /var /var/log /var/run /mnt; do echo "dir $d 0755 0 0"; done
            echo "dir /tmp 01777 0 0"
            echo "file /bin/busybox $BB/busybox 0755 0 0"
            echo "slink /init /bin/busybox 0777 0 0"
            grep -v '^/bin/busybox$' "$BB/busybox.links" | sort -u | while read -r l; do
                echo "slink $l /bin/busybox 0777 0 0"
            done
            (cd "$HERE/rootfs" && find etc -type f | sort) | while read -r f; do
                m=0644; [ -x "$HERE/rootfs/$f" ] || [[ $f == etc/init.d/* ]] && m=0755
                [ $f = etc/shadow ] && m=0600
                echo "file /$f $HERE/rootfs/$f $m 0 0"
            done
            for p in lua minigzip coremark hello xtime qjs; do echo "file /usr/bin/$p $R/apps/$p 0755 0 0"; done
            for p in libc signals threads math badptr; do echo "file /usr/lib/gvm-tests/$p $R/apps/test-$p 0755 0 0"; done
            echo "file /usr/bin/gvm-tests $HERE/rootfs/gvm-tests 0755 0 0"
        } > "$L"
        ksteps="sync initramfs"
        DEFCONFIG="$ROOT/gpu_vm/linux/arch/gpuvm/configs/gpuvm_defconfig"
        [ -f "$KOUT/.config" ] && [ "$KOUT/.config" -nt "$DEFCONFIG" ] || ksteps="sync config initramfs"
        # the initramfs not compressed: gzip's 1.2 MB took the kernel 0.7 s of the machine's time to unpack
        { OUT="$KOUT" INITRAMFS_LIST="$L" bash "$ROOT/gpu_vm/linux/build.sh" $ksteps &&
          "${KDIR:-$HOME/gpu_vm/linux}/scripts/config" --file "$KOUT/.config" -d INITRAMFS_COMPRESSION_GZIP \
              -e INITRAMFS_COMPRESSION_NONE &&
          OUT="$KOUT" INITRAMFS_LIST="$L" bash "$ROOT/gpu_vm/linux/build.sh" image dtbs; } > "$R/kernel.log" 2>&1 \
            || { tail -30 "$R/kernel.log"; exit 1; }
        # a copy where the Windows programs (gvm_gpu, gvm_cpu.exe) find it
        W="$ROOT/build/gvm_rootfs"; mkdir -p "$W"
        cp "$KOUT/vmlinux" "$W/vmlinux"; cp "$KOUT/arch/gpuvm/boot/dts/gvm.dtb" "$W/gvm.dtb"; cp "$BB/busybox" "$W/busybox"
        ls -l "$W/vmlinux"
        echo "rootfs.sh: kernel $W/vmlinux, device tree $W/gvm.dtb (built in $KOUT)" ;;
    *) echo "rootfs.sh: no step $s" >&2; exit 1 ;;
    esac
done
