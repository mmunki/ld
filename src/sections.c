/* ============================================================================
 * ld -- output section handling
 * ----------------------------------------------------------------------------
 * Input sections are collected into output sections.  The mapping follows the
 * usual conventions of an ELF linker:
 *
 *   .text              executable code
 *   .rodata            read only data
 *   .data / .bss       writable data
 *   .init_array        constructors
 *   .fini_array        destructors
 *   .eh_frame          unwind information
 *   .note.*            notes
 *
 * The generated sections (.plt, .got, .dynsym, ...) are created here as well
 * so that the final section order is known before the layout is computed.
 * ==========================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ld/ld.h"

/* --------------------------------------------------------------- registry */
OutSection *osec_get(LdCtx *ctx, const char *name, uint32_t type, uint64_t flags) {
  for (size_t i = 0; i < ctx->nosecs; i++) {
    OutSection *o = ctx->osecs[i];
    if (!strcmp(o->name, name)) return o;
  }
  OutSection *o = xcalloc(1, sizeof(OutSection));
  o->name = name;
  o->type = type;
  o->flags = flags;
  o->align = 1;
  o->shndx = 0;
  buf_init(&o->data);
  ctx->osecs = xrealloc(ctx->osecs, sizeof(OutSection *) * (ctx->nosecs + 1));
  ctx->osecs[ctx->nosecs++] = o;
  return o;
}

void osec_add_member(OutSection *o, InputSection *is) {
  if (o->nmembers == o->cap_members) {
    o->cap_members = o->cap_members ? o->cap_members * 2 : 8;
    o->members = xrealloc(o->members, sizeof(InputSection *) * o->cap_members);
  }
  o->members[o->nmembers++] = is;
  is->osec = o;
}

/* --------------------------------------------------- input -> output map */
static const char *output_name_for(const InputSection *is) {
  if (!strcmp(is->name, ".init")) return ".init";
  if (!strcmp(is->name, ".fini")) return ".fini";
  if (!strcmp(is->name, ".eh_frame")) return ".eh_frame";
  if (!strcmp(is->name, ".tdata")) return ".tdata";
  if (!strcmp(is->name, ".tbss")) return ".tbss";
  if (is->type == SHT_NOTE) return is->name;
  if (is->type == SHT_INIT_ARRAY) return ".init_array";
  if (is->type == SHT_FINI_ARRAY) return ".fini_array";
  if (is->type == SHT_PREINIT_ARRAY) return ".preinit_array";
  if (is->flags & SHF_EXECINSTR) return ".text";
  if (is->flags & SHF_WRITE) return is->is_bss ? ".bss" : ".data";
  return ".rodata";
}

static uint64_t output_flags_for(const InputSection *is) {
  uint64_t f = SHF_ALLOC;
  if (is->flags & SHF_WRITE) f |= SHF_WRITE;
  if (is->flags & SHF_EXECINSTR) f |= SHF_EXECINSTR;
  return f;
}

/* ------------------------------------------------------------- creation */
void sections_create(LdCtx *ctx) {
  ctx->o_interp = NULL;
  ctx->o_hash = NULL;
  ctx->o_gnu_hash = NULL;
  ctx->o_dynsym = NULL;
  ctx->o_dynstr = NULL;
  ctx->o_reladyn = NULL;
  ctx->o_relaplt = NULL;
  ctx->o_plt = NULL;
  ctx->o_got = NULL;
  ctx->o_gotplt = NULL;
  ctx->o_dynamic = NULL;
  ctx->o_eh_frame = NULL;
  ctx->o_note = NULL;

  if (ctx->opt.interp && ctx->kind != OUT_SHARED) {
    ctx->o_interp = osec_get(ctx, ".interp", SHT_PROGBITS, SHF_ALLOC);
    ctx->o_interp->align = 1;
    ctx->o_interp->synthetic = true;
    ctx->o_interp->size = strlen(ctx->opt.interp) + 1;
    buf_init(&ctx->o_interp->data);
    buf_put_str0(&ctx->o_interp->data, ctx->opt.interp);
  }

  ctx->o_text = osec_get(ctx, ".text", SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR);
  ctx->o_rodata = osec_get(ctx, ".rodata", SHT_PROGBITS, SHF_ALLOC);
  ctx->o_data = osec_get(ctx, ".data", SHT_PROGBITS, SHF_ALLOC | SHF_WRITE);
  ctx->o_bss = osec_get(ctx, ".bss", SHT_NOBITS, SHF_ALLOC | SHF_WRITE);
  ctx->o_init_array = osec_get(ctx, ".init_array", SHT_INIT_ARRAY,
                               SHF_ALLOC | SHF_WRITE);
  ctx->o_fini_array = osec_get(ctx, ".fini_array", SHT_FINI_ARRAY,
                               SHF_ALLOC | SHF_WRITE);

  /* assign the input sections */
  for (size_t f = 0; f < ctx->nobj; f++) {
    InputFile *file = ctx->objects[f];
    for (size_t i = 0; i < file->nsections; i++) {
      InputSection *is = file->sections[i];
      if (!is || !is->live) continue;
      const char *name = output_name_for(is);
      OutSection *o = osec_get(ctx, name, is->is_bss ? SHT_NOBITS : SHT_PROGBITS,
                               output_flags_for(is));
      osec_add_member(o, is);
      if (is->align > o->align) o->align = is->align;
      if (o->type == SHT_PROGBITS && is->type == SHT_NOBITS) {
        o->type = SHT_NOBITS;
        o->flags |= SHF_WRITE;
      }
      if (is->type == SHT_NOTE) {
        o->type = SHT_NOTE;
        o->flags = SHF_ALLOC;
      }
    }
  }

  /* the synthetic section that holds the common symbols */
  for (size_t i = 0; i < ctx->nextra; i++) {
    InputSection *is = ctx->extra[i];
    if (!is || !is->live) continue;
    OutSection *o = osec_get(ctx, ".bss", SHT_NOBITS, SHF_ALLOC | SHF_WRITE);
    osec_add_member(o, is);
    if (is->align > o->align) o->align = is->align;
  }

  /* the symbol table */
  if (!ctx->opt.strip_all) {
    ctx->o_symtab = osec_get(ctx, ".symtab", SHT_SYMTAB, 0);
    ctx->o_strtab = osec_get(ctx, ".strtab", SHT_STRTAB, 0);
    ctx->o_symtab->synthetic = true;
    ctx->o_strtab->synthetic = true;
    ctx->o_symtab->align = 8;
    ctx->o_strtab->align = 1;
    ctx->o_symtab->entsize = sizeof(Elf64_Sym);

    size_t count = 1;
    for (size_t i = 0; i < ctx->nsymbols; i++) {
      Symbol *s = ctx->symbols[i];
      if (s->state != SYM_DEFINED && s->state != SYM_ABSOLUTE) continue;
      if (s->bind == STB_LOCAL) continue;
      count++;
    }
    ctx->o_symtab->size = (uint64_t)count * sizeof(Elf64_Sym);

    /* the names of the symbols go into .strtab */
    strtab_init(&ctx->symstr);
    for (size_t i = 0; i < ctx->nsymbols; i++) {
      Symbol *s = ctx->symbols[i];
      if (s->state != SYM_DEFINED && s->state != SYM_ABSOLUTE) continue;
      if (s->bind == STB_LOCAL) continue;
      strtab_add(&ctx->symstr, s->name);
    }
    ctx->o_strtab->size = ctx->symstr.data.size;
    ctx->o_strtab->data = ctx->symstr.data;
  }
  ctx->o_shstrtab_out = osec_get(ctx, ".shstrtab", SHT_STRTAB, 0);
  ctx->o_shstrtab_out->synthetic = true;
  ctx->o_shstrtab_out->align = 1;
}

/* The section header string table has to be built when every section is
 * known, which is the case after the dynamic sections have been created. */
void sections_prepare_strings(LdCtx *ctx) {
  strtab_init(&ctx->shstrtab);
  for (size_t i = 0; i < ctx->nosecs; i++) {
    OutSection *o = ctx->osecs[i];
    o->sh_name = strtab_add(&ctx->shstrtab, o->name);
  }
  ctx->o_shstrtab_out->size = ctx->shstrtab.data.size;
  ctx->o_shstrtab_out->data = ctx->shstrtab.data;
}

/* Compute the size of every output section and copy the input data. */
void sections_finalize(LdCtx *ctx) {
  for (size_t i = 0; i < ctx->nosecs; i++) {
    OutSection *o = ctx->osecs[i];
    if (o->synthetic) continue;   /* generated sections size themselves */

    uint64_t off = 0;
    for (size_t m = 0; m < o->nmembers; m++) {
      InputSection *is = o->members[m];
      off = align_up(off, is->align);
      is->out_off = off;
      off += is->size;
    }
    o->size = off;
    if (o->align < 1) o->align = 1;

    if (o->type == SHT_NOBITS) continue;

    buf_free(&o->data);
    buf_init(&o->data);
    buf_reserve(&o->data, (size_t)o->size);
    buf_zero(&o->data, (size_t)o->size);
    for (size_t m = 0; m < o->nmembers; m++) {
      InputSection *is = o->members[m];
      if (!is->data || !is->size) continue;
      memcpy(o->data.data + is->out_off, is->data, (size_t)is->size);
    }
  }
}
