#!/usr/bin/env bash
# Compile GFX11 assembly and extract the .text section as raw binary

set -euo pipefail

if [ $# -ne 1 ]; then
  echo "usage: $0 file.s"
  exit 1
fi

clang -c -x assembler -target amdgcn-amd-amdhsa -mcpu=gfx1100 -o asm.o "$1"
objdump -h asm.o | grep .text | \
  awk '{print "dd if=asm.o of=asmc.bin bs=1 count=$[0x" $3 "] skip=$[0x" $6 "] status=none"}' | bash
rm asm.o
