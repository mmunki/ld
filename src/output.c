/* ============================================================================
 * ld -- ELF output
 * ----------------------------------------------------------------------------
 * Writes the finished image: the ELF header, the program header table, the
 * contents of every output section and finally the section header table.
 * ==========================================================================*/
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ld/ld.h"

/* ------------------------------------------------------------- symbol tab */
void symtab_fill(LdCtx *ctx) {
  if (ctx->opt.strip_all || !ctx->o_symtab) return;

  OutSection *symtab = ctx->o_symtab;
  Buffer data;
  buf_init(&data);
  buf_reserve(&data, (size_t)symtab->size);
  buf_zero(&data, (size_t)symtab->size);

  Elf64_Sym es;
  memset(&es, 0, sizeof(es));
  memcpy(data.data, &es, sizeof(es));

  size_t index = 1;
  for (size_t i = 0; i < ctx->nsymbols; i++) {
    Symbol *s = ctx->symbols[i];
    if (s->state != SYM_DEFINED && s->state != SYM_ABSOLUTE) continue;
    if (s->bind == STB_LOCAL) continue;

    memset(&es, 0, sizeof(es));
    es.st_name = strtab_find(&ctx->symstr, s->name);
    es.st_info = ELF64_ST_INFO(s->bind, s->type);
    es.st_other = s->visibility;
    es.st_size = s->size;
    es.st_value = (s->state == SYM_ABSOLUTE) ? s->value : sym_address(ctx, s);
    es.st_shndx = (s->state == SYM_ABSOLUTE) ? SHN_ABS
                : (s->isec && s->isec->osec ? (uint16_t)s->isec->osec->shndx
                                            : SHN_UNDEF);
    memcpy(data.data + index * sizeof(Elf64_Sym), &es, sizeof(es));
    index++;
  }

  symtab->data = data;
  symtab->sh_info = 1;   /* the first global symbol */
}

/* ------------------------------------------------------------------ write */
void output_write(LdCtx *ctx, const char *path) {
  OutSection *shstrtab = ctx->o_shstrtab_out;

  /* the section header links */
  for (size_t i = 0; i < ctx->nosecs; i++) {
    OutSection *o = ctx->osecs[i];
    if (!strcmp(o->name, ".dynsym")) o->sh_link = ctx->o_dynstr ? ctx->o_dynstr->shndx : 0;
    else if (!strcmp(o->name, ".hash") || !strcmp(o->name, ".gnu.hash"))
      o->sh_link = ctx->o_dynsym ? ctx->o_dynsym->shndx : 0;
    else if (!strcmp(o->name, ".rela.dyn")) {
      o->sh_link = ctx->o_dynsym ? ctx->o_dynsym->shndx : 0;
      o->sh_info = 0;
    } else if (!strcmp(o->name, ".rela.plt")) {
      o->sh_link = ctx->o_dynsym ? ctx->o_dynsym->shndx : 0;
      o->sh_info = ctx->o_plt ? ctx->o_plt->shndx : 0;
    } else if (!strcmp(o->name, ".dynamic")) {
      o->sh_link = ctx->o_dynstr ? ctx->o_dynstr->shndx : 0;
    } else if (!strcmp(o->name, ".symtab")) {
      for (size_t j = 0; j < ctx->nosecs; j++)
        if (!strcmp(ctx->osecs[j]->name, ".strtab")) o->sh_link = ctx->osecs[j]->shndx;
    }
  }

  /* the file offset of every section is known, the section header table
   * goes to the end of the file */
  uint64_t max_off = 0;
  for (size_t i = 0; i < ctx->nosecs; i++) {
    OutSection *o = ctx->osecs[i];
    if (!o->shndx) continue;
    uint64_t end = o->offset + (o->type == SHT_NOBITS ? 0 : o->size);
    if (end > max_off) max_off = end;
  }

  uint64_t shoff = align_up(max_off, 8);
  uint32_t shnum = 1;                     /* the null section comes first */
  for (size_t i = 0; i < ctx->nosecs; i++)
    if (ctx->osecs[i]->shndx) shnum++;
  uint32_t shstrndx = shstrtab ? shstrtab->shndx : 0;

  /* build the file */
  size_t total = (size_t)(shoff + (uint64_t)shnum * sizeof(Elf64_Shdr));
  uint8_t *image = xcalloc(total ? total : 1, 1);

  /* the section contents */
  for (size_t i = 0; i < ctx->nosecs; i++) {
    OutSection *o = ctx->osecs[i];
    if (!o->shndx) continue;
    if (o->type == SHT_NOBITS || !o->size) continue;
    if (o->offset + o->size > total) {
      ld_error("internal error: section %s does not fit into the output", o->name);
      continue;
    }
    memcpy(image + o->offset, o->data.data, (size_t)o->size);
  }

  /* the ELF header */
  Elf64_Ehdr eh;
  memset(&eh, 0, sizeof(eh));
  memcpy(eh.e_ident, "\177ELF", 4);
  eh.e_ident[EI_CLASS] = ELFCLASS64;
  eh.e_ident[EI_DATA] = ELFDATA2LSB;
  eh.e_ident[EI_VERSION] = EV_CURRENT;
  eh.e_ident[EI_OSABI] = ELFOSABI_NONE;   /* like the Android toolchain */
  eh.e_type = (ctx->kind == OUT_EXEC) ? ET_EXEC : ET_DYN;
  eh.e_machine = EM_AARCH64;
  eh.e_version = EV_CURRENT;
  eh.e_entry = ctx->entry_addr;
  eh.e_phoff = sizeof(Elf64_Ehdr);
  eh.e_shoff = shoff;
  eh.e_flags = 0;
  eh.e_ehsize = sizeof(Elf64_Ehdr);
  eh.e_phentsize = sizeof(Elf64_Phdr);
  eh.e_phnum = (uint16_t)ctx->nphdrs;
  eh.e_shentsize = sizeof(Elf64_Shdr);
  eh.e_shnum = (uint16_t)shnum;
  eh.e_shstrndx = (uint16_t)shstrndx;
  memcpy(image, &eh, sizeof(eh));

  /* the program headers */
  for (size_t i = 0; i < ctx->nphdrs; i++)
    memcpy(image + sizeof(Elf64_Ehdr) + i * sizeof(Elf64_Phdr), &ctx->phdrs[i],
           sizeof(Elf64_Phdr));

  /* the section header table, the first entry is the null section */
  size_t idx = 1;
  for (size_t i = 0; i < ctx->nosecs; i++) {
    OutSection *o = ctx->osecs[i];
    if (!o->shndx) continue;
    Elf64_Shdr sh;
    memset(&sh, 0, sizeof(sh));
    sh.sh_name = o->sh_name;
    sh.sh_type = o->type;
    sh.sh_flags = o->flags;
    sh.sh_addr = o->addr;
    sh.sh_offset = o->offset;
    sh.sh_size = o->size;
    sh.sh_link = o->sh_link;
    sh.sh_info = o->sh_info;
    sh.sh_addralign = o->align ? o->align : 1;
    sh.sh_entsize = o->entsize;
    memcpy(image + shoff + idx * sizeof(Elf64_Shdr), &sh, sizeof(sh));
    idx++;
  }

  /* The output is an executable image, it is created with the execute bit
   * set (the umask still applies). */
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0777);
  if (fd < 0) {
    ld_error("cannot create %s: %s", path, strerror(errno));
    free(image);
    return;
  }
  size_t written = 0;
  while (written < total) {
    ssize_t n = write(fd, image + written, total - written);
    if (n <= 0) {
      ld_error("cannot write %s: %s", path, strerror(errno));
      break;
    }
    written += (size_t)n;
  }
  close(fd);
  free(image);
}

/* -------------------------------------------------------------- map file */
void output_map(LdCtx *ctx, FILE *out) {
  fprintf(out, "Memory map of %s\n\n", ctx->opt.output ? ctx->opt.output : "a.out");
  for (size_t i = 0; i < ctx->nosecs; i++) {
    OutSection *o = ctx->osecs[i];
    if (!o->shndx) continue;
    fprintf(out, "  %-16s %016llx  %10llu  %s\n", o->name,
            (unsigned long long)o->addr, (unsigned long long)o->size,
            (o->flags & SHF_WRITE) ? "RW" :
            (o->flags & SHF_EXECINSTR) ? "RX" : "R");
  }
  fprintf(out, "\n");
  for (size_t i = 0; i < ctx->nphdrs; i++) {
    Elf64_Phdr *ph = &ctx->phdrs[i];
    fprintf(out, "  PHDR %-14s off=%016llx vaddr=%016llx filesz=%llu memsz=%llu\n",
            ph->p_type == PT_LOAD ? "LOAD" :
            ph->p_type == PT_DYNAMIC ? "DYNAMIC" :
            ph->p_type == PT_INTERP ? "INTERP" :
            ph->p_type == PT_PHDR ? "PHDR" :
            ph->p_type == PT_GNU_RELRO ? "GNU_RELRO" :
            ph->p_type == PT_GNU_STACK ? "GNU_STACK" : "other",
            (unsigned long long)ph->p_offset, (unsigned long long)ph->p_vaddr,
            (unsigned long long)ph->p_filesz, (unsigned long long)ph->p_memsz);
  }
}
