/* ============================================================================
 * ld -- command line handling
 * ----------------------------------------------------------------------------
 * The option set intentionally mirrors the subset of GNU ld / lld that is
 * actually used when building Android aarch64 binaries, so that the linker
 * can be dropped into an existing build system.
 * ==========================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ld/ld.h"

void options_init(LdOptions *o) {
  memset(o, 0, sizeof(*o));
  o->entry = NULL;
  o->page_size = 4096;
  o->image_base = 0;
  o->z_now = true;          /* Android binds eagerly, like -z now        */
  o->z_relro = true;        /* full RELRO is the Android default         */
  o->z_execstack = false;   /* non executable stack                      */
  o->eh_frame_hdr = true;
  o->build_id = false;
}

static void add_spec(LdOptions *o, const char *value, bool is_lib) {
  o->specs = xrealloc(o->specs, sizeof(InputSpec) * (o->nspecs + 1));
  o->specs[o->nspecs].value = xstrdup(value);
  o->specs[o->nspecs].is_lib = is_lib;
  o->nspecs++;
}

static void add_str(char ***list, size_t *count, const char *value) {
  *list = xrealloc(*list, sizeof(char *) * (*count + 1));
  (*list)[(*count)++] = xstrdup(value);
}

void options_version(FILE *out) {
  fprintf(out, "ld (mmunki aarch64-android linker) 0.1.0\n");
  fprintf(out, "Target: aarch64-linux-android (ELF64, little endian)\n");
  fprintf(out, "Copyright (C) 2026 mmunki. License Apache-2.0.\n");
}

void options_usage(FILE *out) {
  fprintf(out,
"Usage: ld [options] file...\n"
"\n"
"Link AArch64 relocatable objects into an ELF executable or shared library.\n"
"\n"
"Output selection:\n"
"  -o FILE                 write the output to FILE\n"
"  -shared                 create a shared library (ET_DYN)\n"
"  -pie                    create a position independent executable\n"
"  -no-pie                 create a non position independent executable\n"
"  -static                 do not link against shared libraries\n"
"  -e SYM, --entry=SYM     entry point symbol (default: _start)\n"
"  -soname NAME            shared object name (DT_SONAME)\n"
"  -dynamic-linker PATH    program interpreter (default: /system/bin/linker64)\n"
"  -Map FILE, --print-map  write a link map to FILE\n"
"  -s, --strip-all          do not write a symbol table\n"
"  --build-id              emit a .note.gnu.build-id note\n"
"  --eh-frame-hdr          emit .eh_frame_hdr (default: on)\n"
"\n"
"Library search:\n"
"  -L DIR                  add DIR to the library search path\n"
"  -l NAME                 link against libNAME.so / libNAME.a\n"
"  -rpath PATH             runtime library search path\n"
"  --sysroot DIR           prefix applied to library search paths\n"
"  --allow-shlib-undefined  allow undefined symbols in shared libraries\n"
"  --no-undefined          report undefined symbols as errors\n"
"\n"
"Layout:\n"
"  --image-base ADDR       base address of the image\n"
"  --page-size N           page size used for segment alignment\n"
"  -z now|relro|noexecstack linker behaviour switches\n"
"  --gc-sections           discard unreferenced sections (not implemented yet)\n"
"\n"
"Other:\n"
"  -m EMULATION             accepted and ignored (only elf_aarch64)\n"
"  -v, --version            print version information\n"
"  -h, --help               print this help\n");
}

static uint64_t parse_number(const char *text) {
  return strtoull(text, NULL, 0);
}

int options_parse(LdCtx *ctx, int argc, char **argv) {
  LdOptions *o = &ctx->opt;

  for (int i = 1; i < argc; i++) {
    const char *arg = argv[i];

    if (arg[0] != '-' || arg[1] == '\0') {
      /* plain input file */
      add_spec(o, arg, false);
      continue;
    }

    if (!strcmp(arg, "-o")) {
      if (++i >= argc) ld_fatal("option '-o' requires an argument");
      o->output = xstrdup(argv[i]);
    } else if (!strncmp(arg, "-o", 2) && arg[2]) {
      o->output = xstrdup(arg + 2);
    } else if (!strcmp(arg, "-L")) {
      if (++i >= argc) ld_fatal("option '-L' requires an argument");
      add_str(&o->libpaths, &o->nlibpaths, argv[i]);
    } else if (!strncmp(arg, "-L", 2) && arg[2]) {
      add_str(&o->libpaths, &o->nlibpaths, arg + 2);
    } else if (!strcmp(arg, "-l")) {
      if (++i >= argc) ld_fatal("option '-l' requires an argument");
      add_spec(o, argv[i], true);
    } else if (!strncmp(arg, "-l", 2) && arg[2]) {
      add_spec(o, arg + 2, true);
    } else if (!strcmp(arg, "-shared")) {
      o->shared = true;
    } else if (!strcmp(arg, "-pie")) {
      o->pie = true;
    } else if (!strcmp(arg, "-no-pie") || !strcmp(arg, "--no-pie")) {
      o->no_pie = true;
    } else if (!strcmp(arg, "-static")) {
      o->static_link = true;
    } else if (!strcmp(arg, "-e")) {
      if (++i >= argc) ld_fatal("option '-e' requires an argument");
      o->entry = xstrdup(argv[i]);
    } else if (!strncmp(arg, "--entry=", 8)) {
      o->entry = xstrdup(arg + 8);
    } else if (!strcmp(arg, "-soname") || !strcmp(arg, "--soname")) {
      if (++i >= argc) ld_fatal("option '-soname' requires an argument");
      o->soname = xstrdup(argv[i]);
    } else if (!strncmp(arg, "--soname=", 9)) {
      o->soname = xstrdup(arg + 9);
    } else if (!strcmp(arg, "-dynamic-linker") || !strcmp(arg, "--dynamic-linker")) {
      if (++i >= argc) ld_fatal("option '-dynamic-linker' requires an argument");
      o->interp = xstrdup(argv[i]);
    } else if (!strncmp(arg, "--dynamic-linker=", 17)) {
      o->interp = xstrdup(arg + 17);
    } else if (!strcmp(arg, "-rpath")) {
      if (++i >= argc) ld_fatal("option '-rpath' requires an argument");
      o->rpath = xstrdup(argv[i]);
    } else if (!strncmp(arg, "--rpath=", 8)) {
      o->rpath = xstrdup(arg + 8);
    } else if (!strcmp(arg, "--sysroot")) {
      if (++i >= argc) ld_fatal("option '--sysroot' requires an argument");
      o->sysroot = xstrdup(argv[i]);
    } else if (!strncmp(arg, "--sysroot=", 10)) {
      o->sysroot = xstrdup(arg + 10);
    } else if (!strcmp(arg, "-Map") || !strcmp(arg, "--print-map")) {
      if (!strcmp(arg, "-Map")) {
        if (++i >= argc) ld_fatal("option '-Map' requires an argument");
        o->mapfile = xstrdup(argv[i]);
      } else {
        o->print_map = true;
      }
    } else if (!strncmp(arg, "-Map=", 5)) {
      o->mapfile = xstrdup(arg + 5);
    } else if (!strcmp(arg, "-s") || !strcmp(arg, "--strip-all")) {
      o->strip_all = true;
    } else if (!strcmp(arg, "--no-undefined")) {
      o->no_undefined = true;
    } else if (!strcmp(arg, "--allow-shlib-undefined")) {
      o->allow_undefined = true;
    } else if (!strcmp(arg, "--build-id")) {
      o->build_id = true;
    } else if (!strcmp(arg, "--eh-frame-hdr")) {
      o->eh_frame_hdr = true;
    } else if (!strcmp(arg, "--no-eh-frame-hdr")) {
      o->eh_frame_hdr = false;
    } else if (!strcmp(arg, "--gc-sections")) {
      o->gc_sections = true;
    } else if (!strcmp(arg, "--image-base")) {
      if (++i >= argc) ld_fatal("option '--image-base' requires an argument");
      o->image_base = parse_number(argv[i]);
    } else if (!strncmp(arg, "--image-base=", 13)) {
      o->image_base = parse_number(arg + 13);
    } else if (!strcmp(arg, "--page-size")) {
      if (++i >= argc) ld_fatal("option '--page-size' requires an argument");
      o->page_size = parse_number(argv[i]);
    } else if (!strncmp(arg, "--page-size=", 13)) {
      o->page_size = parse_number(arg + 13);
    } else if (!strcmp(arg, "-z")) {
      if (++i >= argc) ld_fatal("option '-z' requires an argument");
      const char *z = argv[i];
      if (!strcmp(z, "now")) o->z_now = true;
      else if (!strcmp(z, "relro")) o->z_relro = true;
      else if (!strcmp(z, "norelro")) o->z_relro = false;
      else if (!strcmp(z, "execstack")) o->z_execstack = true;
      else if (!strcmp(z, "noexecstack")) o->z_execstack = false;
      else ld_warn("ignoring unsupported '-z %s'", z);
    } else if (!strcmp(arg, "-m")) {
      if (++i >= argc) ld_fatal("option '-m' requires an argument");
      if (strcmp(argv[i], "elf_aarch64") && strcmp(argv[i], "aarch64linux"))
        ld_warn("unsupported emulation '%s', only elf_aarch64 is supported", argv[i]);
    } else if (!strncmp(arg, "-m", 2) && arg[2]) {
      if (strcmp(arg + 2, "elf_aarch64") && strcmp(arg + 2, "aarch64linux"))
        ld_warn("unsupported emulation '%s', only elf_aarch64 is supported", arg + 2);
    } else if (!strcmp(arg, "-v") || !strcmp(arg, "--version")) {
      o->print_version = true;
    } else if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) {
      o->print_help = true;
    } else if (!strcmp(arg, "--")) {
      /* everything after -- is an input file */
      continue;
    } else if (!strcmp(arg, "-r") || !strcmp(arg, "--relocatable")) {
      ld_fatal("relocatable output (-r) is not supported by this linker");
    } else if (!strcmp(arg, "-static-pie")) {
      o->pie = true;
      o->static_link = true;
    } else {
      ld_fatal("unrecognised option '%s'", arg);
    }
  }

  if (o->shared && o->static_link)
    ld_fatal("cannot combine '-shared' and '-static'");

  if (o->shared) {
    ctx->kind = OUT_SHARED;
  } else if (o->static_link) {
    ctx->kind = OUT_EXEC;
  } else if (o->no_pie) {
    ctx->kind = OUT_EXEC;
  } else {
    ctx->kind = OUT_PIE;
  }

  if (!o->page_size) o->page_size = 4096;
  if (!o->entry) o->entry = xstrdup("_start");
  if (!o->interp && ctx->kind == OUT_PIE) o->interp = xstrdup("/system/bin/linker64");

  return 0;
}
