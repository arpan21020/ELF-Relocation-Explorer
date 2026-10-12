#!/usr/bin/env bash

set -e

ROOT=$(cd "$(dirname "$0")" && pwd)
TOOL="$ROOT/build/elf-explorer"
OUT="$ROOT/build/demo"

step() { printf '\n\033[1m########## %s ##########\033[0m\n\n' "$1"; }

step "1. Build"
mkdir -p "$ROOT/build" "$OUT"
g++ -std=c++17 -Wall -Wextra -Wpedantic -I"$ROOT/include" "$ROOT"/src/*.cpp -o "$TOOL"
echo "built $TOOL"

step "2. Compile examples"
cd "$OUT"
for name in main add missing dup_add weak_add; do
    gcc -c -O0 "$ROOT/examples/$name.c" -o "$name.o"
    echo "examples/$name.c -> build/demo/$name.o"
done

set +e

step "3a. inspect main.o"
"$TOOL" inspect main.o

step "3b. analyze main.o add.o"
"$TOOL" analyze main.o add.o

step "3c. relocate --base 0x401000 main.o add.o"
"$TOOL" relocate --base 0x401000 main.o add.o

step "4a. Error case: missing symbol"
"$TOOL" analyze main.o add.o missing.o | sed -n '/References/,/^$/p'
echo "exit code: ${PIPESTATUS[0]}"

step "4b. Error case: duplicate definition"
"$TOOL" analyze main.o add.o dup_add.o | grep -E 'error'
echo "exit code: ${PIPESTATUS[0]}"

step "4c. Weak definition overridden by strong"
"$TOOL" analyze main.o weak_add.o add.o | grep -E '^  add |note'
echo "exit code: ${PIPESTATUS[0]}"

step "5. Compare with GNU ld"
ld -Ttext=0x401000 -e main -o ref main.o add.o
echo "ld:   $(objdump -d ref | grep 'call.*<add>' | sed 's/^ *//')"
echo "tool: $("$TOOL" relocate main.o add.o | awk '/R_X86_64_PLT32 +add/ {f=1} f && /patch/ {sub(/^ +/, ""); print; exit}')"
