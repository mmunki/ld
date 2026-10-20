# ld

A linker for **AArch64 Android** (`aarch64-linux-android`).  It reads ELF64
relocatable objects, static archives and shared objects and writes ELF64
AArch64 executables and shared libraries.

The linker is a host tool: it runs on the machine that runs the build (a PC, a
Mac, a Linux box, a phone) and always produces Android ready binaries.  It is
written in portable C99 and has no dependencies beyond a C compiler.

```
$ ld -o hello hello.o
$ ld -shared -soname libhello.so -o libhello.so hello.o
```

## What it does

| Input                | Output                                                     |
| -------------------- | ---------------------------------------------------------- |
| `.o` (ET_REL)        | `.so` (ET_DYN, shared library)                             |
| `.o` + `.a` + `.so`  | ELF executable (ET_EXEC, static)                          |
| `.o` + `.a` + `.so`  | position independent executable (ET_DYN, PIE)              |

* **Relocations** - the complete AArch64 relocation set that the Android
  toolchain produces: `ADR_PREL_PG_HI21`, `ADD_ABS_LO12_NC`, `CALL26`,
  `JUMP26`, `CONDBR19`, `LDST*_ABS_LO12_NC`, `ADR_GOT_PAGE`,
  `LD64_GOT_LO12_NC`, `MOVW_*`, `MOVZ_*`, `ABS*`, `PREL*` and the dynamic
  relocations `RELATIVE`, `GLOB_DAT` and `JUMP_SLOT`.
* **Dynamic linking** - `.plt`, `.got`, `.got.plt`, `.dynsym`, `.dynstr`,
  `.hash`, `.gnu.hash`, `.rela.dyn`, `.rela.plt` and `.dynamic` are generated
  the way the Android dynamic linker expects them, including `DT_BIND_NOW`
  (eager binding), `DT_RELACOUNT`, `DT_NEEDED`, `DT_SONAME` and `DT_RUNPATH`.
* **RELRO** - the writable part of the image is protected with
  `PT_GNU_RELRO`, the same way the Android linker does it.
* **Android conventions** - the interpreter of a PIE is
  `/system/bin/linker64`, shared objects are position independent and the
  `DT_SONAME` is honoured.

## Building

```
make                 # builds bin/ld
make test            # builds and runs the test suite
sudo make install    # installs /usr/local/bin/ld
```

or

```
./build.sh
```

The build has no dependencies.  `make test` needs an AArch64 cross compiler
(`gcc-aarch64-linux-gnu`) and `qemu-user`; the tests are skipped when they
are not installed.

## Usage

```
ld [options] file...
```

| Option                    | Meaning                                          |
| ------------------------- | ------------------------------------------------ |
| `-o FILE`                 | the output file                                  |
| `-shared`                 | build a shared library                           |
| `-pie` / `-no-pie`        | position independent (or not) executable          |
| `-static`                 | do not link against shared objects               |
| `-e SYM`, `--entry=SYM`   | the entry point, `_start` by default              |
| `-soname NAME`            | the `DT_SONAME` of a shared library              |
| `-dynamic-linker PATH`    | the program interpreter                          |
| `-L DIR`, `-l NAME`       | the library search path and the libraries        |
| `-rpath PATH`             | the runtime library search path                  |
| `--sysroot DIR`           | the prefix of the library search path            |
| `-Map FILE`               | write a link map                                 |
| `-s`, `--strip-all`       | do not write a symbol table                      |
| `-z now`, `-z relro`      | the linker behaviour switches                    |
| `--no-undefined`          | report undefined symbols as an error             |
| `--image-base ADDR`       | the base address of the image                    |
| `--page-size N`           | the page size used for the segment alignment     |

`ld --help` prints the complete list.

## How it works

The linker follows the classic four phase model:

```
input files  ->  symbols  ->  sections  ->  layout  ->  output
```

1. **Input** (`src/input.c`) reads the object files, the archives (members
   are pulled in on demand) and the shared objects.
2. **Symbols** (`src/symbols.c`) resolve the global symbols: a definition
   beats a common symbol, a strong definition beats a weak one and the first
   definition wins.  Common symbols are allocated in `.bss`.
3. **Sections** (`src/sections.c`) merge the input sections into the output
   sections and compute their sizes.
4. **Relocations** (`src/reloc.c`) decide which GOT slots, PLT entries and
   dynamic relocations are needed and, after the layout, patch the
   instructions and the data.
5. **Layout** (`src/layout.c`) assigns the addresses and builds the program
   header table.
6. **Output** (`src/output.c`) writes the ELF file.

More details are in [docs/design.md](docs/design.md).

## Limitations

* Only AArch64 (ELF64, little endian) is supported - that is the point of
  this linker.
* Thread local storage is not implemented yet.  Objects that use `__thread`
  variables are rejected with a clear error message.
* `--gc-sections` is accepted but not implemented.

## Tests

```
./tests/run.sh            # everything
./tests/run.sh shared     # only the tests that match "shared"
```

The suite builds real AArch64 binaries with the cross compiler and runs them
under `qemu-aarch64`:

* a static executable without any C library,
* a position independent executable that uses the C library,
* a shared library that is loaded at run time,
* a static archive,
* a check of the structure of the produced image.

## License

Apache License 2.0, see [LICENSE](LICENSE).
