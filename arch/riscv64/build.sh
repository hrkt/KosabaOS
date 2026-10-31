#!/bin/sh
set -e
cd "$(dirname "$0")"
riscv64-linux-gnu-gcc -nostdlib -no-pie -Wl,--build-id=none -T kernel.ld -o kernel.elf start.S
riscv64-linux-gnu-objcopy -O binary kernel.elf kernel.bin
timeout 5 qemu-system-riscv64 -machine virt -bios default -kernel kernel.bin -m 128M -nographic || true
