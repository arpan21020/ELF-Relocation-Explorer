#!/usr/bin/env bash

set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
TOOL=$(realpath "${1:-$ROOT/build/elf-explorer}")
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
cd "$WORK" || exit 1

failures=0
pass() { echo "PASS: $1"; }
fail() { echo "FAIL: $1"; failures=$((failures + 1)); }

run() {
    local name=$1 expected=$2
    shift 2
    OUT=$("$TOOL" "$@" 2>&1)
    local status=$?
    if [ "$status" -eq "$expected" ]; then
        pass "$name (exit $status)"
    else
        fail "$name: expected exit $expected, got $status"
        echo "$OUT" | sed 's/^/    /'
    fi
}

contains() {
    if echo "$OUT" | grep -Eq -- "$2"; then
        pass "$1"
    else
        fail "$1: output does not match /$2/"
    fi
}

[ -x "$TOOL" ] || { echo "tool not found: $TOOL (build it first)"; exit 1; }

for name in main add missing dup_add weak_add; do
    gcc -c -O0 "$ROOT/examples/$name.c" -o "$name.o" || { echo "cannot compile $name.c"; exit 1; }
done

run "inspect main.o" 0 inspect main.o
contains "inspect lists .rela.text" 'Relocations in \.rela\.text'
contains "inspect shows add as undefined" '^ +[0-9]+ +add +GLOBAL +NOTYPE +UND'
contains "inspect shows the .rodata + 2 relocation" 'R_X86_64_PC32 +\.rodata +\+ 0x2'

run "analyze main.o add.o" 0 analyze main.o add.o
contains "add resolved from add.o" '^  main\.o +add +add\.o +\.text +0x0 +resolved'
contains "relocation formula shown" 'D = S \+ A - P'

run "analyze with missing symbol" 3 analyze main.o add.o missing.o
contains "subtract reported as unresolved" 'missing\.o +subtract .*UNRESOLVED'

run "analyze with duplicate definition" 3 analyze main.o add.o dup_add.o
contains "duplicate add reported" "duplicate definition of 'add': add\.o and dup_add\.o"

run "analyze with weak definition" 0 analyze main.o weak_add.o add.o
contains "strong add from add.o wins" '^  add +GLOBAL +add\.o'
contains "weak override noted" "strong 'add' in add\.o overrides weak definition in weak_add\.o"

head -c 40 main.o > truncated.o
run "truncated file rejected" 2 inspect truncated.o
contains "truncation reported" 'truncated'
echo "not an elf file" > text.o
run "non-ELF file rejected" 2 inspect text.o
contains "bad magic reported" 'bad magic'

run "relocate main.o add.o" 0 relocate --base 0x401000 main.o add.o
contains "layout is labeled as assumed" 'ASSUMED LAYOUT'
contains "data relocation reaches base" 'check .* <base>$'
contains "rodata + 2 relocation reaches name" 'check .* <name>$'
contains "rodata - 4 relocation reaches greeting" 'check .* <greeting>$'
contains "eh_frame relocations skipped" 'skipped +target section \.eh_frame'
RELOCATE_OUT=$OUT

if ld -Ttext=0x401000 -e main -o ref main.o add.o; then
    ld_add=$(printf '0x%x' "0x$(nm ref | awk '$3 == "add" {print $1}')")
    tool_add=$(echo "$RELOCATE_OUT" | awk '/R_X86_64_PLT32 +add/ {f=1} f && /symbol/ {print $(NF-1); exit}')
    [ "$ld_add" = "$tool_add" ] && pass "add address matches ld ($ld_add)" \
        || fail "add address: ld $ld_add, tool $tool_add"

    ld_call=$(objdump -d ref | awk -F'\t' '/call.*<add>/ {print $2; exit}' | awk '{print $2, $3, $4, $5}')
    tool_call=$(echo "$RELOCATE_OUT" | awk '/R_X86_64_PLT32 +add/ {f=1} f && /patch/ {sub(/.*-> /, ""); print; exit}')
    [ -n "$ld_call" ] && [ "$ld_call" = "$tool_call" ] && pass "call add bytes match ld ($ld_call)" \
        || fail "call add bytes: ld '$ld_call', tool '$tool_call'"
else
    fail "ld could not link the examples"
fi

gcc -c -O0 -fno-pic "$ROOT/examples/main.c" -o main_nopic.o
run "relocate detects overflow" 3 relocate --base 0x80000000 main_nopic.o add.o
contains "overflow reported" 'overflow: D does not fit in a signed 32-bit field'

run "relocate with undefined symbol" 3 relocate main.o add.o missing.o
contains "undefined subtract reported" "cannot compute: undefined symbol 'subtract'"

echo
if [ "$failures" -eq 0 ]; then
    echo "All tests passed."
else
    echo "$failures test(s) failed."
fi
[ "$failures" -eq 0 ]
