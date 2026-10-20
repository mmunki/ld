/* ============================================================================
 * ld -- dynamic linking support
 * ----------------------------------------------------------------------------
 * Everything that is needed to produce a shared library or a position
 * independent executable:
 *
 *   .got / .got.plt    global offset tables
 *   .plt               procedure linkage table (one stub per imported symbol)
 *   .dynsym / .dynstr  dynamic symbol table
 *   .hash / .gnu.hash  symbol lookup tables
 *   .rela.dyn          relative and global data relocations
 *   .rela.plt          jump slot relocations
 *   .dynamic           the dynamic array itself
 *
 * The Android dynamic linker binds eagerly (DT_BIND_NOW), therefore the PLT
 * stubs only have to load the resolved address from the GOT.
 * ==========================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ld/ld.h"

/* --------------------------------------------------------------- ELF hash */
static uint32_t elf_hash(const char *name) {
  uint32_t h = 0, g;
  for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
    h = (h << 4) + *p;
    g = h & 0xf0000000u;
    if (g) h ^= g >> 24;
    h &= ~g;
  }
  return h;
}

static uint32_t gnu_hash(const char *name) {
  uint32_t h = 5381;
  for (const unsigned char *p = (const unsigned char *)name; *p; p++)
    h = (h << 5) + h + *p;
  return h;
}

#define GOTPLT_RESERVED 3

/* ------------------------------------------------------------------- GOT */
void got_add(LdCtx *ctx, Symbol *s, GotKind kind) {
  if (!s) return;
  if (s->got_index != (uint32_t)-1) return;

  if (ctx->ngot == ctx->cap_got) {
    ctx->cap_got = ctx->cap_got ? ctx->cap_got * 2 : 16;
    ctx->got = xrealloc(ctx->got, sizeof(GotEntry) * ctx->cap_got);
  }
  GotEntry *e = &ctx->got[ctx->ngot];
  e->sym = s;
  e->kind = kind;
  e->index = (uint32_t)ctx->ngot;
  e->addr = 0;
  s->got_index = (uint32_t)ctx->ngot;
  s->needs_got = true;
  ctx->ngot++;
  if (kind == GOT_GLOBAL) dynsym_add(ctx, s);
}

/* ------------------------------------------------------------------- PLT */
void plt_add(LdCtx *ctx, Symbol *s) {
  if (!s) return;
  if (s->plt_index != (uint32_t)-1) return;

  if (ctx->nplt == ctx->cap_plt) {
    ctx->cap_plt = ctx->cap_plt ? ctx->cap_plt * 2 : 16;
    ctx->plt = xrealloc(ctx->plt, sizeof(PltEntry) * ctx->cap_plt);
  }
  PltEntry *e = &ctx->plt[ctx->nplt];
  e->sym = s;
  e->index = (uint32_t)ctx->nplt;
  e->addr = 0;
  e->got_slot = (uint32_t)(GOTPLT_RESERVED + ctx->nplt);
  s->plt_index = (uint32_t)ctx->nplt;
  s->needs_plt = true;
  ctx->nplt++;
  dynsym_add(ctx, s);
}

/* --------------------------------------------------------------- dynsym */
uint32_t dynsym_add(LdCtx *ctx, Symbol *s) {
  if (!s) return 0;
  if (s->dynsym_index != (uint32_t)-1) return s->dynsym_index;

  if (ctx->ndynsyms == ctx->cap_dynsyms) {
    ctx->cap_dynsyms = ctx->cap_dynsyms ? ctx->cap_dynsyms * 2 : 32;
    ctx->dynsyms = xrealloc(ctx->dynsyms, sizeof(Symbol *) * ctx->cap_dynsyms);
  }
  ctx->dynsyms[ctx->ndynsyms] = s;
  s->dynsym_index = (uint32_t)ctx->ndynsyms;
  s->exported = true;
  ctx->ndynsyms++;
  return s->dynsym_index;
}

/* --------------------------------------------------------- dyn relocation */
void dynreloc_add(LdCtx *ctx, DynRelocKind kind, Symbol *s,
                   const Relocation *rel, int64_t addend, InputSection *isec,
                   uint64_t site_off) {
  if (ctx->ndynrels == ctx->cap_dynrels) {
    ctx->cap_dynrels = ctx->cap_dynrels ? ctx->cap_dynrels * 2 : 32;
    ctx->dynrels = xrealloc(ctx->dynrels, sizeof(DynReloc) * ctx->cap_dynrels);
  }
  DynReloc *r = &ctx->dynrels[ctx->ndynrels];
  r->offset = 0;
  r->sym = s;
  r->addend = addend;
  r->kind = kind;
  r->sym_index = 0;
  r->isec = isec;
  r->site_off = site_off;
  r->rel = rel;
  switch (kind) {
    case DYN_RELATIVE:  r->type = R_AARCH64_RELATIVE;  break;
    case DYN_GLOB_DAT:  r->type = R_AARCH64_GLOB_DAT;  break;
    case DYN_JUMP_SLOT: r->type = R_AARCH64_JUMP_SLOT; break;
    case DYN_GOT_SLOT:  r->type = R_AARCH64_RELATIVE;  break;
  }
  ctx->ndynrels++;
}

/* ------------------------------------------------------------- PLT stubs */
#define PLT_HEADER_SIZE 32
#define PLT_ENTRY_SIZE  16

static void emit_insn(uint8_t *p, uint32_t inst) {
  for (int i = 0; i < 4; i++) p[i] = (uint8_t)(inst >> (8 * i));
}

static void emit_adrp(uint8_t *p, uint64_t from, uint64_t target, int reg) {
  int64_t delta = (int64_t)((target & ~((uint64_t)0xfff)) -
                             (from & ~((uint64_t)0xfff)));
  int64_t imm = delta >> 12;
  emit_insn(p, 0x90000000u | (uint32_t)((imm & 3) << 29) |
                (uint32_t)(((imm >> 2) & 0x7ffff) << 5) | (uint32_t)reg);
}

static void emit_ldr_u64(uint8_t *p, uint64_t offset, int rn, int rt) {
  emit_insn(p, 0xf9400000u | (uint32_t)(((offset >> 3) & 0xfff) << 10) |
                ((uint32_t)rn << 5) | (uint32_t)rt);
}

static void emit_add_imm(uint8_t *p, uint64_t imm, int rn, int rd) {
  emit_insn(p, 0x91000000u | (uint32_t)((imm & 0xfff) << 10) |
                ((uint32_t)rn << 5) | (uint32_t)rd);
}

static void emit_br(uint8_t *p, int rn) {
  emit_insn(p, 0xd61f0000u | ((uint32_t)rn << 5));
}

static void emit_nop(uint8_t *p) { emit_insn(p, 0xd503201fu); }

/* Build one PLT stub that loads the address from the given GOT slot. */
static void write_plt_stub(uint8_t *p, uint64_t stub_addr, uint64_t got_slot) {
  uint64_t page = got_slot & ~((uint64_t)0xfff);
  uint64_t off = got_slot - page;
  emit_adrp(p, stub_addr, page, 16);
  emit_ldr_u64(p + 4, off, 16, 17);
  emit_add_imm(p + 8, off, 16, 16);
  emit_br(p + 12, 17);
}

/* ------------------------------------------------------------- creation */
typedef struct {
  Symbol *sym;
  uint32_t hash;
  uint32_t bucket;
} DynSymEntry;

static int dynsym_cmp(const void *a, const void *b) {
  const DynSymEntry *x = a, *y = b;
  if (x->bucket != y->bucket) return x->bucket < y->bucket ? -1 : 1;
  if ((x->hash & ~1u) != (y->hash & ~1u))
    return (x->hash & ~1u) < (y->hash & ~1u) ? -1 : 1;
  return 0;
}

static uint32_t choose_nbuckets(size_t n) {
  uint32_t nbuckets = 1;
  while ((uint64_t)nbuckets * 2 < (n ? n : 1)) nbuckets *= 2;
  if (nbuckets > 1024) nbuckets = 1024;
  return nbuckets;
}

/* Does the output section carry any input section? */
static bool has_members(OutSection *o) {
  return o && o->nmembers > 0;
}

/* Find an output section by its name. */
static OutSection *find_osec(LdCtx *ctx, const char *name) {
  for (size_t i = 0; i < ctx->nosecs; i++)
    if (!strcmp(ctx->osecs[i]->name, name)) return ctx->osecs[i];
  return NULL;
}

/* The number of entries in the dynamic array.  It has to be known before the
 * layout, because the size of the .dynamic section depends on it. */
static size_t dynamic_entry_count(LdCtx *ctx) {
  size_t count = 1;                       /* DT_NULL */
  count += 6;                             /* hash, gnu hash, strtab, symtab ... */
  if (ctx->o_reladyn && ctx->o_reladyn->size) count += 4;
  if (ctx->nplt) count += 4;
  if (ctx->kind == OUT_PIE) count += 1;   /* DT_DEBUG */
  if (ctx->opt.z_now) count += 3;
  {
    OutSection *init = find_osec(ctx, ".init");
    OutSection *fini = find_osec(ctx, ".fini");
    OutSection *iarr = find_osec(ctx, ".init_array");
    OutSection *farr = find_osec(ctx, ".fini_array");
    if (has_members(init)) count++;
    if (has_members(fini)) count++;
    if (has_members(iarr)) count += 2;
    if (has_members(farr)) count += 2;
  }
  for (size_t i = 0; i < ctx->nshared; i++)
    if (ctx->shared[i]->is_needed) count++;
  if (ctx->kind == OUT_SHARED && ctx->opt.soname) count++;
  if (ctx->opt.rpath) count++;
  return count;
}

void dynamic_create(LdCtx *ctx) {
  if (!ctx->has_dynamic) return;

  /* Collect the symbols that need an entry in .dynsym. */
  for (size_t i = 0; i < ctx->nsymbols; i++) {
    Symbol *s = ctx->symbols[i];
    if (s->state == SYM_UNDEFINED) {
      if (s->referenced && !s->is_weak) dynsym_add(ctx, s);
      continue;
    }
    if (s->state == SYM_SHARED) {
      if (s->referenced) dynsym_add(ctx, s);
      continue;
    }
    if (s->visibility != STV_DEFAULT) continue;
    if (s->bind != STB_GLOBAL && s->bind != STB_WEAK) continue;
    dynsym_add(ctx, s);
  }

  /* Order the dynamic symbols the way the GNU hash table expects it: by
   * bucket and, inside a bucket, by hash value. */
  size_t n = ctx->ndynsyms;
  DynSymEntry *entries = xcalloc(n ? n : 1, sizeof(DynSymEntry));
  uint32_t nbuckets = choose_nbuckets(n);

  for (size_t i = 0; i < n; i++) {
    entries[i].sym = ctx->dynsyms[i];
    entries[i].hash = gnu_hash(entries[i].sym->name);
    entries[i].bucket = entries[i].hash % nbuckets;
  }
  qsort(entries, n, sizeof(DynSymEntry), dynsym_cmp);

  for (size_t i = 0; i < n; i++) {
    ctx->dynsyms[i] = entries[i].sym;
    entries[i].sym->dynsym_index = (uint32_t)(i + 1);
  }
  free(entries);

  /* .dynstr */
  for (size_t i = 0; i < n; i++)
    strtab_add(&ctx->dynstr, ctx->dynsyms[i]->name);

  /* every GOT slot needs a dynamic relocation */
  for (size_t i = 0; i < ctx->ngot; i++) {
    GotEntry *e = &ctx->got[i];
    dynreloc_add(ctx, DYN_GOT_SLOT, e->sym, NULL, 0, NULL, 0);
  }

  /* the sections themselves */
  ctx->o_dynsym = osec_get(ctx, ".dynsym", SHT_DYNSYM, SHF_ALLOC);
  ctx->o_dynsym->synthetic = true;
  ctx->o_dynsym->align = 8;
  ctx->o_dynsym->size = (uint64_t)(n + 1) * sizeof(Elf64_Sym);  /* + the null symbol */
  ctx->o_dynsym->entsize = sizeof(Elf64_Sym);
  ctx->o_dynsym->sh_info = 1;      /* the first non local symbol */

  ctx->o_dynstr = osec_get(ctx, ".dynstr", SHT_STRTAB, SHF_ALLOC);
  ctx->o_dynstr->synthetic = true;
  ctx->o_dynstr->align = 1;
  ctx->o_dynstr->size = ctx->dynstr.data.size;

  ctx->o_hash = osec_get(ctx, ".hash", SHT_HASH, SHF_ALLOC);
  ctx->o_hash->synthetic = true;
  ctx->o_hash->align = 8;
  ctx->o_hash->entsize = 4;
  ctx->o_hash->size = (2 + nbuckets + (uint64_t)n + 1) * 4;

  ctx->o_gnu_hash = osec_get(ctx, ".gnu.hash", SHT_GNU_HASH, SHF_ALLOC);
  ctx->o_gnu_hash->synthetic = true;
  ctx->o_gnu_hash->align = 8;
  ctx->o_gnu_hash->entsize = 4;
  {
    uint32_t bloom_size = 1;
    while ((uint64_t)bloom_size * 8 < n) bloom_size *= 2;
    /* the header (4 words), the bloom filter (one word each), the buckets
     * and the chain */
    ctx->o_gnu_hash->size = 4 * 4 + (uint64_t)bloom_size * 8 +
                            (uint64_t)nbuckets * 4 + (uint64_t)n * 4;
  }

  ctx->o_reladyn = osec_get(ctx, ".rela.dyn", SHT_RELA, SHF_ALLOC);
  ctx->o_reladyn->synthetic = true;
  ctx->o_reladyn->align = 8;
  ctx->o_reladyn->entsize = sizeof(Elf64_Rela);
  ctx->o_reladyn->size = (uint64_t)ctx->ndynrels * sizeof(Elf64_Rela);

  ctx->o_relaplt = osec_get(ctx, ".rela.plt", SHT_RELA, SHF_ALLOC);
  ctx->o_relaplt->synthetic = true;
  ctx->o_relaplt->align = 8;
  ctx->o_relaplt->entsize = sizeof(Elf64_Rela);
  ctx->o_relaplt->size = (uint64_t)ctx->nplt * sizeof(Elf64_Rela);

  ctx->o_got = osec_get(ctx, ".got", SHT_PROGBITS, SHF_ALLOC | SHF_WRITE);
  ctx->o_got->synthetic = true;
  ctx->o_got->align = 8;
  ctx->o_got->size = (uint64_t)ctx->ngot * 8;

  ctx->o_gotplt = osec_get(ctx, ".got.plt", SHT_PROGBITS, SHF_ALLOC | SHF_WRITE);
  ctx->o_gotplt->synthetic = true;
  ctx->o_gotplt->align = 8;
  ctx->o_gotplt->size = (uint64_t)(GOTPLT_RESERVED + ctx->nplt) * 8;

  ctx->o_plt = osec_get(ctx, ".plt", SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR);
  ctx->o_plt->synthetic = true;
  ctx->o_plt->align = 16;
  ctx->o_plt->size = ctx->nplt ? (uint64_t)(PLT_HEADER_SIZE +
                                           PLT_ENTRY_SIZE * ctx->nplt) : 0;

  ctx->o_dynamic = osec_get(ctx, ".dynamic", SHT_DYNAMIC, SHF_ALLOC | SHF_WRITE);
  ctx->o_dynamic->synthetic = true;
  ctx->o_dynamic->align = 8;
  ctx->o_dynamic->entsize = sizeof(Elf64_Dyn);

  /* The names of the needed libraries and the soname go into .dynstr, they
   * have to be there before the layout because they change its size. */
  for (size_t i = 0; i < ctx->nshared; i++) {
    SharedFile *sh = ctx->shared[i];
    if (!sh->is_needed) continue;
    strtab_add(&ctx->dynstr, sh->soname);
  }
  if (ctx->kind == OUT_SHARED && ctx->opt.soname)
    strtab_add(&ctx->dynstr, ctx->opt.soname);
  if (ctx->opt.rpath) strtab_add(&ctx->dynstr, ctx->opt.rpath);

  /* the size of .dynstr is final now */
  ctx->o_dynstr->size = ctx->dynstr.data.size;

  ctx->o_dynamic->size = dynamic_entry_count(ctx) * sizeof(Elf64_Dyn);
}

/* ----------------------------------------------------------------- filling */
void dynamic_fill(LdCtx *ctx) {
  if (!ctx->has_dynamic) return;

  const size_t n = ctx->ndynsyms;

  /* ---- .dynstr ---- */
  buf_free(&ctx->o_dynstr->data);
  buf_init(&ctx->o_dynstr->data);
  buf_append(&ctx->o_dynstr->data, ctx->dynstr.data.data, ctx->dynstr.data.size);

  /* ---- .dynsym ---- */
  buf_free(&ctx->o_dynsym->data);
  buf_init(&ctx->o_dynsym->data);
  buf_reserve(&ctx->o_dynsym->data, (size_t)ctx->o_dynsym->size);
  buf_zero(&ctx->o_dynsym->data, (size_t)ctx->o_dynsym->size);

  /* the first entry of .dynsym is always the null symbol */
  {
    Elf64_Sym es;
    memset(&es, 0, sizeof(es));
    memcpy(ctx->o_dynsym->data.data, &es, sizeof(es));
  }

  for (size_t i = 0; i < n; i++) {
    Symbol *s = ctx->dynsyms[i];
    Elf64_Sym es;
    memset(&es, 0, sizeof(es));
    es.st_name = strtab_find(&ctx->dynstr, s->name);
    es.st_info = ELF64_ST_INFO(s->bind ? s->bind : STB_GLOBAL, s->type);
    es.st_other = s->visibility;
    es.st_size = s->size;

    uint16_t shndx = SHN_UNDEF;
    if (s->state == SYM_DEFINED && s->isec && s->isec->osec &&
        s->isec->osec->shndx)
      shndx = (uint16_t)s->isec->osec->shndx;
    else if (s->state == SYM_ABSOLUTE)
      shndx = SHN_ABS;

    es.st_shndx = shndx;
    es.st_value = (s->state == SYM_DEFINED) ? sym_address(ctx, s)
                  : (s->state == SYM_ABSOLUTE ? s->value : 0);

    memcpy(ctx->o_dynsym->data.data + (i + 1) * sizeof(Elf64_Sym), &es,
           sizeof(es));
  }

  /* ---- .hash (System V) ---- */
  {
    uint32_t nbuckets = choose_nbuckets(n);
    uint32_t *buckets = xcalloc(nbuckets, sizeof(uint32_t));
    uint32_t *chain = xcalloc(n + 1, sizeof(uint32_t));

    for (size_t i = 0; i < n; i++) {
      uint32_t h = elf_hash(ctx->dynsyms[i]->name);
      uint32_t b = h % nbuckets;
      chain[i + 1] = buckets[b];
      buckets[b] = (uint32_t)(i + 1);
    }

    buf_free(&ctx->o_hash->data);
    buf_init(&ctx->o_hash->data);
    buf_put_u32(&ctx->o_hash->data, nbuckets);
    buf_put_u32(&ctx->o_hash->data, (uint32_t)(n + 1));
    for (uint32_t i = 0; i < nbuckets; i++) buf_put_u32(&ctx->o_hash->data, buckets[i]);
    for (size_t i = 0; i <= n; i++) buf_put_u32(&ctx->o_hash->data, chain[i]);
    free(buckets);
    free(chain);
  }

  /* ---- .gnu.hash ---- */
  {
    uint32_t nbuckets = choose_nbuckets(n);
    uint32_t bloom_size = 1;
    while ((uint64_t)bloom_size * 8 < n) bloom_size *= 2;
    uint32_t bloom_shift = 5;

    uint64_t *bloom = xcalloc(bloom_size, sizeof(uint64_t));
    uint32_t *buckets = xcalloc(nbuckets, sizeof(uint32_t));

    for (size_t i = 0; i < n; i++) {
      uint32_t h = gnu_hash(ctx->dynsyms[i]->name);
      uint32_t b = h % nbuckets;
      if (!buckets[b]) buckets[b] = (uint32_t)(i + 1);
      uint64_t word = (h / 64) % bloom_size;
      bloom[word] |= (uint64_t)1 << (h % 64);
      bloom[word] |= (uint64_t)1 << ((h >> bloom_shift) % 64);
    }

    buf_free(&ctx->o_gnu_hash->data);
    buf_init(&ctx->o_gnu_hash->data);
    buf_put_u32(&ctx->o_gnu_hash->data, nbuckets);
    buf_put_u32(&ctx->o_gnu_hash->data, 1);          /* symoffset */
    buf_put_u32(&ctx->o_gnu_hash->data, bloom_size);
    buf_put_u32(&ctx->o_gnu_hash->data, bloom_shift);
    for (uint32_t i = 0; i < bloom_size; i++)
      buf_put_u64(&ctx->o_gnu_hash->data, bloom[i]);
    for (uint32_t i = 0; i < nbuckets; i++)
      buf_put_u32(&ctx->o_gnu_hash->data, buckets[i]);
    for (size_t i = 0; i < n; i++) {
      uint32_t h = gnu_hash(ctx->dynsyms[i]->name);
      uint32_t chain = h & ~1u;
      /* the last symbol of a bucket has the lowest bit set */
      bool last = true;
      if (i + 1 < n) {
        uint32_t hb = gnu_hash(ctx->dynsyms[i + 1]->name) % nbuckets;
        if (hb == gnu_hash(ctx->dynsyms[i]->name) % nbuckets) last = false;
      }
      if (last) chain |= 1;
      buf_put_u32(&ctx->o_gnu_hash->data, chain);
    }
    free(bloom);
    free(buckets);
  }

  /* ---- .got ---- */
  buf_free(&ctx->o_got->data);
  buf_init(&ctx->o_got->data);
  buf_reserve(&ctx->o_got->data, (size_t)ctx->o_got->size);
  buf_zero(&ctx->o_got->data, (size_t)ctx->o_got->size);

  for (size_t i = 0; i < ctx->ngot; i++) {
    GotEntry *e = &ctx->got[i];
    uint64_t value = 0;
    if (e->kind == GOT_LOCAL && e->sym) value = sym_address(ctx, e->sym);
    uint8_t *p = ctx->o_got->data.data + i * 8;
    for (int k = 0; k < 8; k++) p[k] = (uint8_t)(value >> (8 * k));
  }

  /* ---- .got.plt ---- */
  buf_free(&ctx->o_gotplt->data);
  buf_init(&ctx->o_gotplt->data);
  buf_reserve(&ctx->o_gotplt->data, (size_t)ctx->o_gotplt->size);
  buf_zero(&ctx->o_gotplt->data, (size_t)ctx->o_gotplt->size);

  {
    uint64_t dynamic_addr = ctx->o_dynamic ? ctx->o_dynamic->addr : 0;
    uint8_t *p = ctx->o_gotplt->data.data;
    uint64_t v[3] = { dynamic_addr, 0, dynamic_addr };
    for (int i = 0; i < 3; i++)
      for (int k = 0; k < 8; k++) p[i * 8 + k] = (uint8_t)(v[i] >> (8 * k));
  }

  /* ---- .plt ---- */
  if (ctx->nplt) {
    buf_free(&ctx->o_plt->data);
    buf_init(&ctx->o_plt->data);
    buf_reserve(&ctx->o_plt->data, (size_t)ctx->o_plt->size);
    buf_zero(&ctx->o_plt->data, (size_t)ctx->o_plt->size);

    uint64_t gotplt = ctx->o_gotplt->addr;
    uint8_t *p = ctx->o_plt->data.data;
    /* header: loads the address of .dynamic, only used by lazy binding */
    emit_adrp(p, ctx->o_plt->addr, gotplt & ~((uint64_t)0xfff), 16);
    emit_ldr_u64(p + 4, (gotplt + 16) & 0xfff, 16, 17);
    emit_add_imm(p + 8, (gotplt + 16) & 0xfff, 16, 16);
    emit_br(p + 12, 17);
    for (int i = 16; i < PLT_HEADER_SIZE; i += 4) emit_nop(p + i);

    for (size_t i = 0; i < ctx->nplt; i++) {
      PltEntry *e = &ctx->plt[i];
      uint64_t slot = ctx->o_gotplt->addr + e->got_slot * 8;
      uint64_t stub = ctx->o_plt->addr + PLT_HEADER_SIZE + PLT_ENTRY_SIZE * i;
      write_plt_stub(p + PLT_HEADER_SIZE + PLT_ENTRY_SIZE * i, stub, slot);
    }
  }

  /* ---- .rela.dyn ---- */
  buf_free(&ctx->o_reladyn->data);
  buf_init(&ctx->o_reladyn->data);
  buf_reserve(&ctx->o_reladyn->data, (size_t)ctx->o_reladyn->size);
  buf_zero(&ctx->o_reladyn->data, (size_t)ctx->o_reladyn->size);

  uint32_t relacount = 0;
  size_t written = 0;
  for (int pass = 0; pass < 2; pass++) {
    for (size_t i = 0; i < ctx->ndynrels; i++) {
      DynReloc *r = &ctx->dynrels[i];
      bool relative = (r->kind == DYN_RELATIVE || r->kind == DYN_GOT_SLOT);
      if ((pass == 0) != relative) continue;

      uint64_t site = 0;
      int64_t addend = 0;
      uint32_t sym_index = 0;
      uint32_t type = r->type;

      switch (r->kind) {
        case DYN_RELATIVE:
          site = r->isec ? r->isec->addr + r->site_off : r->offset;
          uint64_t value = r->rel ? reloc_symbol_value(ctx, r->rel)
                                  : (r->sym ? sym_address(ctx, r->sym) : 0);
          addend = (int64_t)(value + r->addend);
          sym_index = 0;
          type = R_AARCH64_RELATIVE;
          relacount++;
          break;
        case DYN_GOT_SLOT: {
          if (!r->sym || r->sym->got_index == (uint32_t)-1) continue;
          site = ctx->got[r->sym->got_index].addr;
          if (ctx->got[r->sym->got_index].kind == GOT_LOCAL) {
            type = R_AARCH64_RELATIVE;
            addend = (int64_t)sym_address(ctx, r->sym);
            sym_index = 0;
            relacount++;
          } else {
            type = R_AARCH64_GLOB_DAT;
            addend = 0;
            sym_index = r->sym->dynsym_index;
          }
          break;
        }
        case DYN_GLOB_DAT:
          site = r->isec ? r->isec->addr + r->site_off : r->offset;
          addend = r->addend;
          sym_index = r->sym ? r->sym->dynsym_index : 0;
          type = R_AARCH64_GLOB_DAT;
          break;
        case DYN_JUMP_SLOT:
          site = r->offset;
          addend = 0;
          sym_index = r->sym ? r->sym->dynsym_index : 0;
          type = R_AARCH64_JUMP_SLOT;
          break;
        default:
          continue;
      }

      Elf64_Rela rela;
      rela.r_offset = site;
      rela.r_info = ELF64_R_INFO(sym_index, type);
      rela.r_addend = addend;
      memcpy(ctx->o_reladyn->data.data + written * sizeof(Elf64_Rela),
             &rela, sizeof(rela));
      written++;
    }
  }

  /* ---- .rela.plt ---- */
  buf_free(&ctx->o_relaplt->data);
  buf_init(&ctx->o_relaplt->data);
  buf_reserve(&ctx->o_relaplt->data, (size_t)ctx->o_relaplt->size);
  buf_zero(&ctx->o_relaplt->data, (size_t)ctx->o_relaplt->size);

  for (size_t i = 0; i < ctx->nplt; i++) {
    PltEntry *e = &ctx->plt[i];
    Elf64_Rela rela;
    rela.r_offset = ctx->o_gotplt->addr + e->got_slot * 8;
    rela.r_info = ELF64_R_INFO(e->sym ? e->sym->dynsym_index : 0,
                               R_AARCH64_JUMP_SLOT);
    rela.r_addend = 0;
    memcpy(ctx->o_relaplt->data.data + i * sizeof(Elf64_Rela), &rela,
           sizeof(rela));
  }

  /* ---- .dynamic ---- */
  {
    size_t count = 32;
    for (size_t i = 0; i < ctx->nshared; i++)
      if (ctx->shared[i]->is_needed) count++;

    buf_free(&ctx->o_dynamic->data);
    buf_init(&ctx->o_dynamic->data);
    buf_reserve(&ctx->o_dynamic->data, count * sizeof(Elf64_Dyn));

    Elf64_Dyn d;
#define PUT_DYN(tag, val) do {                                \
      d.d_tag = (int64_t)(tag);                               \
      d.d_un.d_val = (uint64_t)(val);                         \
      buf_append(&ctx->o_dynamic->data, &d, sizeof(d));       \
    } while (0)

    for (size_t i = 0; i < ctx->nshared; i++) {
      SharedFile *sh = ctx->shared[i];
      if (!sh->is_needed) continue;
      uint32_t off = strtab_find(&ctx->dynstr, sh->soname);
      PUT_DYN(DT_NEEDED, off);
    }
    if (ctx->kind == OUT_SHARED && ctx->opt.soname) {
      uint32_t off = strtab_find(&ctx->dynstr, ctx->opt.soname);
      PUT_DYN(DT_SONAME, off);
    }
    if (ctx->opt.rpath) {
      uint32_t off = strtab_find(&ctx->dynstr, ctx->opt.rpath);
      PUT_DYN(DT_RUNPATH, off);
    }
    PUT_DYN(DT_HASH, ctx->o_hash->addr);
    PUT_DYN(DT_GNU_HASH, ctx->o_gnu_hash->addr);
    PUT_DYN(DT_STRTAB, ctx->o_dynstr->addr);
    PUT_DYN(DT_STRSZ, ctx->o_dynstr->size);
    PUT_DYN(DT_SYMTAB, ctx->o_dynsym->addr);
    PUT_DYN(DT_SYMENT, sizeof(Elf64_Sym));
    if (ctx->o_reladyn->size) {
      PUT_DYN(DT_RELA, ctx->o_reladyn->addr);
      PUT_DYN(DT_RELASZ, ctx->o_reladyn->size);
      PUT_DYN(DT_RELAENT, sizeof(Elf64_Rela));
      PUT_DYN(DT_RELACOUNT, relacount);
    }
    if (ctx->nplt) {
      PUT_DYN(DT_JMPREL, ctx->o_relaplt->addr);
      PUT_DYN(DT_PLTRELSZ, ctx->o_relaplt->size);
      PUT_DYN(DT_PLTREL, DT_RELA);
      PUT_DYN(DT_PLTGOT, ctx->o_gotplt->addr);
    }
    if (ctx->kind == OUT_PIE) PUT_DYN(DT_DEBUG, 0);

    /* the initialisation and termination functions and their arrays */
    {
      OutSection *init = find_osec(ctx, ".init");
      OutSection *fini = find_osec(ctx, ".fini");
      OutSection *iarr = find_osec(ctx, ".init_array");
      OutSection *farr = find_osec(ctx, ".fini_array");
      if (has_members(init)) PUT_DYN(DT_INIT, init->addr);
      if (has_members(fini)) PUT_DYN(DT_FINI, fini->addr);
      if (has_members(iarr)) {
        PUT_DYN(DT_INIT_ARRAY, iarr->addr);
        PUT_DYN(DT_INIT_ARRAYSZ, iarr->size);
      }
      if (has_members(farr)) {
        PUT_DYN(DT_FINI_ARRAY, farr->addr);
        PUT_DYN(DT_FINI_ARRAYSZ, farr->size);
      }
    }
    if (ctx->opt.z_now) {
      PUT_DYN(DT_BIND_NOW, 0);
      PUT_DYN(DT_FLAGS, DF_BIND_NOW);
      PUT_DYN(DT_FLAGS_1, DF_1_NOW | (ctx->kind == OUT_PIE ? DF_1_PIE : 0));
    }
    /* The RELRO region is described by PT_GNU_RELRO, which is what the
     * Android dynamic linker looks at. */
    PUT_DYN(DT_NULL, 0);
#undef PUT_DYN

    if (ctx->o_dynamic->data.size != ctx->o_dynamic->size)
      ld_error("internal error: the size of .dynamic changed");
    ctx->o_dynamic->size = ctx->o_dynamic->data.size;
  }
}
