/* ============================================================================
 * ld -- address layout
 * ----------------------------------------------------------------------------
 * The layout produces three loadable segments:
 *
 *   R    read only data (headers, .interp, .dynsym, .rela.*, .rodata, ...)
 *   RX   executable code (.init, .plt, .text, .fini)
 *   RW   writable data (.init_array, .dynamic, .got, .data, .bss)
 *
 * The first pages of the writable segment are covered by RELRO, which is the
 * default on Android.  File offsets and virtual addresses are kept in sync
 * (vaddr = base + offset) so that every segment satisfies the ELF constraint
 * (p_vaddr - p_offset) % page_size == 0.
 * ==========================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ld/ld.h"

/* ------------------------------------------------------------ section rank */
static int section_rank(const char *name) {
  if (!strcmp(name, ".interp")) return 0;
  if (!strncmp(name, ".note", 5)) return 1;
  if (!strcmp(name, ".hash")) return 2;
  if (!strcmp(name, ".gnu.hash")) return 3;
  if (!strcmp(name, ".dynsym")) return 4;
  if (!strcmp(name, ".dynstr")) return 5;
  if (!strcmp(name, ".rela.dyn")) return 6;
  if (!strcmp(name, ".rela.plt")) return 7;
  if (!strcmp(name, ".rodata")) return 8;
  if (!strcmp(name, ".eh_frame_hdr")) return 9;
  if (!strcmp(name, ".eh_frame")) return 10;
  if (!strcmp(name, ".init")) return 11;
  if (!strcmp(name, ".plt")) return 12;
  if (!strcmp(name, ".text")) return 13;
  if (!strcmp(name, ".fini")) return 14;
  if (!strcmp(name, ".init_array")) return 15;
  if (!strcmp(name, ".fini_array")) return 16;
  if (!strcmp(name, ".data.rel.ro")) return 17;
  if (!strcmp(name, ".dynamic")) return 18;
  if (!strcmp(name, ".got")) return 19;
  if (!strcmp(name, ".got.plt")) return 20;
  if (!strcmp(name, ".data")) return 21;
  if (!strcmp(name, ".bss")) return 22;
  return 23;
}

static int segment_of(const OutSection *o) {
  if (o->flags & SHF_WRITE) return 2;      /* RW */
  if (o->flags & SHF_EXECINSTR) return 1;  /* RX */
  return 0;                                /* R  */
}

static void sort_sections(LdCtx *ctx) {
  /* stable insertion sort by (segment, rank) */
  for (size_t i = 1; i < ctx->nosecs; i++) {
    OutSection *cur = ctx->osecs[i];
    int crank = section_rank(cur->name);
    int cseg = segment_of(cur);
    size_t j = i;
    while (j > 0) {
      OutSection *prev = ctx->osecs[j - 1];
      int prank = section_rank(prev->name);
      int pseg = segment_of(prev);
      if (pseg < cseg || (pseg == cseg && prank <= crank)) break;
      ctx->osecs[j] = prev;
      j--;
    }
    ctx->osecs[j] = cur;
  }
}

/* ---------------------------------------------------------- segment setup */
typedef struct {
  OutSection **members;
  size_t       nmembers;
  uint64_t     offset;   /* file offset of the segment            */
  uint64_t     filesz;
  uint64_t     memsz;
  uint32_t     flags;    /* PF_*                                   */
  bool         used;
} Segment;

static void segment_add(Segment *s, OutSection *o) {
  s->members = xrealloc(s->members, sizeof(OutSection *) * (s->nmembers + 1));
  s->members[s->nmembers++] = o;
  s->used = true;
}

/* --------------------------------------------------------------- the layout */
void layout(LdCtx *ctx) {
  const uint64_t page = ctx->opt.page_size;

  sort_sections(ctx);

  /* Assign the section header indices.  Sections that carry no information
   * at all are dropped from the output. */
  {
    uint32_t shndx = 1;
    for (size_t i = 0; i < ctx->nosecs; i++) {
      OutSection *o = ctx->osecs[i];
      if (!o->size && !o->nmembers) {
        o->shndx = 0;
        continue;
      }
      o->shndx = shndx++;
    }
  }

  Segment segs[3] = { { 0 } };
  segs[0].flags = PF_R;
  segs[1].flags = PF_R | PF_X;
  segs[2].flags = PF_R | PF_W;

  for (size_t i = 0; i < ctx->nosecs; i++) {
    OutSection *o = ctx->osecs[i];
    if (!o->shndx) continue;
    if (o->flags & SHF_ALLOC) segment_add(&segs[segment_of(o)], o);
  }

  /* A segment is only worth an entry in the program header table when it
   * carries something. */
  bool seg_content[3] = { false, false, false };
  for (int i = 0; i < 3; i++) {
    for (size_t m = 0; m < segs[i].nmembers; m++)
      if (segs[i].members[m]->size) seg_content[i] = true;
  }

  /* the RELRO region covers the writable sections up to .got.plt */
  bool relro_content = false;
  for (size_t i = 0; i < ctx->nosecs; i++) {
    OutSection *o = ctx->osecs[i];
    if (!(o->flags & SHF_WRITE) || !o->shndx) continue;
    if (!strcmp(o->name, ".data") || !strcmp(o->name, ".bss")) continue;
    if (o->size) relro_content = true;
  }

  /* How many program headers do we need? */
  size_t nphdrs = 0;
  bool need_phdr = (ctx->kind == OUT_PIE && ctx->has_dynamic);
  bool need_interp = ctx->o_interp && ctx->o_interp->shndx &&
                      ctx->kind != OUT_SHARED;
  bool need_dynamic = ctx->o_dynamic && ctx->o_dynamic->shndx;
  bool need_relro = ctx->opt.z_relro && ctx->has_dynamic && relro_content;

  if (need_phdr) nphdrs++;
  if (need_interp) nphdrs++;
  for (int i = 0; i < 3; i++) if (seg_content[i]) nphdrs++;
  if (need_dynamic) nphdrs++;
  if (need_relro) nphdrs++;
  nphdrs++;   /* PT_GNU_STACK */

  ctx->phdrs = xcalloc(nphdrs, sizeof(Elf64_Phdr));
  ctx->nphdrs = nphdrs;

  /* The ELF header and the program header table live at the start of the
   * first loadable segment. */
  uint64_t header_size = sizeof(Elf64_Ehdr) +
                         (uint64_t)nphdrs * sizeof(Elf64_Phdr);

  uint64_t file_off = 0;

  for (int i = 0; i < 3; i++) {
    Segment *s = &segs[i];
    if (!s->used) continue;

    uint64_t place = file_off;
    if (i == 0) {
      place = header_size;              /* the headers are part of segment 0 */
    } else {
      place = align_up(place, page);    /* every segment starts on a page */
    }
    /* the first segment starts at the very beginning of the file, it holds
     * the ELF header and the program header table */
    s->offset = (i == 0) ? 0 : place;
    file_off = place;
    uint64_t mem_end = place;

    for (size_t m = 0; m < s->nmembers; m++) {
      OutSection *o = s->members[m];
      if (!o->shndx) continue;
      uint64_t align = o->align ? o->align : 1;
      if (align > page) align = page;   /* huge alignments are not honoured */
      place = align_up(place, align);
      o->offset = place;
      o->addr = ctx->base_addr + place;
      if (o->type == SHT_NOBITS) {
        mem_end = place + o->size;      /* only reserves memory */
      } else {
        place += o->size;
        mem_end = place;
      }
    }
    s->filesz = place - s->offset;
    s->memsz = mem_end - s->offset;
    file_off = place;
  }

  /* the sections that are not loaded into memory go to the end of the file */
  for (size_t i = 0; i < ctx->nosecs; i++) {
    OutSection *o = ctx->osecs[i];
    if ((o->flags & SHF_ALLOC) || !o->shndx) continue;
    file_off = align_up(file_off, 8);
    o->offset = file_off;
    o->addr = 0;
    file_off += o->size;
  }

  /* the address of every input section is now known */
  for (size_t i = 0; i < ctx->nosecs; i++) {
    OutSection *o = ctx->osecs[i];
    for (size_t m = 0; m < o->nmembers; m++) {
      InputSection *is = o->members[m];
      is->addr = o->addr + is->out_off;
    }
  }

  /* build the program header table */
  size_t p = 0;
  ctx->phdr_off = sizeof(Elf64_Ehdr);

  if (need_phdr) {
    Elf64_Phdr *ph = &ctx->phdrs[p++];
    ph->p_type = PT_PHDR;
    ph->p_flags = PF_R;
    ph->p_offset = ctx->phdr_off;
    ph->p_vaddr = ctx->base_addr + ctx->phdr_off;
    ph->p_paddr = ph->p_vaddr;
    ph->p_filesz = (uint64_t)nphdrs * sizeof(Elf64_Phdr);
    ph->p_memsz = ph->p_filesz;
    ph->p_align = 8;
  }
  if (need_interp) {
    Elf64_Phdr *ph = &ctx->phdrs[p++];
    ph->p_type = PT_INTERP;
    ph->p_flags = PF_R;
    ph->p_offset = ctx->o_interp->offset;
    ph->p_vaddr = ctx->o_interp->addr;
    ph->p_paddr = ph->p_vaddr;
    ph->p_filesz = ctx->o_interp->size;
    ph->p_memsz = ph->p_filesz;
    ph->p_align = 1;
  }
  for (int i = 0; i < 3; i++) {
    if (!segs[i].used || !seg_content[i]) continue;
    Elf64_Phdr *ph = &ctx->phdrs[p++];
    ph->p_type = PT_LOAD;
    ph->p_flags = segs[i].flags;
    ph->p_offset = segs[i].offset;
    ph->p_vaddr = ctx->base_addr + segs[i].offset;
    ph->p_paddr = ph->p_vaddr;
    ph->p_filesz = segs[i].filesz;
    ph->p_memsz = segs[i].memsz;
    ph->p_align = page;
  }
  if (need_dynamic) {
    Elf64_Phdr *ph = &ctx->phdrs[p++];
    ph->p_type = PT_DYNAMIC;
    ph->p_flags = PF_R | PF_W;
    ph->p_offset = ctx->o_dynamic->offset;
    ph->p_vaddr = ctx->o_dynamic->addr;
    ph->p_paddr = ph->p_vaddr;
    ph->p_filesz = ctx->o_dynamic->size;
    ph->p_memsz = ph->p_filesz;
    ph->p_align = 8;
  }
  if (need_relro) {
    /* the RELRO region starts at the first writable section and ends at the
     * end of .got.plt; .data and .bss stay writable */
    uint64_t start = 0, end = 0;
    for (size_t i = 0; i < ctx->nosecs; i++) {
      OutSection *o = ctx->osecs[i];
      if (!(o->flags & SHF_WRITE) || !o->shndx) continue;
      if (!strcmp(o->name, ".data") || !strcmp(o->name, ".bss")) continue;
      if (!start) start = o->addr;
      end = o->addr + o->size;
    }
    if (start && end > start) {
      ctx->relro_start = start;
      ctx->relro_size = end - start;
      Elf64_Phdr *ph = &ctx->phdrs[p++];
      ph->p_type = PT_GNU_RELRO;
      ph->p_flags = PF_R;
      ph->p_offset = start - ctx->base_addr;
      ph->p_vaddr = start;
      ph->p_paddr = start;
      ph->p_filesz = ctx->relro_size;
      ph->p_memsz = ctx->relro_size;
      ph->p_align = 1;
    }
  }
  {
    Elf64_Phdr *ph = &ctx->phdrs[p++];
    ph->p_type = PT_GNU_STACK;
    ph->p_flags = PF_R | PF_W | (ctx->opt.z_execstack ? PF_X : 0);
    ph->p_offset = 0;
    ph->p_vaddr = 0;
    ph->p_paddr = 0;
    ph->p_filesz = 0;
    ph->p_memsz = 0;
    ph->p_align = 0x10;
  }
}
