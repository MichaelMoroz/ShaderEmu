#!/usr/bin/env bash
# Builds the project's Linux kernel (pimaker's linux-rvc 5.17.11 plus our drivers and hooks) in
# WSL or any Linux, without root, into build/images/linux/Image. See docs/gpu.md.
#   wsl -- bash /mnt/c/.../linux/kernel/build.sh
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
WORK=${WORK:-$HOME/shaderemu-linux}
TOOLCHAIN=https://toolchains.bootlin.com/downloads/releases/toolchains/riscv32-ilp32d/tarballs/riscv32-ilp32d--glibc--stable-2024.05-1.tar.xz
mkdir -p "$WORK" && cd "$WORK"

if [ ! -d toolchain ]; then
    echo "== cross compiler"
    curl -sSL "$TOOLCHAIN" -o toolchain.tar.xz
    mkdir toolchain && tar -xf toolchain.tar.xz -C toolchain --strip-components=1 && rm toolchain.tar.xz
fi
if [ ! -x tools/usr/bin/flex ]; then
    echo "== flex, bison, m4 (unpacked from Ubuntu's packages, not installed)"
    mkdir -p debs tools
    (cd debs && apt-get download flex bison m4 >/dev/null && for d in *.deb; do dpkg-deb -x "$d" ../tools; done)
fi
export PATH="$WORK/tools/usr/bin:$WORK/toolchain/bin:$PATH"
export BISON_PKGDATADIR="$WORK/tools/usr/share/bison" M4="$WORK/tools/usr/bin/m4"

if [ ! -d linux ]; then
    echo "== kernel source"
    git clone -q --depth 1 -b rvcnet-5.17.11 https://github.com/pimaker/linux-rvc linux
fi
[ -f linux.config ] || curl -sSL https://raw.githubusercontent.com/pimaker/rvc/master/linux.config -o linux.config

# our drivers and hooks, on top of the fork
python3 "$HERE/install_drivers.py" linux
python3 "$HERE/memset_hook.py" linux
python3 "$HERE/memcpy_hook.py" linux
cat linux.config "$HERE/config.extra" > all.config

echo "== configure and build"
MAKE="make -C linux ARCH=riscv CROSS_COMPILE=riscv32-linux- -j$(nproc)"
$MAKE KCONFIG_ALLCONFIG="$WORK/all.config" allnoconfig >/dev/null
for symbol in SHADEREMU_GPU FB_SHADEREMU INPUT_SHADEREMU INPUT_EVDEV HZ_100; do
    grep -q "^CONFIG_$symbol=y" linux/.config || { echo "CONFIG_$symbol is not set in .config"; exit 1; }
done
$MAKE Image 2>&1 | grep -E "error|warning: .*shaderemu|Image is ready" || true
mkdir -p "$REPO/build/images/linux"
cp linux/arch/riscv/boot/Image "$REPO/build/images/linux/Image"
ls -l "$REPO/build/images/linux/Image"
