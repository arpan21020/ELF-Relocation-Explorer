# ELF Relocation Explorer

A small C++17 command-line tool that reads real ELF relocatable object files (`.o`) produced by GCC and explains:

1. what is inside them: sections, symbols, relocations (`inspect`),
2. how symbols in one file are resolved to definitions in another (`analyze`),
3. exactly which bytes a linker writes for each relocation, under an explicit assumed layout, verified against GNU `ld` (`relocate`).

It uses only the C++ standard library and `<elf.h>`; there is no libelf or LLVM.

## Background

**ELF** (Executable and Linkable Format) is the file format Linux uses for object files, executables and shared libraries. An object file is split into **sections**: `.text` (machine code), `.data` (initialized variables), `.rodata` (constants), `.bss` (zero-initialized variables), plus metadata sections.

When the compiler builds `main.o`, it does not know:

- where `add()` will be. It is in another file.
- where `main.o`'s own `.text`, `.data` and `.rodata` will be placed in the final program.

So it emits placeholder bytes (usually zeros), and two kinds of metadata:

- a **symbol table** (`.symtab`): the names this file defines (`main`) and the names it needs (`add`, marked `UND`);
- **relocation entries** (`.rela.text`): "at offset X in `.text`, write the value computed from symbol Y and addend A using formula T".

### Symbol resolution vs relocation

These are two separate linker steps:

| Step | Question | Input | Output |
|---|---|---|---|
| Symbol resolution | *Which definition does the name `add` refer to?* | symbol tables of all files | `add` → `add.o:.text + 0x0` |
| Relocation | *What bytes do I write so the code reaches it?* | resolved symbols + final addresses | `e8 00 00 00 00` → `e8 02 00 00 00` |

Resolution can be done with section-relative information only. Relocation needs **final addresses**. In an `ET_REL` file every section starts at 0, so `st_value` and `r_offset` are offsets inside a section, not addresses. That is why `analyze` describes S and P symbolically and only `relocate`, which assigns addresses, computes numbers.

### The relocation formulas

| Type | Formula | Field |
|---|---|---|
| `R_X86_64_PC32` | `S + A - P` | 4 bytes, signed |
| `R_X86_64_PLT32` | `S + A - P`, no PLT stub because the definition is linked statically | 4 bytes, signed |
| `R_X86_64_64` | `S + A` | 8 bytes |
| `R_X86_64_32` | `S + A` | 4 bytes, unsigned |
| `R_X86_64_32S` | `S + A` | 4 bytes, signed |

- **S**: final address of the symbol (for a section symbol such as `.rodata`, the address of that section)
- **A**: addend stored in the relocation entry
- **P**: final address of the field being patched
- **D**: the value written into the field (little-endian)

**Why is the addend `-4`?** A `call` or RIP-relative operand is relative to the *next* instruction, and the 4-byte field is at the end of the instruction. The CPU computes `target = (P + 4) + D`. We want `target = S`, so `D = S - (P + 4) = S + (-4) - P`. The compiler stores the `-4` as the addend so the linker can use one generic formula.

**Why `.rodata + 2`?** `main.c` has two strings in `.rodata`: `greeting` at offset 0 and `name` at offset 6. Static objects are referenced through the *section* symbol, so the object's offset is folded into the addend: `6 + (-4) = 2`. The same reference compiled with `-fno-pic` uses an absolute `R_X86_64_32S` instead, and the addend is just `+6`.

## Build

CMake (install with `sudo apt install cmake` if needed):

    cmake -S . -B build
    cmake --build build

Or directly with g++:

    mkdir -p build
    g++ -std=c++17 -Wall -Wextra -Wpedantic -Iinclude src/*.cpp -o build/elf-explorer

## Run

    gcc -c -O0 examples/main.c -o main.o
    gcc -c -O0 examples/add.c  -o add.o

    ./build/elf-explorer inspect main.o
    ./build/elf-explorer analyze main.o add.o
    ./build/elf-explorer relocate --base 0x401000 main.o add.o

Exit codes: `0` success, `1` usage error, `2` unreadable/malformed/unsupported input, `3` problems found (unresolved or duplicate symbols, failed relocations).

## Tests

    tests/test_basic.sh            # uses build/elf-explorer
    tests/test_basic.sh path/to/elf-explorer

The script compiles the examples and checks `inspect`, symbol resolution, an unresolved symbol (`missing.c`), a duplicate strong definition (`dup_add.c`), strong-over-weak (`weak_add.c`), truncated and non-ELF input, relocation overflow and undefined symbols during `relocate`. It also **links the same objects with `ld -Ttext=0x401000`** and checks that the address of `add` and the patched `call add` bytes are identical to the tool's output.

## Example output

### Milestone 1: `inspect main.o` (excerpt)

    Symbols in .symtab (10):
       Num  Name                     Bind    Type     Section        Value
         4  base                     LOCAL   OBJECT   .data          0x0
         6  greeting                 LOCAL   OBJECT   .rodata        0x0
         7  name                     LOCAL   OBJECT   .rodata        0x6
         8  main                     GLOBAL  FUNC     .text          0x0
         9  add                      GLOBAL  NOTYPE   UND            0x0

    Relocations in .rela.text (patch .text, symbols from .symtab), 5 entries:
      Offset     Type                     Symbol                   Addend
      0xa        R_X86_64_PC32            .data                    - 0x4
      0x13       R_X86_64_PC32            .rodata                  + 0x2
      0x20       R_X86_64_PC32            .data                    - 0x4
      0x29       R_X86_64_PC32            .rodata                  - 0x4
      0x39       R_X86_64_PLT32           add                      - 0x4

### Milestone 2: `analyze main.o add.o` (excerpt)

    References to symbols defined in another file:
      Referencing file Symbol               Defined in       Section        Value    Status
      main.o           add                  add.o            .text          0x0      resolved

    main.o  .rela.text -> patches .text (5 entries)
      .text+0x39   R_X86_64_PLT32   add - 0x4
          D = S + A - P   (4-byte field, must fit signed 32-bit)
          S = address of add.o:.text + 0x0  (add)
          A = -4
          P = address of main.o:.text + 0x39

### Milestone 3: `relocate main.o add.o` (excerpt)

    == ASSUMED LAYOUT — a real linker may place sections differently ==

      Output   Input file       Input section        Size  Align  Address
      .text    main.o           .text                0x3f    0x1  0x401000
      .text    add.o            .text                0x18    0x1  0x40103f
      .rodata  main.o           .rodata               0xd    0x1  0x401057
      .data    main.o           .data                 0x4    0x4  0x401064

    main.o:.text + 0x13   R_X86_64_PC32   .rodata + 0x2
        location  P = main.o:.text (0x401000) + 0x13 = 0x401013
        bytes     98 48 8d 15 [00 00 00 00] 0f b6
        symbol    S = main.o:.rodata (0x401057) + 0x0 = 0x401057  (.rodata)
        formula   D = S + A - P = 0x401057 + (2) - 0x401013
        result    D = 0x00000046 (70)
        patch     00 00 00 00 -> 46 00 00 00
        check     P + 4 + D = 0x40105d -> main.o:.rodata + 0x6 <name>

    main.o:.text + 0x39   R_X86_64_PLT32   add - 0x4
        location  P = main.o:.text (0x401000) + 0x39 = 0x401039
        bytes     d6 89 c7 e8 [00 00 00 00] 5d c3
        symbol    S = add.o:.text (0x40103f) + 0x0 = 0x40103f  (add)
        formula   D = S + A - P = 0x40103f + (-4) - 0x401039
        result    D = 0x00000002 (2)
        patch     00 00 00 00 -> 02 00 00 00
        check     P + 4 + D = 0x40103f -> add.o:.text + 0x0 <add>

### Worked example: `call add`

The `call` instruction in `main` is at `.text + 0x38`: opcode `e8` followed by a 4-byte displacement at offset `0x39`. The relocation says `R_X86_64_PLT32 add - 4`.

1. Resolution: `add` is `UND` in `main.o` and defined at `add.o:.text + 0x0`.
2. Layout: `main.o:.text` (0x3f bytes) is at `0x401000`, and `add.o:.text` follows at `0x40103f`.
3. `S = 0x40103f`, `A = -4`, `P = 0x401000 + 0x39 = 0x401039`.
4. `D = 0x40103f - 4 - 0x401039 = 2`, written little-endian: `e8 00 00 00 00` → `e8 02 00 00 00`.
5. Check: the next instruction is at `0x40103d`, and `0x40103d + 2 = 0x40103f`, which is `add`.

GNU `ld` produces the same bytes: `401038: e8 02 00 00 00  call 40103f <add>`.

## Supported formats and limitations

- ELF64, little-endian, x86-64, `ET_REL` only, with `SHT_RELA` relocations and a single `.symtab`. Executables, shared objects, ELF32, big-endian and other architectures are rejected with an error message.
- `relocate` uses a simple layout: `.text`, `.rodata`, `.data`, `.bss`, packed one after another from the base address, honoring `sh_addralign`. `ld` puts `.rodata` and `.data` on separate pages, so only `.text`-to-`.text` relocations are expected to be byte-identical to `ld`. The others are checked by confirming they reach the right object (`<name>`, `<base>`).
- Other allocatable sections (`.eh_frame`, notes) are not laid out; relocations into them are listed as skipped.
- Supported relocation types: `PC32`, `PLT32`, `64`, `32`, `32S`. GOT, PLT-stub, TLS and other types are reported as unsupported.
- `COMMON` symbols are not allocated; archives (`.a`) and section groups (COMDAT) are not handled.
- Patching happens in memory only; no output file is written.

## Next milestone

Write a runnable static executable: an ELF header, `PT_LOAD` program headers with R/X and R/W permissions, page-aligned segments, and a tiny `_start` that calls `main` and invokes the `exit` system call, without libc.

## Project layout

    include/elf_parser.h           ELF structures, bounds-checked loader (Milestone 1)
    include/relocation_analyzer.h  symbol resolution, relocation rules (Milestone 2)
    include/layout.h               assumed layout, relocation application (Milestone 3)
    src/main.cpp                   command-line interface, `inspect` output
    src/*.cpp                      implementations
    examples/                      input C files, including test cases
    tests/test_basic.sh            end-to-end tests
