#!/bin/sh
# Architecture-neutral entry point: ./build.sh [arch]
# Default (and currently only implemented) arch is riscv64.
set -e
cd "$(dirname "$0")"
ARCH=${1:-riscv64}
exec "arch/$ARCH/build.sh"
