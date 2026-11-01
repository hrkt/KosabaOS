#!/bin/sh
# Build the riscv64 kernel and boot it in QEMU for 5 seconds.
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
echo "build: CC=$CC"
echo "build: QEMU=$QEMU"

# -march/-mabi are pinned on purpose: toolchain defaults differ
# (riscv64-linux-gnu-gcc defaults to rv64, xPack riscv-none-elf-gcc
# defaults to rv32), and this kernel is rv64i / lp64.
# -mcmodel=medany: the kernel sits at 0x80200000, outside the range
# the default code model can address; medany uses PC-relative
# addressing instead. -ffreestanding: no libc, no hosted assumptions.
"$CC" -march=rv64i -mabi=lp64 -mcmodel=medany -ffreestanding -nostdlib -no-pie -Wl,--build-id=none -T kernel.ld -o kernel.elf start.S kernel.c uart.c
"$OBJCOPY" -O binary kernel.elf kernel.bin
timeout 5 "$QEMU" -machine virt -bios default -kernel kernel.bin -m 128M -nographic || true
