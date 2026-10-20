/* ============================================================================
 * ld -- the linker driver
 * ----------------------------------------------------------------------------
 *   ld [options] file...
 *
 * Reads relocatable objects, static archives and shared objects, resolves the
 * symbols, lays the image out and writes an ELF64 AArch64 executable or
 * shared library.
 * ==========================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ld/ld.h"

static LdCtx g_ctx;

static void usage_error(const char *message) {
  fprintf(stderr, "ld: error: %s\n", message);
  options_usage(stderr);
  exit(1);
}

/* The entry point of the image. */
static void resolve_entry(LdCtx *ctx) {
  Symbol *sym = sym_lookup(ctx, ctx->opt.entry);
  if (!sym || sym->state != SYM_DEFINED) {
    if (ctx->kind == OUT_SHARED) {
      ctx->entry_addr = 0;      /* a shared library needs no entry point */
      return;
    }
    ld_error("cannot find entry symbol '%s'", ctx->opt.entry);
    return;
  }
  ctx->entry_addr = sym_address(ctx, sym);
}

int main(int argc, char **argv) {
  LdCtx *ctx = &g_ctx;
  memset(ctx, 0, sizeof(*ctx));
  options_init(&ctx->opt);

  options_parse(ctx, argc, argv);

  if (ctx->opt.print_help) {
    options_usage(stdout);
    return 0;
  }
  if (ctx->opt.print_version) {
    options_version(stdout);
    return 0;
  }
  if (!ctx->opt.nspecs) usage_error("no input files");

  /* the kind of the output decides whether a dynamic section is needed */
  ctx->has_dynamic = !ctx->opt.static_link;
  if (ctx->kind == OUT_SHARED) ctx->has_dynamic = true;
  if (!ctx->has_dynamic) ctx->opt.interp = NULL;
  if (!ctx->opt.interp && ctx->has_dynamic && ctx->kind != OUT_SHARED)
    ctx->opt.interp = xstrdup("/system/bin/linker64");

  /* the base address of the image */
  if (ctx->opt.image_base)
    ctx->base_addr = ctx->opt.image_base;
  else if (ctx->kind == OUT_EXEC)
    ctx->base_addr = 0x400000;
  else
    ctx->base_addr = 0;

  strtab_init(&ctx->dynstr);
  strtab_init(&ctx->shstrtab);

  /* ---- read the input files ---- */
  for (size_t i = 0; i < ctx->opt.nspecs; i++) {
    InputSpec *spec = &ctx->opt.specs[i];
    if (spec->is_lib) {
      /* handled by input_resolve_libraries() */
      continue;
    }
    if (input_add_file(ctx, spec->value) != 0) {
      if (ld_error_count()) return 1;
    }
  }

  /* the shared objects provide the definitions for the undefined symbols */
  for (size_t i = 0; i < ctx->opt.nspecs; i++) {
    InputSpec *spec = &ctx->opt.specs[i];
    if (!spec->is_lib) continue;
    char *path = NULL;
    /* the library search is done by input_resolve_libraries() */
    (void)path;
  }
  input_resolve_libraries(ctx);

  /* static archives are scanned until nothing more can be pulled in */
  input_pull_archives(ctx);

  if (ld_error_count()) return 1;

  /* a static link must not depend on a shared object */
  if (!ctx->has_dynamic && ctx->nshared) {
    ld_error("cannot link against shared libraries in a static link");
    return 1;
  }

  /* ---- symbols ---- */
  sym_allocate_commons(ctx);
  sym_report_undefined(ctx);
  if (ld_error_count()) return 1;

  /* ---- sections ---- */
  sections_create(ctx);

  /* ---- relocations ---- */
  reloc_scan(ctx);
  if (ld_error_count()) return 1;

  /* ---- dynamic information ---- */
  dynamic_create(ctx);

  /* ---- sizes and contents ---- */
  sections_prepare_strings(ctx);
  sections_finalize(ctx);

  /* ---- addresses ---- */
  layout(ctx);

  /* the GOT and PLT addresses are only known after the layout */
  for (size_t i = 0; i < ctx->ngot; i++)
    ctx->got[i].addr = ctx->o_got->addr + i * 8;
  for (size_t i = 0; i < ctx->nplt; i++)
    ctx->plt[i].addr = ctx->o_plt->addr + 32 + 16 * i;

  /* ---- apply the relocations ---- */
  reloc_apply(ctx);
  if (ld_error_count()) return 1;

  /* ---- fill the dynamic structures ---- */
  dynamic_fill(ctx);

  /* ---- the symbol table ---- */
  symtab_fill(ctx);

  /* ---- the entry point ---- */
  resolve_entry(ctx);
  if (ld_error_count()) return 1;

  /* ---- write the output ---- */
  const char *out = ctx->opt.output ? ctx->opt.output : "a.out";
  output_write(ctx, out);

  if (ctx->opt.print_map || ctx->opt.mapfile) {
    if (ctx->opt.mapfile) {
      FILE *f = fopen(ctx->opt.mapfile, "w");
      if (f) {
        output_map(ctx, f);
        fclose(f);
      }
    } else {
      output_map(ctx, stdout);
    }
  }

  if (ld_error_count()) return 1;
  return 0;
}
