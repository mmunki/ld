/* ============================================================================
 * ld -- input file handling
 * ----------------------------------------------------------------------------
 * Reads the three kinds of input the linker understands:
 *
 *   .o    relocatable objects (ET_REL, ELF64, EM_AARCH64)
 *   .a    static archives (ar format, members pulled in on demand)
 *   .so   shared objects (ET_DYN), used to resolve undefined symbols
 * ==========================================================================*/
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ld/ld.h"

/* --------------------------------------------------------------- mappings */
typedef struct {
  uint8_t *addr;
  size_t   size;
} Mapping;

static Mapping *g_mappings = NULL;
static size_t   g_nmappings = 0;

static uint8_t *map_file(const char *path, size_t *out_size) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    ld_error("cannot open %s: %s", path, strerror(errno));
    return NULL;
  }
  struct stat st;
  if (fstat(fd, &st) != 0) {
    ld_error("cannot stat %s: %s", path, strerror(errno));
    close(fd);
    return NULL;
  }
  if (st.st_size == 0) {
    close(fd);
    ld_error("%s is empty", path);
    return NULL;
  }
  void *addr = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);
  if (addr == MAP_FAILED) {
    ld_error("cannot map %s: %s", path, strerror(errno));
    return NULL;
  }
  g_mappings = xrealloc(g_mappings, sizeof(Mapping) * (g_nmappings + 1));
  g_mappings[g_nmappings].addr = addr;
  g_mappings[g_nmappings].size = (size_t)st.st_size;
  g_nmappings++;
  *out_size = (size_t)st.st_size;
  return addr;
}

/* ------------------------------------------------------------ ELF helpers */
static int check_elf(const uint8_t *image, size_t size, const char *path) {
  if (size < sizeof(Elf64_Ehdr)) {
    ld_error("%s: file is too small to be an ELF file", path);
    return -1;
  }
  const Elf64_Ehdr *eh = (const Elf64_Ehdr *)image;
  if (memcmp(eh->e_ident, "\177ELF", 4) != 0) {
    ld_error("%s: not an ELF file", path);
    return -1;
  }
  if (eh->e_ident[EI_CLASS] != ELFCLASS64) {
    ld_error("%s: not an ELF64 file", path);
    return -1;
  }
  if (eh->e_ident[EI_DATA] != ELFDATA2LSB) {
    ld_error("%s: not a little endian ELF file", path);
    return -1;
  }
  if (eh->e_machine != EM_AARCH64) {
    ld_error("%s: incompatible architecture (e_machine=%u), "
             "only AArch64 is supported", path, eh->e_machine);
    return -1;
  }
  return 0;
}

static int in_range(uint64_t offset, uint64_t size, size_t total) {
  return offset <= total && size <= total - offset;
}

/* ------------------------------------------------------- section handling */
static bool section_is_alloc(uint32_t type) {
  switch (type) {
    case SHT_PROGBITS:
    case SHT_NOBITS:
    case SHT_INIT_ARRAY:
    case SHT_FINI_ARRAY:
    case SHT_PREINIT_ARRAY:
    case SHT_NOTE:
      return true;
    default:
      return false;
  }
}

static bool section_is_discarded(const char *name, uint32_t type) {
  if (!strcmp(name, ".comment")) return true;
  if (!strcmp(name, ".note.GNU-stack")) return true;
  if (!strcmp(name, ".ARM.attributes")) return true;
  if (!strcmp(name, ".aarch64.attributes")) return true;
  if (type == SHT_GROUP) return true;
  if (type == SHT_SYMTAB_SHNDX) return true;
  return false;
}

/* --------------------------------------------------------------- objects */
static void parse_relocations(LdCtx *ctx, InputFile *file, InputSection *target,
                              const Elf64_Shdr *shdr) {
  (void)ctx;
  const char *name = file->shstrtab + shdr->sh_name;
  if (shdr->sh_type != SHT_RELA) {
    ld_error("%s: %s: unsupported relocation section type %u "
             "(only RELA is supported on AArch64)", file->path, name,
             shdr->sh_type);
    return;
  }
  if (shdr->sh_entsize && shdr->sh_entsize != sizeof(Elf64_Rela)) {
    ld_error("%s: %s: unexpected relocation entry size", file->path, name);
    return;
  }
  if (!in_range(shdr->sh_offset, shdr->sh_size, file->size)) {
    ld_error("%s: %s: relocation section is out of bounds", file->path, name);
    return;
  }
  const Elf64_Rela *rela = (const Elf64_Rela *)(file->image + shdr->sh_offset);
  size_t n = (size_t)(shdr->sh_size / sizeof(Elf64_Rela));

  target->rels = xcalloc(n ? n : 1, sizeof(Relocation));
  target->nrels = 0;

  for (size_t i = 0; i < n; i++) {
    Relocation *r = &target->rels[target->nrels];
    r->offset = rela[i].r_offset;
    r->type = ELF64_R_TYPE(rela[i].r_info);
    r->sym_index = ELF64_R_SYM(rela[i].r_info);
    r->addend = rela[i].r_addend;
    r->sym = NULL;
    r->sym_isec = NULL;
    r->sym_value = 0;
    r->sym_name = "";
    r->sym_is_abs = false;
    r->resolved = false;
    if (r->type == R_AARCH64_NONE) continue;
    target->nrels++;
  }
}

static void parse_comdat_groups(LdCtx *ctx, InputFile *file) {
  for (size_t i = 0; i < file->nsections; i++) {
    const Elf64_Shdr *sh = &file->shdrs[i];
    if (sh->sh_type != SHT_GROUP) continue;

    const Elf64_Word *members = (const Elf64_Word *)(file->image + sh->sh_offset);
    size_t nmembers = (size_t)(sh->sh_size / sizeof(Elf64_Word));

    ComdatGroup *g = xcalloc(1, sizeof(ComdatGroup));
    g->file = file;
    g->members = xcalloc(nmembers ? nmembers : 1, sizeof(InputSection *));
    g->nmembers = 0;

    /* the group leader symbol carries the signature */
    if (file->symtab && sh->sh_info < file->nsymbols) {
      const Elf64_Sym *leader = &file->symtab[sh->sh_info];
      g->signature = xstrdup(file->strtab + leader->st_name);
    } else {
      g->signature = xasprintf("%s.group.%zu", file->path, i);
    }

    for (size_t m = 0; m < nmembers; m++) {
      uint32_t idx = members[m];
      if (idx >= file->nsections) continue;
      if (!file->sections[idx]) continue;
      file->sections[idx]->group = g;
      g->members[g->nmembers++] = file->sections[idx];
    }
    (void)ctx;
  }
}

static int load_object(LdCtx *ctx, const char *path, uint8_t *image, size_t size) {
  if (check_elf(image, size, path) != 0) return -1;

  InputFile *file = xcalloc(1, sizeof(InputFile));
  file->path = path;
  file->image = image;
  file->size = size;
  file->ehdr = (const Elf64_Ehdr *)image;

  if (!in_range(file->ehdr->e_shoff,
                (uint64_t)file->ehdr->e_shnum * sizeof(Elf64_Shdr), size)) {
    ld_error("%s: section header table is out of bounds", path);
    return -1;
  }
  file->shdrs = (const Elf64_Shdr *)(image + file->ehdr->e_shoff);
  file->nsections = file->ehdr->e_shnum;

  if (file->ehdr->e_shstrndx >= file->nsections) {
    ld_error("%s: invalid section name string table index", path);
    return -1;
  }
  const Elf64_Shdr *shstr = &file->shdrs[file->ehdr->e_shstrndx];
  if (!in_range(shstr->sh_offset, shstr->sh_size, size)) {
    ld_error("%s: section name string table is out of bounds", path);
    return -1;
  }
  file->shstrtab = (const char *)(image + shstr->sh_offset);

  file->sections = xcalloc(file->nsections ? file->nsections : 1,
                           sizeof(InputSection *));

  /* locate the symbol table */
  for (size_t i = 0; i < file->nsections; i++) {
    const Elf64_Shdr *sh = &file->shdrs[i];
    if (sh->sh_type == SHT_SYMTAB) {
      if (!in_range(sh->sh_offset, sh->sh_size, size)) {
        ld_error("%s: symbol table is out of bounds", path);
        return -1;
      }
      file->symtab = (const Elf64_Sym *)(image + sh->sh_offset);
      file->nsymbols = (size_t)(sh->sh_size / sizeof(Elf64_Sym));
      if (sh->sh_link < file->nsections) {
        const Elf64_Shdr *strh = &file->shdrs[sh->sh_link];
        if (in_range(strh->sh_offset, strh->sh_size, size))
          file->strtab = (const char *)(image + strh->sh_offset);
      }
      break;
    }
  }

  if (!file->symtab) {
    ld_error("%s: object file has no symbol table", path);
    return -1;
  }
  if (!file->strtab) {
    ld_error("%s: object file has no string table", path);
    return -1;
  }

  /* create the input sections */
  for (size_t i = 0; i < file->nsections; i++) {
    const Elf64_Shdr *sh = &file->shdrs[i];
    const char *name = file->shstrtab + sh->sh_name;

    if (!section_is_alloc(sh->sh_type)) continue;
    if (sh->sh_flags & SHF_EXCLUDE) continue;
    if (section_is_discarded(name, sh->sh_type)) continue;
    if (!(sh->sh_flags & SHF_ALLOC)) continue;

    InputSection *is = xcalloc(1, sizeof(InputSection));
    is->file = file;
    is->index = (uint32_t)i;
    is->name = name;
    is->type = sh->sh_type;
    is->flags = sh->sh_flags;
    is->size = sh->sh_size;
    is->align = sh->sh_addralign ? sh->sh_addralign : 1;
    is->entsize = sh->sh_entsize;
    is->is_bss = (sh->sh_type == SHT_NOBITS);
    if (!is->is_bss) {
      if (!in_range(sh->sh_offset, sh->sh_size, size)) {
        ld_error("%s: section %s is out of bounds", path, name);
        return -1;
      }
      is->data = image + sh->sh_offset;
    }
    is->live = true;
    file->sections[i] = is;
  }

  parse_comdat_groups(ctx, file);

  /* attach relocations to their target sections */
  for (size_t i = 0; i < file->nsections; i++) {
    const Elf64_Shdr *sh = &file->shdrs[i];
    if (sh->sh_type != SHT_RELA && sh->sh_type != SHT_REL) continue;
    if (sh->sh_info >= file->nsections) {
      ld_error("%s: relocation section %s has a bad target", path,
               file->shstrtab + sh->sh_name);
      continue;
    }
    InputSection *target = file->sections[sh->sh_info];
    if (!target) continue;
    parse_relocations(ctx, file, target, sh);
  }

  /* register the file */
  ctx->objects = xrealloc(ctx->objects, sizeof(InputFile *) * (ctx->nobj + 1));
  ctx->objects[ctx->nobj++] = file;

  /* global symbols */
  for (size_t i = 0; i < file->nsymbols; i++) {
    const Elf64_Sym *es = &file->symtab[i];
    if (i == 0) continue;
    uint8_t bind = ELF64_ST_BIND(es->st_info);
    if (bind == STB_LOCAL) continue;
    uint8_t type = ELF64_ST_TYPE(es->st_info);
    if (type == STT_FILE || type == STT_SECTION) continue;

    const char *name = file->strtab + es->st_name;
    if (!name[0]) continue;

    Symbol *sym = sym_get(ctx, name);
    sym->bind = bind;
    sym->type = type;
    if (ELF64_ST_VISIBILITY(es->st_other) != STV_DEFAULT)
      sym->visibility = ELF64_ST_VISIBILITY(es->st_other);

    if (es->st_shndx == SHN_UNDEF) {
      /* undefined reference */
      sym_reference(ctx, name, bind);
      continue;
    }
    if (es->st_shndx == SHN_COMMON) {
      /* tentative definition */
      if (sym->state == SYM_UNDEFINED || sym->state == SYM_COMMON) {
        if (sym->state != SYM_COMMON || es->st_size > sym->size) {
          sym->state = SYM_COMMON;
          sym->size = es->st_size;
          sym->value = es->st_value;   /* alignment */
          sym->is_weak = (bind == STB_WEAK);
          sym->file = file;
          sym->isec = NULL;
          sym->shndx = SHN_COMMON;
        }
      }
      continue;
    }
    if (es->st_shndx == SHN_ABS) {
      if (sym->state == SYM_UNDEFINED) {
        sym->state = SYM_ABSOLUTE;
        sym->value = es->st_value;
        sym->size = es->st_size;
        sym->bind = bind;
        sym->type = type;
        sym->file = file;
      }
      continue;
    }
    if (es->st_shndx >= file->nsections) continue;
    InputSection *is = file->sections[es->st_shndx];
    if (!is) continue;

    sym_define(ctx, sym, file, is, es->st_value, es->st_size, bind, type,
               ELF64_ST_VISIBILITY(es->st_other), SYM_DEFINED);
  }

  return 0;
}

int input_add_object(LdCtx *ctx, const char *path) {
  size_t size = 0;
  uint8_t *image = map_file(path, &size);
  if (!image) return -1;
  return load_object(ctx, path, image, size);
}

/* -------------------------------------------------------------- archives */
#define AR_MAGIC "!<arch>\n"

static bool ar_is_archive(const uint8_t *data, size_t size) {
  return size >= 8 && memcmp(data, AR_MAGIC, 8) == 0;
}

typedef struct {
  const uint8_t *data;
  size_t         size;
  char          *name;
} ArMember;

/* Read the name of an archive member, handling the GNU and BSD variants. */
static char *ar_member_name(LdCtx *ctx, const uint8_t *hdr, const uint8_t *body,
                             size_t body_size, const char *long_names) {
  char raw[17];
  memcpy(raw, hdr, 16);
  raw[16] = '\0';
  char *end = raw + strcspn(raw, " ");
  *end = '\0';

  if (!strncmp(raw, "#1/", 3)) {
    size_t len = (size_t)strtoul(raw + 3, NULL, 10);
    if (len > body_size) len = body_size;
    return xstrndup((const char *)body, len);
  }
  if (raw[0] == '/' && raw[1] == '/') {
    (void)ctx;
    return xstrdup("/");
  }
  if (raw[0] == '/') {
    size_t off = (size_t)strtoul(raw + 1, NULL, 10);
    if (long_names && off < strlen(long_names)) return xstrdup(long_names + off);
    return xstrdup("");
  }
  /* GNU short name: "name/" */
  size_t len = strlen(raw);
  if (len && raw[len - 1] == '/') raw[len - 1] = '\0';
  return xstrdup(raw);
}

/* Does the archive member define a symbol that is currently undefined? */
static bool member_defines_needed(LdCtx *ctx, const uint8_t *data, size_t size) {
  /* the archive also holds a symbol index and a name table, those members
   * are simply not objects */
  if (size < sizeof(Elf64_Ehdr)) return false;
  if (memcmp(data, "\177ELF", 4) != 0) return false;
  const Elf64_Ehdr *eh = (const Elf64_Ehdr *)data;
  if (eh->e_machine != EM_AARCH64) return false;
  if (eh->e_shoff == 0 || eh->e_shnum == 0) return false;
  if (!in_range(eh->e_shoff, (uint64_t)eh->e_shnum * sizeof(Elf64_Shdr), size))
    return false;
  const Elf64_Shdr *shdrs = (const Elf64_Shdr *)(data + eh->e_shoff);

  for (size_t i = 0; i < eh->e_shnum; i++) {
    if (shdrs[i].sh_type != SHT_SYMTAB) continue;
    if (!in_range(shdrs[i].sh_offset, shdrs[i].sh_size, size)) continue;
    if (shdrs[i].sh_link >= eh->e_shnum) return false;
    const Elf64_Shdr *strh = &shdrs[shdrs[i].sh_link];
    if (!in_range(strh->sh_offset, strh->sh_size, size)) return false;
    const Elf64_Sym *syms = (const Elf64_Sym *)(data + shdrs[i].sh_offset);
    size_t nsyms = (size_t)(shdrs[i].sh_size / sizeof(Elf64_Sym));
    const char *strtab = (const char *)(data + strh->sh_offset);

    for (size_t s = 1; s < nsyms; s++) {
      const Elf64_Sym *es = &syms[s];
      if (es->st_shndx == SHN_UNDEF) continue;
      uint8_t bind = ELF64_ST_BIND(es->st_info);
      if (bind != STB_GLOBAL && bind != STB_WEAK) continue;
      uint8_t type = ELF64_ST_TYPE(es->st_info);
      if (type == STT_FILE || type == STT_SECTION) continue;
      Symbol *sym = sym_lookup(ctx, strtab + es->st_name);
      if (sym && sym->state == SYM_UNDEFINED) return true;
    }
    return false;
  }
  return false;
}

int input_add_archive(LdCtx *ctx, const char *path) {
  size_t size = 0;
  uint8_t *image = map_file(path, &size);
  if (!image) return -1;
  if (!ar_is_archive(image, size)) {
    ld_error("%s: not an archive", path);
    return -1;
  }
  Archive *ar = xcalloc(1, sizeof(Archive));
  ar->path = xstrdup(path);
  ar->image = image;
  ar->size = size;
  ar->pos = 8;
  ctx->archives = xrealloc(ctx->archives, sizeof(Archive *) * (ctx->narchives + 1));
  ctx->archives[ctx->narchives++] = ar;
  return 0;
}

/* Pull every archive member that resolves a currently undefined symbol.
 * The scan is repeated until nothing changes, which mirrors the behaviour of
 * GNU ld: an archive may be listed before the object that needs it. */
int input_pull_archives(LdCtx *ctx) {
  bool progress = true;

  while (progress) {
    progress = false;
    for (size_t a = 0; a < ctx->narchives; a++) {
      Archive *ar = ctx->archives[a];
      const char *long_names = NULL;
      size_t p = 8;

      while (p + 60 <= ar->size) {
        const uint8_t *hdr = ar->image + p;
        const uint8_t *body = ar->image + p + 60;
        size_t msize = (size_t)strtoul((const char *)hdr + 48, NULL, 10);
        if (p + 60 + msize > ar->size) {
          ld_error("%s: truncated archive member", ar->path);
          break;
        }
        char *name = ar_member_name(ctx, hdr, body, msize, long_names);

        if (!strcmp(name, "/")) {
          long_names = xstrndup((const char *)body, msize);
        } else if (name[0] != '/' && strcmp(name, "/SYM64/") &&
                   strcmp(name, "__.SYMDEF")) {
          if (member_defines_needed(ctx, body, msize)) {
            char *member_path = xasprintf("%s(%s)", ar->path, name);
            if (load_object(ctx, member_path, (uint8_t *)body, msize) == 0)
              progress = true;
            free(member_path);
          }
        }
        free(name);
        p += 60 + msize + (msize & 1);
      }
    }
  }
  return 0;
}

/* ------------------------------------------------------- shared objects */
static int load_shared(LdCtx *ctx, const char *path, uint8_t *image, size_t size) {
  if (check_elf(image, size, path) != 0) return -1;
  const Elf64_Ehdr *eh = (const Elf64_Ehdr *)image;
  if (eh->e_type != ET_DYN) {
    ld_error("%s: not a shared object", path);
    return -1;
  }
  if (!in_range(eh->e_shoff, (uint64_t)eh->e_shnum * sizeof(Elf64_Shdr), size)) {
    ld_error("%s: section header table is out of bounds", path);
    return -1;
  }
  const Elf64_Shdr *shdrs = (const Elf64_Shdr *)(image + eh->e_shoff);
  size_t nsections = eh->e_shnum;

  const Elf64_Shdr *shstr = &shdrs[eh->e_shstrndx];
  const char *shstrtab = (const char *)(image + shstr->sh_offset);

  SharedFile *sh = xcalloc(1, sizeof(SharedFile));
  sh->path = path;
  sh->ehdr = eh;
  sh->shdrs = shdrs;
  sh->shstrtab = shstrtab;
  sh->soname = path;

  /* dynamic symbol table */
  for (size_t i = 0; i < nsections; i++) {
    const Elf64_Shdr *s = &shdrs[i];
    if (s->sh_type == SHT_DYNSYM) {
      if (!in_range(s->sh_offset, s->sh_size, size)) continue;
      sh->dynsym = (const Elf64_Sym *)(image + s->sh_offset);
      sh->ndynsym = (size_t)(s->sh_size / sizeof(Elf64_Sym));
      if (s->sh_link < nsections) {
        const Elf64_Shdr *strh = &shdrs[s->sh_link];
        if (in_range(strh->sh_offset, strh->sh_size, size))
          sh->dynstr = (const char *)(image + strh->sh_offset);
      }
    }
  }
  if (!sh->dynsym || !sh->dynstr) {
    ld_error("%s: shared object has no dynamic symbol table", path);
    return -1;
  }

  /* look for DT_SONAME in the dynamic section */
  for (size_t i = 0; i < nsections; i++) {
    const Elf64_Shdr *s = &shdrs[i];
    if (s->sh_type != SHT_DYNAMIC) continue;
    if (!in_range(s->sh_offset, s->sh_size, size)) continue;
    const Elf64_Dyn *dyn = (const Elf64_Dyn *)(image + s->sh_offset);
    size_t n = (size_t)(s->sh_size / sizeof(Elf64_Dyn));
    for (size_t d = 0; d < n && dyn[d].d_tag != DT_NULL; d++) {
      if (dyn[d].d_tag == DT_SONAME)
        sh->soname = sh->dynstr + dyn[d].d_un.d_val;
      else if (dyn[d].d_tag == DT_HASH)
        sh->dt_hash = (uint32_t)dyn[d].d_un.d_ptr;
      else if (dyn[d].d_tag == DT_GNU_HASH)
        sh->dt_gnu_hash = (uint32_t)dyn[d].d_un.d_ptr;
    }
  }

  ctx->shared = xrealloc(ctx->shared, sizeof(SharedFile *) * (ctx->nshared + 1));
  ctx->shared[ctx->nshared++] = sh;

  /* register the exported symbols */
  for (size_t i = 1; i < sh->ndynsym; i++) {
    const Elf64_Sym *es = &sh->dynsym[i];
    if (es->st_shndx == SHN_UNDEF) continue;
    uint8_t bind = ELF64_ST_BIND(es->st_info);
    if (bind != STB_GLOBAL && bind != STB_WEAK) continue;
    uint8_t type = ELF64_ST_TYPE(es->st_info);
    if (type == STT_FILE || type == STT_SECTION) continue;
    uint8_t vis = ELF64_ST_VISIBILITY(es->st_other);
    if (vis == STV_HIDDEN || vis == STV_INTERNAL) continue;

    const char *name = sh->dynstr + es->st_name;
    if (!name[0]) continue;
    /* The symbol is interned so that a later reference from an object file
     * finds it.  A definition in an object file always wins. */
    Symbol *sym = sym_get(ctx, name);
    if (sym->state != SYM_UNDEFINED) continue;   /* a real definition wins */
    sym->state = SYM_SHARED;
    sym->shared = sh;
    sym->bind = bind;
    sym->type = type;
    sym->visibility = vis;
    sym->size = es->st_size;
    sym->is_weak = (bind == STB_WEAK);
  }

  return 0;
}

/* ------------------------------------------------------ file type probing */
int input_add_file(LdCtx *ctx, const char *path) {
  size_t size = 0;
  uint8_t *image = map_file(path, &size);
  if (!image) return -1;

  if (ar_is_archive(image, size)) return input_add_archive(ctx, path);

  if (size >= sizeof(Elf64_Ehdr) &&
      memcmp(image, "\177ELF", 4) == 0) {
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)image;
    if (eh->e_type == ET_DYN) return load_shared(ctx, path, image, size);
    if (eh->e_type == ET_REL) return load_object(ctx, path, image, size);
    ld_error("%s: unsupported ELF type %u", path, eh->e_type);
    return -1;
  }
  ld_error("%s: unrecognised file format", path);
  return -1;
}

/* -------------------------------------------------------- library search */
static bool file_exists(const char *path) {
  return access(path, R_OK) == 0;
}

static char *join_path(const char *dir, const char *name) {
  if (!dir || !*dir) return xstrdup(name);
  size_t n = strlen(dir);
  if (dir[n - 1] == '/') return xasprintf("%s%s", dir, name);
  return xasprintf("%s/%s", dir, name);
}

static char *find_library(LdCtx *ctx, const char *name) {
  char *libname = xasprintf("lib%s.so", name);

  for (size_t i = 0; i < ctx->opt.nlibpaths; i++) {
    const char *dir = ctx->opt.libpaths[i];
    char *path = join_path(dir, libname);
    if (file_exists(path)) {
      free(libname);
      return path;
    }
    free(path);
  }
  free(libname);

  libname = xasprintf("lib%s.a", name);
  for (size_t i = 0; i < ctx->opt.nlibpaths; i++) {
    const char *dir = ctx->opt.libpaths[i];
    char *path = join_path(dir, libname);
    if (file_exists(path)) {
      free(libname);
      return path;
    }
    free(path);
  }
  free(libname);
  return NULL;
}

int input_resolve_libraries(LdCtx *ctx) {
  for (size_t i = 0; i < ctx->opt.nspecs; i++) {
    if (!ctx->opt.specs[i].is_lib) continue;
    const char *name = ctx->opt.specs[i].value;
    char *path = find_library(ctx, name);
    if (!path) {
      ld_error("cannot find -l%s", name);
      continue;
    }
    input_add_file(ctx, path);
    free(path);
  }
  return 0;
}
