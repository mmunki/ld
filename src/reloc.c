/* ============================================================================
 * ld -- relocation processing for AArch64
 * ----------------------------------------------------------------------------
 * Two passes are performed:
 *
 *   reloc_scan()   walks every relocation, resolves the symbol and decides
 *                  which GOT slots, PLT entries, dynamic symbols and dynamic
 *                  relocations are needed.  It runs before the layout, so all
 *                  sizes are known when the addresses are assigned.
 *
 *   reloc_apply()  computes the final values and patches the instructions and
 *                  data in the output sections.  It runs after the layout.
 *
 * The value computation follows the AArch64 ELF ABI:
 *
 *   value  = S + A                 absolute relocations
 *   value  = S + A - P             PC relative relocations
 *   value  = page(S + A) - page(P) page relative relocations (ADRP)
 * ==========================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ld/ld.h"

/* ------------------------------------------------------------- primitives */
static uint32_t read_u32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static void write_u16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}

static void write_u32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static void write_u64(uint8_t *p, uint64_t v) {
  for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static uint64_t page_of(uint64_t v) { return v & ~((uint64_t)0xfff); }

/* ------------------------------------------------------------ instruction patching */

/* ADRP / ADR: the 21 bit immediate is split into immlo (bits 30:29) and
 * immhi (bits 23:5). */
static void patch_adr(uint8_t *loc, int64_t delta) {
  uint32_t inst = read_u32(loc);
  inst &= ~0x63ffffe0u;
  inst |= (uint32_t)((delta & 3) << 29);
  inst |= (uint32_t)(((delta >> 2) & 0x7ffff) << 5);
  write_u32(loc, inst);
}

/* ADD / LDST immediate field, bits 21:10 */
static void patch_imm12(uint8_t *loc, uint64_t value) {
  uint32_t inst = read_u32(loc);
  inst &= ~0x003ffc00u;
  inst |= (uint32_t)((value & 0xfff) << 10);
  write_u32(loc, inst);
}

/* B / BL: 26 bit immediate in bits 25:0 */
static void patch_branch26(uint8_t *loc, int64_t delta) {
  uint32_t inst = read_u32(loc);
  inst &= ~0x03ffffffu;
  inst |= (uint32_t)((delta >> 2) & 0x03ffffff);
  write_u32(loc, inst);
}

/* B.cond / LDR literal: 19 bit immediate in bits 23:5 */
static void patch_branch19(uint8_t *loc, int64_t delta) {
  uint32_t inst = read_u32(loc);
  inst &= ~0x00ffffe0u;
  inst |= (uint32_t)(((delta >> 2) & 0x7ffff) << 5);
  write_u32(loc, inst);
}

/* TBZ / TBNZ: 14 bit immediate in bits 23:5 */
static void patch_branch14(uint8_t *loc, int64_t delta) {
  uint32_t inst = read_u32(loc);
  inst &= ~0x007fffe0u;
  inst |= (uint32_t)(((delta >> 2) & 0x3fff) << 5);
  write_u32(loc, inst);
}

/* MOVZ / MOVK / MOVN: 16 bit immediate in bits 20:5 */
static void patch_movw(uint8_t *loc, uint64_t imm16) {
  uint32_t inst = read_u32(loc);
  inst &= ~0x001fffe0u;
  inst |= (uint32_t)((imm16 & 0xffff) << 5);
  write_u32(loc, inst);
}

/* ------------------------------------------------------- relocation kinds */
static bool reloc_is_got(uint32_t type) {
  switch (type) {
    case R_AARCH64_ADR_GOT_PAGE:
    case R_AARCH64_LD64_GOT_LO12_NC:
    case R_AARCH64_LD64_GOTPAGE_LO15:
    case R_AARCH64_LD64_GOTOFF_LO15:
    case R_AARCH64_GOTPCREL32:
    case R_AARCH64_GOTREL64:
    case R_AARCH64_GOTREL32:
    case R_AARCH64_GOT_LD_PREL19:
    case R_AARCH64_MOVW_GOTOFF_G0:
    case R_AARCH64_MOVW_GOTOFF_G0_NC:
    case R_AARCH64_MOVW_GOTOFF_G1:
    case R_AARCH64_MOVW_GOTOFF_G1_NC:
    case R_AARCH64_MOVW_GOTOFF_G2:
    case R_AARCH64_MOVW_GOTOFF_G2_NC:
    case R_AARCH64_MOVW_GOTOFF_G3:
      return true;
    default:
      return false;
  }
}

static bool reloc_is_call(uint32_t type) {
  return type == R_AARCH64_CALL26 || type == R_AARCH64_JUMP26 ||
         type == R_AARCH64_PLT32;
}

static bool reloc_is_absolute(uint32_t type) {
  return type == R_AARCH64_ABS64 || type == R_AARCH64_ABS32 ||
         type == R_AARCH64_ABS16;
}

static bool reloc_is_pc_relative(uint32_t type) {
  switch (type) {
    case R_AARCH64_PREL64:
    case R_AARCH64_PREL32:
    case R_AARCH64_PREL16:
    case R_AARCH64_ADR_PREL_LO21:
    case R_AARCH64_ADR_PREL_PG_HI21:
    case R_AARCH64_ADR_PREL_PG_HI21_NC:
    case R_AARCH64_CALL26:
    case R_AARCH64_JUMP26:
    case R_AARCH64_PLT32:
    case R_AARCH64_CONDBR19:
    case R_AARCH64_LD_PREL_LO19:
    case R_AARCH64_TSTBR14:
    case R_AARCH64_MOVW_PREL_G0:
    case R_AARCH64_MOVW_PREL_G0_NC:
    case R_AARCH64_MOVW_PREL_G1:
    case R_AARCH64_MOVW_PREL_G1_NC:
    case R_AARCH64_MOVW_PREL_G2:
    case R_AARCH64_MOVW_PREL_G2_NC:
    case R_AARCH64_MOVW_PREL_G3:
      return true;
    default:
      return false;
  }
}

/* --------------------------------------------------------------- symbols */
static void resolve_symbol(LdCtx *ctx, InputFile *file, Relocation *r) {
  if (r->sym_index >= file->nsymbols) {
    ld_error("%s: relocation references symbol index %u out of range",
             file->path, r->sym_index);
    r->sym = NULL;
    return;
  }
  const Elf64_Sym *es = &file->symtab[r->sym_index];
  const char *name = file->strtab + es->st_name;
  uint8_t bind = ELF64_ST_BIND(es->st_info);

  if (bind == STB_LOCAL) {
    r->sym_name = name;
    r->sym = NULL;
    if (es->st_shndx == SHN_ABS || es->st_shndx == SHN_UNDEF ||
        es->st_shndx >= file->nsections || !file->sections[es->st_shndx]) {
      r->sym_is_abs = true;
      r->sym_value = (es->st_shndx == SHN_ABS) ? es->st_value : 0;
      r->sym_isec = NULL;
    } else {
      r->sym_isec = file->sections[es->st_shndx];
      r->sym_value = es->st_value;
      r->sym_is_abs = false;
    }
    return;
  }

  r->sym = sym_get(ctx, name);
  r->sym_name = r->sym->name;
  r->sym->referenced = true;
}

/* Is the symbol defined inside this output file? */
static bool symbol_is_local(Symbol *s) {
  if (!s) return true;
  if (s->visibility != STV_DEFAULT) return true;
  if (s->state == SYM_SHARED || s->state == SYM_UNDEFINED) return false;
  return false;
}

/* Address of the symbol as it is known at link time. */
uint64_t reloc_symbol_value(LdCtx *ctx, const Relocation *r) {
  if (r->sym) return sym_address(ctx, r->sym);
  if (r->sym_isec) return r->sym_isec->addr + r->sym_value;
  return r->sym_value;
}

/* ------------------------------------------------------------------ scan */
void reloc_scan(LdCtx *ctx) {
  const bool dynamic = ctx->has_dynamic;

  for (size_t f = 0; f < ctx->nobj; f++) {
    InputFile *file = ctx->objects[f];
    for (size_t i = 0; i < file->nsections; i++) {
      InputSection *is = file->sections[i];
      if (!is || !is->live || !is->nrels) continue;

      for (size_t r = 0; r < is->nrels; r++) {
        Relocation *rel = &is->rels[r];
        resolve_symbol(ctx, file, rel);
        if (!rel->sym && !rel->sym_isec && !rel->sym_is_abs &&
            rel->sym_index != 0) {
          ld_error("%s: cannot resolve symbol index %u", file->path,
                   rel->sym_index);
          continue;
        }
        rel->resolved = true;

        Symbol *s = rel->sym;
        if (s && s->state == SYM_SHARED && s->shared)
          s->shared->is_needed = true;
        bool defined_here = false;
        if (s) {
          if (s->state == SYM_DEFINED || s->state == SYM_COMMON ||
              s->state == SYM_ABSOLUTE)
            defined_here = true;
        } else {
          defined_here = true;   /* local symbol */
        }

        if (reloc_is_got(rel->type)) {
          if (!s) {
            ld_error("%s: GOT relocation against a local symbol", file->path);
            continue;
          }
          GotKind kind = symbol_is_local(s) ? GOT_LOCAL : GOT_GLOBAL;
          got_add(ctx, s, kind);
          continue;
        }

        if (reloc_is_call(rel->type)) {
          if (!defined_here) {
            if (!dynamic) {
              ld_error("undefined symbol '%s' referenced by %s",
                       rel->sym_name, file->path);
              continue;
            }
            plt_add(ctx, s);
          }
          continue;
        }

        if (reloc_is_absolute(rel->type) && dynamic) {
          if (!defined_here) {
            /* resolved by the dynamic linker: GLOB_DAT against the import */
            dynsym_add(ctx, s);
            dynreloc_add(ctx, DYN_GLOB_DAT, s, rel, rel->addend, is,
                         rel->offset);
          } else if (symbol_is_local(s)) {
            dynreloc_add(ctx, DYN_RELATIVE, s, rel, rel->addend, is,
                         rel->offset);
          } else {
            dynsym_add(ctx, s);
            dynreloc_add(ctx, DYN_GLOB_DAT, s, rel, rel->addend, is,
                         rel->offset);
          }
          continue;
        }

        if (!defined_here && reloc_is_pc_relative(rel->type) && !s->is_weak) {
          ld_error("%s: relocation against undefined symbol '%s' "
                   "(recompile with -fPIC)", file->path, rel->sym_name);
        }
      }
    }
  }
}

/* ----------------------------------------------------------------- apply */
static void check_overflow(LdCtx *ctx, Relocation *rel, int64_t value, int bits) {
  (void)ctx;
  int64_t min = -((int64_t)1 << (bits - 1));
  int64_t max = ((int64_t)1 << (bits - 1)) - 1;
  if (value < min || value > max)
    ld_error("relocation %u against '%s' overflows a %d bit field",
             rel->type, rel->sym_name ? rel->sym_name : "", bits);
}

static uint64_t got_address(LdCtx *ctx, Symbol *s) {
  if (!s || s->got_index == (uint32_t)-1) return 0;
  return ctx->got[s->got_index].addr;
}

static uint64_t plt_address(LdCtx *ctx, Symbol *s) {
  if (!s || s->plt_index == (uint32_t)-1) return 0;
  return ctx->plt[s->plt_index].addr;
}

void reloc_apply(LdCtx *ctx) {
  for (size_t f = 0; f < ctx->nobj; f++) {
    InputFile *file = ctx->objects[f];
    for (size_t i = 0; i < file->nsections; i++) {
      InputSection *is = file->sections[i];
      if (!is || !is->live || !is->nrels || !is->osec) continue;
      if (is->osec->type == SHT_NOBITS) continue;

      uint8_t *base = is->osec->data.data + is->out_off;

      for (size_t r = 0; r < is->nrels; r++) {
        Relocation *rel = &is->rels[r];
        if (!rel->resolved) continue;

        uint8_t *loc = base + rel->offset;
        uint64_t p = is->addr + rel->offset;
        int64_t a = rel->addend;
        uint64_t s_val = reloc_symbol_value(ctx, rel);
        Symbol *sym = rel->sym;
        bool defined_here = sym ? (sym->state == SYM_DEFINED ||
                                    sym->state == SYM_COMMON ||
                                    sym->state == SYM_ABSOLUTE)
                                 : true;

        switch (rel->type) {
          case R_AARCH64_ABS64:
            write_u64(loc, s_val + (uint64_t)a);
            break;
          case R_AARCH64_ABS32:
            write_u32(loc, (uint32_t)(s_val + (uint64_t)a));
            break;
          case R_AARCH64_ABS16:
            write_u16(loc, (uint16_t)(s_val + (uint64_t)a));
            break;
          case R_AARCH64_PREL64:
            write_u64(loc, s_val + (uint64_t)a - p);
            break;
          case R_AARCH64_PREL32:
            write_u32(loc, (uint32_t)(s_val + (uint64_t)a - p));
            break;
          case R_AARCH64_PREL16:
            write_u16(loc, (uint16_t)(s_val + (uint64_t)a - p));
            break;

          case R_AARCH64_ADR_PREL_LO21: {
            int64_t delta = (int64_t)(s_val + (uint64_t)a - p);
            check_overflow(ctx, rel, delta, 21);
            patch_adr(loc, delta);
            break;
          }
          case R_AARCH64_ADR_PREL_PG_HI21:
          case R_AARCH64_ADR_PREL_PG_HI21_NC: {
            /* the immediate of ADRP counts pages */
            int64_t delta = (int64_t)(page_of(s_val + (uint64_t)a) - page_of(p));
            check_overflow(ctx, rel, delta >> 12, 21);
            patch_adr(loc, delta >> 12);
            break;
          }
          case R_AARCH64_ADD_ABS_LO12_NC:
            patch_imm12(loc, (s_val + (uint64_t)a) & 0xfff);
            break;
          /* The scaled load immediates hold the offset inside the page,
           * so only eleven bits of the scaled value are used. */
          case R_AARCH64_LDST8_ABS_LO12_NC:
            patch_imm12(loc, (s_val + (uint64_t)a) & 0x1ff);
            break;
          case R_AARCH64_LDST16_ABS_LO12_NC:
            patch_imm12(loc, ((s_val + (uint64_t)a) >> 1) & 0x1ff);
            break;
          case R_AARCH64_LDST32_ABS_LO12_NC:
            patch_imm12(loc, ((s_val + (uint64_t)a) >> 2) & 0x1ff);
            break;
          case R_AARCH64_LDST64_ABS_LO12_NC:
            patch_imm12(loc, ((s_val + (uint64_t)a) >> 3) & 0x1ff);
            break;
          case R_AARCH64_LDST128_ABS_LO12_NC:
            patch_imm12(loc, ((s_val + (uint64_t)a) >> 4) & 0x1ff);
            break;

          case R_AARCH64_CALL26:
          case R_AARCH64_JUMP26: {
            uint64_t target = defined_here ? (s_val + (uint64_t)a)
                                           : plt_address(ctx, sym);
            int64_t delta = (int64_t)(target - p);
            check_overflow(ctx, rel, delta, 28);
            patch_branch26(loc, delta);
            break;
          }
          case R_AARCH64_PLT32: {
            uint64_t target = defined_here ? (s_val + (uint64_t)a)
                                           : plt_address(ctx, sym);
            int64_t delta = (int64_t)(target - p);
            check_overflow(ctx, rel, delta, 32);
            write_u32(loc, (uint32_t)delta);
            break;
          }
          case R_AARCH64_CONDBR19:
          case R_AARCH64_LD_PREL_LO19: {
            int64_t delta = (int64_t)(s_val + (uint64_t)a - p);
            check_overflow(ctx, rel, delta, 21);
            patch_branch19(loc, delta);
            break;
          }
          case R_AARCH64_TSTBR14: {
            int64_t delta = (int64_t)(s_val + (uint64_t)a - p);
            check_overflow(ctx, rel, delta, 16);
            patch_branch14(loc, delta);
            break;
          }

          case R_AARCH64_MOVW_UABS_G0:
          case R_AARCH64_MOVW_UABS_G0_NC:
            patch_movw(loc, (s_val + (uint64_t)a) & 0xffff);
            break;
          case R_AARCH64_MOVW_UABS_G1:
          case R_AARCH64_MOVW_UABS_G1_NC:
            patch_movw(loc, ((s_val + (uint64_t)a) >> 16) & 0xffff);
            break;
          case R_AARCH64_MOVW_UABS_G2:
          case R_AARCH64_MOVW_UABS_G2_NC:
            patch_movw(loc, ((s_val + (uint64_t)a) >> 32) & 0xffff);
            break;
          case R_AARCH64_MOVW_UABS_G3:
            patch_movw(loc, ((s_val + (uint64_t)a) >> 48) & 0xffff);
            break;
          case R_AARCH64_MOVW_SABS_G0:
            patch_movw(loc, (uint64_t)((int64_t)s_val + a) & 0xffff);
            break;
          case R_AARCH64_MOVW_SABS_G1:
            patch_movw(loc, (uint64_t)(((int64_t)s_val + a) >> 16) & 0xffff);
            break;
          case R_AARCH64_MOVW_SABS_G2:
            patch_movw(loc, (uint64_t)(((int64_t)s_val + a) >> 32) & 0xffff);
            break;
          case R_AARCH64_MOVW_PREL_G0:
          case R_AARCH64_MOVW_PREL_G0_NC:
            patch_movw(loc, (uint64_t)((int64_t)(s_val + (uint64_t)a - p)) & 0xffff);
            break;
          case R_AARCH64_MOVW_PREL_G1:
          case R_AARCH64_MOVW_PREL_G1_NC:
            patch_movw(loc, (uint64_t)(((int64_t)(s_val + (uint64_t)a - p)) >> 16) & 0xffff);
            break;
          case R_AARCH64_MOVW_PREL_G2:
          case R_AARCH64_MOVW_PREL_G2_NC:
            patch_movw(loc, (uint64_t)(((int64_t)(s_val + (uint64_t)a - p)) >> 32) & 0xffff);
            break;
          case R_AARCH64_MOVW_PREL_G3:
            patch_movw(loc, (uint64_t)(((int64_t)(s_val + (uint64_t)a - p)) >> 48) & 0xffff);
            break;

          /* ------------------------------------------------------- GOT */
          case R_AARCH64_ADR_GOT_PAGE: {
            uint64_t g = got_address(ctx, sym) + (uint64_t)a;
            int64_t delta = (int64_t)(page_of(g) - page_of(p));
            check_overflow(ctx, rel, delta >> 12, 21);
            patch_adr(loc, delta >> 12);
            break;
          }
          case R_AARCH64_LD64_GOT_LO12_NC: {
            uint64_t g = got_address(ctx, sym) + (uint64_t)a;
            patch_imm12(loc, (g >> 3) & 0x1ff);
            break;
          }
          case R_AARCH64_GOTPCREL32: {
            uint64_t g = got_address(ctx, sym) + (uint64_t)a;
            write_u32(loc, (uint32_t)(g - p));
            break;
          }
          case R_AARCH64_GOTREL64:
            write_u64(loc, s_val - ctx->o_got->addr + (uint64_t)a);
            break;
          case R_AARCH64_GOTREL32:
            write_u32(loc, (uint32_t)(s_val - ctx->o_got->addr + (uint64_t)a));
            break;

          default:
            if (rel->type >= R_AARCH64_TLSGD_ADR_PREL21 &&
                rel->type <= R_AARCH64_TLSDESC_CALL) {
              ld_error("thread local storage is not supported by this linker "
                       "yet (relocation %u against '%s')", rel->type,
                       rel->sym_name ? rel->sym_name : "");
            } else {
              ld_error("unsupported relocation type %u against '%s'",
                       rel->type, rel->sym_name ? rel->sym_name : "");
            }
            break;
        }
      }
    }
  }
}
