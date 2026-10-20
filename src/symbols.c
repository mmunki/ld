/* ============================================================================
 * ld -- global symbol table and symbol resolution
 * ----------------------------------------------------------------------------
 * Resolution rules follow the usual ELF conventions:
 *
 *   - a real definition beats a common (tentative) definition
 *   - a strong definition beats a weak definition
 *   - the first definition of a symbol wins, a second strong definition is
 *     an error
 *   - a common symbol is allocated in .bss using the largest size and the
 *     strictest alignment requested by any of the participants
 *   - a symbol coming from a shared object is only used when no object
 *     defines it
 * ==========================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ld/ld.h"

/* ------------------------------------------------------------- hash table */
typedef struct {
  char   *name;
  Symbol *sym;
} SymSlot;

static SymSlot *g_slots = NULL;
static size_t   g_slot_cap = 0;
static size_t   g_slot_used = 0;

static size_t hash_name(const char *name) {
  size_t h = 1469598103934665603ULL;
  for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
    h ^= *p;
    h *= 1099511628211ULL;
  }
  return h;
}

static void slots_grow(void) {
  size_t cap = g_slot_cap ? g_slot_cap * 2 : 256;
  SymSlot *slots = xcalloc(cap, sizeof(SymSlot));
  for (size_t i = 0; i < g_slot_cap; i++) {
    if (!g_slots[i].sym) continue;
    size_t j = hash_name(g_slots[i].name) & (cap - 1);
    while (slots[j].sym) j = (j + 1) & (cap - 1);
    slots[j] = g_slots[i];
  }
  free(g_slots);
  g_slots = slots;
  g_slot_cap = cap;
}

static Symbol *slot_find(const char *name) {
  if (!g_slot_cap) return NULL;
  size_t i = hash_name(name) & (g_slot_cap - 1);
  while (g_slots[i].sym) {
    if (!strcmp(g_slots[i].name, name)) return g_slots[i].sym;
    i = (i + 1) & (g_slot_cap - 1);
  }
  return NULL;
}

static void slot_insert(const char *name, Symbol *sym) {
  if ((g_slot_used + 1) * 10 >= g_slot_cap * 7) slots_grow();
  size_t i = hash_name(name) & (g_slot_cap - 1);
  while (g_slots[i].sym) i = (i + 1) & (g_slot_cap - 1);
  g_slots[i].name = sym->name;
  g_slots[i].sym = sym;
  g_slot_used++;
}

/* -------------------------------------------------------------- interface */
Symbol *sym_lookup(LdCtx *ctx, const char *name) {
  (void)ctx;
  return slot_find(name);
}

Symbol *sym_get(LdCtx *ctx, const char *name) {
  Symbol *s = slot_find(name);
  if (s) return s;

  s = xcalloc(1, sizeof(Symbol));
  s->name = xstrdup(name);
  s->bind = STB_GLOBAL;
  s->type = STT_NOTYPE;
  s->visibility = STV_DEFAULT;
  s->state = SYM_UNDEFINED;
  s->got_index = (uint32_t)-1;
  s->gotplt_index = (uint32_t)-1;
  s->plt_index = (uint32_t)-1;
  s->dynsym_index = (uint32_t)-1;
  slot_insert(name, s);

  ctx->symbols = xrealloc(ctx->symbols, sizeof(Symbol *) * (ctx->nsymbols + 1));
  ctx->symbols[ctx->nsymbols++] = s;
  return s;
}

void sym_reference(LdCtx *ctx, const char *name, uint8_t bind) {
  Symbol *s = sym_get(ctx, name);
  s->referenced = true;
  if (s->state == SYM_UNDEFINED && bind == STB_WEAK) s->is_weak = true;
}

static void discard_group(ComdatGroup *g) {
  for (size_t i = 0; i < g->nmembers; i++)
    if (g->members[i]) g->members[i]->live = false;
}

void sym_define(LdCtx *ctx, Symbol *s, InputFile *file, InputSection *isec,
                uint64_t value, uint64_t size, uint8_t bind, uint8_t type,
                uint8_t visibility, SymState state) {
  (void)ctx;
  /* COMDAT: only the first definition of a group survives */
  if (isec && isec->group) {
    ComdatGroup *g = isec->group;
    if (g->claimed) {
      discard_group(g);
      return;
    }
    g->claimed = true;
  }

  if (s->state == SYM_UNDEFINED) {
    s->state = state;
    s->file = file;
    s->isec = isec;
    s->value = value;
    s->size = size;
    s->bind = bind;
    s->type = type;
    s->visibility = visibility;
    s->is_weak = (bind == STB_WEAK);
    return;
  }

  if (s->state == SYM_COMMON) {
    /* a real definition replaces the tentative one */
    if (s->is_weak && bind != STB_WEAK) {
      s->state = state;
      s->file = file;
      s->isec = isec;
      s->value = value;
      s->size = size;
      s->bind = bind;
      s->type = type;
      s->visibility = visibility;
      s->is_weak = false;
    }
    return;
  }

  if (s->state == SYM_DEFINED || s->state == SYM_ABSOLUTE) {
    if (s->is_weak && bind != STB_WEAK && visibility == STV_DEFAULT) {
      s->state = state;
      s->file = file;
      s->isec = isec;
      s->value = value;
      s->size = size;
      s->bind = bind;
      s->type = type;
      s->visibility = visibility;
      s->is_weak = false;
    } else if (!s->is_weak && bind != STB_WEAK && s->visibility == STV_DEFAULT) {
      ld_error("multiple definition of symbol '%s'", s->name);
      if (s->file) ld_error("  defined in %s", s->file->path);
      if (file) ld_error("  defined in %s", file->path);
    }
    return;
  }

  if (s->state == SYM_SHARED) {
    /* an object definition beats the shared object one */
    if (bind != STB_WEAK) {
      s->state = state;
      s->file = file;
      s->isec = isec;
      s->value = value;
      s->size = size;
      s->bind = bind;
      s->type = type;
      s->visibility = visibility;
      s->is_weak = false;
      s->shared = NULL;
    }
    return;
  }
}

uint64_t sym_address(LdCtx *ctx, Symbol *s) {
  (void)ctx;
  switch (s->state) {
    case SYM_DEFINED:
      if (s->isec) return s->isec->addr + s->value;
      return s->value;
    case SYM_COMMON:
      return s->addr;
    case SYM_ABSOLUTE:
      return s->value;
    default:
      return 0;
  }
}

/* ------------------------------------------------------- common symbols */
void sym_allocate_commons(LdCtx *ctx) {
  InputSection *bss = NULL;
  uint64_t offset = 0;
  uint64_t align = 1;

  for (size_t i = 0; i < ctx->nsymbols; i++) {
    Symbol *s = ctx->symbols[i];
    if (s->state != SYM_COMMON) continue;
    if (s->value > align) align = s->value;
  }
  if (!align) align = 1;

  for (size_t i = 0; i < ctx->nsymbols; i++) {
    Symbol *s = ctx->symbols[i];
    if (s->state != SYM_COMMON) continue;

    if (!bss) {
      bss = xcalloc(1, sizeof(InputSection));
      bss->file = NULL;
      bss->index = 0;
      bss->name = ".bss";
      bss->type = SHT_NOBITS;
      bss->flags = SHF_ALLOC | SHF_WRITE;
      bss->align = align;
      bss->is_bss = true;
      bss->live = true;
    }
    uint64_t sym_align = s->value ? s->value : 1;
    if (sym_align > bss->align) bss->align = sym_align;
    offset = align_up(offset, sym_align);
    s->value = offset;
    s->isec = bss;
    s->state = SYM_DEFINED;
    offset += s->size;
  }

  if (bss) {
    bss->size = offset;
    ctx->extra = xrealloc(ctx->extra, sizeof(InputSection *) * (ctx->nextra + 1));
    ctx->extra[ctx->nextra++] = bss;
  }
}

/* -------------------------------------------------- undefined reporting */
void sym_report_undefined(LdCtx *ctx) {
  for (size_t i = 0; i < ctx->nsymbols; i++) {
    Symbol *s = ctx->symbols[i];
    if (s->state != SYM_UNDEFINED) continue;
    if (!s->referenced) continue;
    if (s->is_weak) continue;             /* weak undefined resolves to 0 */
    if (ctx->kind == OUT_SHARED && !ctx->opt.no_undefined) continue;
    if (s->visibility == STV_HIDDEN || s->visibility == STV_INTERNAL) {
      ld_error("hidden symbol '%s' is undefined", s->name);
      continue;
    }
    ld_error("undefined symbol '%s'", s->name);
  }
}
