# The design of the linker

This document describes how the linker is put together and why the pieces
look the way they do.

## The modules

| File                | Responsibility                                        |
| ------------------- | ----------------------------------------------------- |
| `src/main.c`        | the driver, it wires the phases together              |
| `src/options.c`     | the command line                                      |
| `src/input.c`       | reading objects, archives and shared objects          |
| `src/symbols.c`     | the global symbol table and the symbol resolution     |
| `src/sections.c`    | merging the input sections into the output sections   |
| `src/reloc.c`       | the relocations                                       |
| `src/layout.c`      | the addresses and the program header table            |
| `src/dynamic.c`     | the dynamic linking structures                        |
| `src/output.c`      | writing the ELF file                                  |
| `src/util.c`        | memory, buffers and string tables                     |
| `src/diag.c`        | the error and warning messages                        |

The data model lives in `include/ld/ld.h`, the ELF definitions (including the
AArch64 relocation numbers and the dynamic tags) in `include/ld/elf.h`.

## The phases

```
    options
       |
       v
    input          read the .o, .a and .so files
       |
       v
    symbols        resolve the global symbols, allocate the commons
       |
       v
    sections       merge the input sections, compute the sizes
       |
       v
    reloc scan     decide about the GOT slots, PLT entries and the
                   dynamic relocations
       |
       v
    dynamic        build .dynsym, .hash, .rela.dyn, .rela.plt, .dynamic
       |
       v
    layout         assign the addresses, build the program headers
       |
       v
    reloc apply    patch the instructions and the data
       |
       v
    dynamic fill   write the GOT, the PLT and the dynamic section
       |
       v
    output         write the file
```

The order matters: everything that influences a size has to happen before the
layout, and everything that needs an address has to happen after it.  That is
why the relocations are processed twice - once to collect the requirements
and once to patch the code.

## The memory layout

The image is built from three loadable segments:

```
  R     .interp .hash .gnu.hash .dynsym .dynstr .rela.* .rodata .eh_frame
  RX    .init .plt .text .fini
  RW    .init_array .fini_array .dynamic .got .got.plt .data .bss
```

The file offset and the virtual address of a section always differ by the
load address of the image, so `(p_vaddr - p_offset) % page_size == 0` holds
for every segment - the constraint the kernel and every dynamic linker
require.

The writable segment starts with the sections that the dynamic linker has to
write to (`.dynamic`, `.got`, `.got.plt`).  They are covered by
`PT_GNU_RELRO`, which makes the dynamic linker protect them after the
relocations have been applied.

## The dynamic linking

The Android dynamic linker binds eagerly, therefore the linker emits
`DT_BIND_NOW` and `DF_BIND_NOW` and the PLT stubs only have to load the
resolved address from their GOT slot:

```
  adrp x16, <page of the GOT slot>
  ldr  x17, [x16, #offset]
  add  x16, x16, #offset
  br   x17
```

The slot belongs to the symbol, so a JUMP_SLOT relocation fills it at load
time.  The same mechanism is used for the data symbols, there the relocation
is a GLOB_DAT.

An absolute relocation inside a shared object or a position independent
executable cannot be resolved at link time, because the load address is only
known at run time.  Such a relocation becomes either

* `R_AARCH64_RELATIVE` - the value is the load address plus the addend, used
  for local symbols, or
* `R_AARCH64_GLOB_DAT` - the dynamic linker looks the symbol up in the
  global scope, used for the symbols that may be interposed.

The relative relocations are written first so that `DT_RELACOUNT` can point
at them.

## The symbol tables

`.dynsym` always starts with the null symbol and the symbols are ordered the
way the GNU hash table expects them: by bucket and, inside a bucket, by the
hash value.  Both the System V hash table (`.hash`) and the GNU hash table
(`.gnu.hash`) are written, the Android dynamic linker understands both.

The static symbol table (`.symtab`) is written unless `--strip-all` is given.

## The relocations

The value of a relocation follows the AArch64 ELF ABI:

```
  value = S + A                 absolute
  value = S + A - P             PC relative
  value = page(S + A) - page(P) page relative (ADRP)
```

The instructions are patched in place:

| Relocation                | Instruction field                    |
| ------------------------- | ------------------------------------ |
| `ADR_PREL_PG_HI21`        | imm21, page relative                |
| `ADR_PREL_LO21`           | imm21, byte relative                |
| `ADD_ABS_LO12_NC`         | imm12, unscaled                     |
| `LDST*_ABS_LO12_NC`       | imm12, scaled by the access size    |
| `CALL26`, `JUMP26`        | imm26, in instructions               |
| `CONDBR19`, `LD_PREL_LO19`| imm19, in instructions               |
| `MOVW_*`, `MOVZ_*`        | imm16                               |
| `ADR_GOT_PAGE`            | imm21, page relative                |
| `LD64_GOT_LO12_NC`        | imm12, scaled                        |

## What is not implemented

* Thread local storage.  The TLS relocations (`TLSLD_*`, `TLSLE_*`,
  `TLSDESC_*`) are not applied, an object that uses them is rejected.
* `--gc-sections`.
* Copy relocations.  Android does not use them, the dynamic linker of
  Android rejects them.
