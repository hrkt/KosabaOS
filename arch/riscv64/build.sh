#!/bin/sh
# Build the riscv64 kernel and boot it in QEMU for 5 seconds.
#
# Board selection: BOARD=<name> picks boards/<name>/ (board.h,
# kernel.ld, start.S, uart.c). Default is virt (QEMU). Board-specific
# parts live only in boards/; the common code sees board.h alone.
# Example: BOARD=k210 ./build.sh   (build only; flashing is separate)
#
# Toolchain selection (first match wins):
#   1. $CC / $QEMU environment variables, if set
#   2. The project-local tools in ../../../tools (not committed to git,
#      survives machine refreshes): riscv-gcc (xPack) + bundled QEMU
#   3. System tools: riscv64-linux-gnu-gcc + qemu-system-riscv64
set -e
cd "$(dirname "$0")"
TOOLS_DIR=$(CDPATH= cd -- "../../../tools" 2>/dev/null && pwd || true)

if [ -z "$CC" ] && [ -n "$TOOLS_DIR" ] && [ -x "$TOOLS_DIR/riscv-gcc/bin/riscv-none-elf-gcc" ]; then
    CC="$TOOLS_DIR/riscv-gcc/bin/riscv-none-elf-gcc"
    OBJCOPY="$TOOLS_DIR/riscv-gcc/bin/riscv-none-elf-objcopy"
fi
if [ -z "$QEMU" ] && [ -n "$TOOLS_DIR" ] && [ -x "$TOOLS_DIR/qemu/qemu-system-riscv64" ]; then
    QEMU="$TOOLS_DIR/qemu/qemu-system-riscv64"
fi
CC=${CC:-riscv64-linux-gnu-gcc}
OBJCOPY=${OBJCOPY:-riscv64-linux-gnu-objcopy}
QEMU=${QEMU:-qemu-system-riscv64}
BOARD=${BOARD:-virt}
if [ ! -d "boards/$BOARD" ]; then
    echo "build: unknown BOARD=$BOARD (no boards/$BOARD/)" >&2
    exit 1
fi
echo "build: CC=$CC"
echo "build: QEMU=$QEMU"
echo "build: BOARD=$BOARD"

# -march/-mabi are pinned on purpose: toolchain defaults differ
# (riscv64-linux-gnu-gcc defaults to rv64, xPack riscv-none-elf-gcc
# defaults to rv32). Target baseline is RV64G (decided 2026-11-01,
# see doc/roadmap-multitasking.md): G = I+M+A+F+D, so multiply and
# divide are real instructions and libgcc helpers are not needed.
# G also covers the Zicsr extension the CSR instructions belong to.
# The ABI stays lp64 (integer-only calling convention; the kernel
# uses no floating point).
# -mcmodel=medany: the kernel sits at 0x80200000, outside the range
# the default code model can address; medany uses PC-relative
# addressing instead. -ffreestanding: no libc, no hosted assumptions.
# User programs first: their ELFs are embedded into the kernel
# via embed.S and loaded by the kernel's ELF loader (elf.c).
"$CC" -march=rv64gc -mabi=lp64 -mcmodel=medany -ffreestanding -nostdlib -no-pie -Wl,--build-id=none -T user.ld -o user.elf uprog.c
"$CC" -march=rv64gc -mabi=lp64 -mcmodel=medany -ffreestanding -nostdlib -no-pie -Wl,--build-id=none -T user.ld -o nish.elf nish.c
"$CC" -march=rv64gc -mabi=lp64 -mcmodel=medany -ffreestanding -nostdlib -no-pie -Wl,--build-id=none -T user.ld -o chain.elf chain.c
"$CC" -march=rv64gc -mabi=lp64 -mcmodel=medany -ffreestanding -nostdlib -no-pie -Wl,--build-id=none -T user.ld -o eat.elf eat.c
"$CC" -march=rv64gc -mabi=lp64 -mcmodel=medany -ffreestanding -nostdlib -no-pie -Wl,--build-id=none -T user.ld -o uptime.elf uptime.c
"$CC" -march=rv64gc -mabi=lp64 -mcmodel=medany -ffreestanding -nostdlib -no-pie -Wl,--build-id=none -T user.ld -o atest.elf atest.c
# fptest だけは浮動小数点の検証用に lp64d (FP レジスタ渡しの ABI)
"$CC" -march=rv64gc -mabi=lp64d -mcmodel=medany -ffreestanding -nostdlib -no-pie -Wl,--build-id=none -T user.ld -o fptest.elf fptest.c
# slow / crash はジョブ制御 (段3) の検証用 (test.sh シナリオ X)
"$CC" -march=rv64gc -mabi=lp64 -mcmodel=medany -ffreestanding -nostdlib -no-pie -Wl,--build-id=none -T user.ld -o slow.elf slow.c
"$CC" -march=rv64gc -mabi=lp64 -mcmodel=medany -ffreestanding -nostdlib -no-pie -Wl,--build-id=none -T user.ld -o crash.elf crash.c
# copy は SD exec の実機確認用 (ELF を /sd へ運ぶ道具)
"$CC" -march=rv64gc -mabi=lp64 -mcmodel=medany -ffreestanding -nostdlib -no-pie -Wl,--build-id=none -T user.ld -o copy.elf copy.c
# gdemo / gjob は画面表示 (描画 syscall と窓) の検証用
# (test.sh シナリオ Y / Z、doc/gfx-design.md)
"$CC" -march=rv64gc -mabi=lp64 -mcmodel=medany -ffreestanding -nostdlib -no-pie -Wl,--build-id=none -T user.ld -o gdemo.elf gdemo.c
"$CC" -march=rv64gc -mabi=lp64 -mcmodel=medany -ffreestanding -nostdlib -no-pie -Wl,--build-id=none -T user.ld -o gjob.elf gjob.c

# Ikesu 合流テスト (test.sh シナリオ U) の素材は、Ikesu 側の
# ビルド成果物を借りてくる。OS リポジトリにはバイナリを
# 持ち込まない方針なので、ビルド時にここへコピーして
# embed.S / fs.c が -DIKESU_EMBED のときだけ取り込む。
# 素材が無いときは従来と完全に同じビルドになる (写しが
# 残っていると前回の素材が紛れるので、毎回まず消す)。
# 埋め込みは virt 向けだけ (ikesu.elf は K210 のデータ
# 領域には収まらない)。場所は IKESU_DIR で差し替え可能。
IKESU_DIR=${IKESU_DIR:-../../../../ikesu}
IKESU_DEFINE=""
rm -f ikesu.elf player.wasm test.mp3
if [ "$BOARD" = virt ] \
    && [ -f "$IKESU_DIR/build/ikesu.elf" ] \
    && [ -f "$IKESU_DIR/build/player.wasm" ] \
    && [ -f "$IKESU_DIR/testdata/test.mp3" ]; then
    cp "$IKESU_DIR/build/ikesu.elf" ikesu.elf
    cp "$IKESU_DIR/build/player.wasm" player.wasm
    cp "$IKESU_DIR/testdata/test.mp3" test.mp3
    IKESU_DEFINE=-DIKESU_EMBED
    echo "build: ikesu materials embedded (IKESU_DIR=$IKESU_DIR)"
fi

if [ "$BOARD" = virt ]; then
    # FAT ドライバのテスト相手 (boards/virt/ramdisk.c が埋め込む)。
    # SD exec の検証用に user.elf を UPROG.ELF として取り込む
    # (doc/sd-exec-design.md 決め事6)
    python3 make_sd_image.py sd.img UPROG.ELF=user.elf
fi

"$CC" -march=rv64gc -mabi=lp64 -mcmodel=medany -ffreestanding -nostdlib -no-pie -Wl,--build-id=none $IKESU_DEFINE -I. -I"boards/$BOARD" -T "boards/$BOARD/kernel.ld" -o kernel.elf boards/"$BOARD"/*.S kernel.c boards/"$BOARD"/*.c trap.S trap.c vm.c page.c klog.c time.c user.S embed.S proc.c switch.S elf.c fs.c fat.c gfx.c
"$OBJCOPY" -O binary kernel.elf kernel.bin
# Boot check can be skipped with BUILD_NO_RUN=1 (test.sh does this:
# its scenarios cover the boot, and QEMU's raw terminal mode makes
# Ctrl-C stop working while it runs). Non-virt boards have no QEMU
# boot check here; their kernel.bin is flashed to the real board.
if [ -z "${BUILD_NO_RUN:-}" ] && [ "$BOARD" = virt ]; then
    timeout 5 "$QEMU" -machine virt -bios default -kernel kernel.bin -m 128M -nographic || true
fi
