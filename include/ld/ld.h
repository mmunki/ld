/* ============================================================================
 * ld -- core data model of the AArch64 (Android) linker
 * ----------------------------------------------------------------------------
 * The linker is a classic static linker:
 *
 *   input files (.o, .a, .so)
 *        -> symbols        (global symbol table, resolution)
 *        -> output sections (input sections are merged into output sections)
 *        -> layout         (addresses of sections and program headers)
 *        -> relocations    (static fixups, GOT/PLT creation)
 *        -> dynamic info   (.dynsym, .hash, .rela.dyn, .rela.plt, .dynamic)
 *        -> output file    (ELF64 AArch64)
 * ==========================================================================*/
#ifndef LD_H
#define LD_H

#include "ld/common.h"
#include "ld/elf.h"

/* ------------------------------------------------------------- forward decl */
typedef struct LdCtx       LdCtx;
typedef struct InputFile    InputFile;
typedef struct InputSection InputSection;
typedef struct OutSection   OutSection;
typedef struct Symbol       Symbol;
typedef struct SharedFile   SharedFile;
typedef struct Relocation   Relocation;
typedef struct GotEntry     GotEntry;
typedef struct PltEntry     PltEntry;
typedef struct DynReloc     DynReloc;
typedef struct ComdatGroup  ComdatGroup;

/* ----------------------------------------------------------- input actions */
typedef struct {
  char *value;    /* file name or library name */
  bool  is_lib;   /* true when it came from -l */
} InputSpec;

/* ------------------------------------------------------------------ options */
typedef struct {
  char  **libpaths;      size_t nlibpaths;   /* -L */
  InputSpec *specs;     size_t nspecs;       /* input files, in order    */
  char   *output;                            /* -o */
  char   *entry;                            /* -e */
  char   *soname;                           /* -soname */
  char   *interp;                           /* -dynamic-linker */
  char   *rpath;                            /* -rpath */
  char   *sysroot;                          /* --sysroot */
  char   *mapfile;                          /* -Map */
  bool    shared;                           /* -shared */
  bool    pie;                              /* -pie */
  bool    static_link;                      /* -static */
  bool    no_pie;                           /* -no-pie */
  bool    strip_all;                        /* -s */
  bool    no_undefined;                     /* --no-undefined */
  bool    allow_undefined;                  /* --allow-shlib-undefined */
  bool    build_id;                         /* --build-id */
  bool    eh_frame_hdr;                     /* --eh-frame-hdr */
  bool    z_now;                            /* -z now */
  bool    z_relro;                          /* -z relro */
  bool    z_execstack;                      /* -z execstack */
  bool    print_map;                        /* -M / --print-map */
  bool    print_version;                    /* --version */
  bool    print_help;                       /* --help */
  bool    gc_sections;                      /* --gc-sections */
  uint64_t image_base;                      /* --image-base */
  uint64_t page_size;                       /* --page-size */
} LdOptions;

/* -------------------------------------------------------------- output type */
typedef enum {
  OUT_EXEC,      /* ET_EXEC, no dynamic linker involved            */
  OUT_PIE,       /* ET_DYN, position independent executable        */
  OUT_SHARED     /* ET_DYN, shared library                         */
} OutputKind;

/* --------------------------------------------------------------- symbol state */
typedef enum {
  SYM_UNDEFINED = 0,  /* referenced, not defined anywhere yet          */
  SYM_DEFINED,        /* defined by a regular object file              */
  SYM_COMMON,         /* tentative definition (common symbol)          */
  SYM_SHARED,         /* defined by a shared object                    */
  SYM_ABSOLUTE        /* SHN_ABS, value is a constant                  */
} SymState;

/* --------------------------------------------------------------- relocations */
struct Relocation {
  uint64_t    offset;     /* offset inside the section being relocated */
  uint32_t    type;       /* R_AARCH64_*                               */
  uint32_t    sym_index;  /* index into the symbol table of the file  */
  int64_t     addend;     /* RELA addend                               */
  Symbol     *sym;        /* global symbol (NULL for local symbols)    */
  InputSection *sym_isec;/* defining section of a local symbol        */
  uint64_t    sym_value;  /* value of a local symbol inside its section */
  const char *sym_name;
  bool        sym_is_abs; /* local symbol is an absolute constant      */
  bool        resolved;   /* symbol has been resolved                  */
};

/* ----------------------------------------------------------- COMDAT groups */
struct ComdatGroup {
  char          *signature;   /* name of the group leader symbol */
  InputFile     *file;
  InputSection **members;
  size_t         nmembers;
  bool           claimed;     /* the first definition wins          */
};

/* ---------------------------------------------------------- static archives */
typedef struct {
  char   *path;
  uint8_t *image;
  size_t  size;
  size_t  pos;        /* scan position inside the archive */
} Archive;

/* ------------------------------------------------------------------ sections */
struct InputSection {
  InputFile   *file;
  ComdatGroup *group;        /* COMDAT group this section belongs to */
  uint32_t     index;         /* section index inside the input file  */
  const char  *name;
  uint32_t     type;          /* SHT_*                               */
  uint64_t     flags;         /* SHF_*                               */
  uint64_t     size;
  uint64_t     align;
  uint64_t     entsize;
  const uint8_t *data;        /* NULL for SHT_NOBITS                 */
  OutSection  *osec;         /* output section it belongs to        */
  uint64_t     out_off;       /* offset inside the output section    */
  uint64_t     addr;          /* final address                       */
  Relocation  *rels;
  size_t       nrels;
  bool         live;          /* kept in the output                   */
  bool         is_bss;        /* NOBITS, occupies memory only        */
};

struct OutSection {
  const char   *name;
  uint32_t      type;         /* SHT_*                               */
  uint64_t      flags;        /* SHF_*                               */
  uint64_t      align;
  uint64_t      addr;
  uint64_t      offset;       /* file offset                         */
  uint64_t      size;
  Buffer        data;         /* contents (empty for NOBITS)          */
  InputSection **members;
  size_t        nmembers;
  size_t        cap_members;
  uint32_t      shndx;        /* section header index (0 = none)      */
  uint32_t      sh_name;      /* offset in .shstrtab                 */
  uint32_t      sh_link;
  uint32_t      sh_info;
  uint64_t      entsize;
  bool          synthetic;    /* generated by the linker             */
};

/* ------------------------------------------------------------------- symbols */
struct Symbol {
  char        *name;
  uint8_t      bind;         /* STB_*                                */
  uint8_t      type;         /* STT_*                                */
  uint8_t      visibility;   /* STV_*                                */
  SymState     state;
  uint64_t     value;        /* st_value of the definition           */
  uint64_t     size;
  uint32_t     shndx;        /* defining section index in the file   */
  InputFile   *file;         /* file holding the definition          */
  InputSection *isec;        /* defining input section               */
  SharedFile  *shared;       /* defining shared object               */
  uint64_t     addr;         /* final address (after layout)         */
  uint32_t     got_index;    /* index in .got, -1 when unused        */
  uint32_t     gotplt_index; /* index in .got.plt, -1 when unused    */
  uint32_t     plt_index;    /* index in .plt, -1 when unused        */
  uint32_t     dynsym_index; /* index in .dynsym, -1 when unused     */
  bool         is_weak;
  bool         is_imported;  /* undefined but known to come from .so */
  bool         exported;     /* goes to .dynsym                      */
  bool         referenced;   /* used by some relocation              */
  bool         needs_got;
  bool         needs_plt;
};

/* --------------------------------------------------------------- input files */
struct InputFile {
  const char      *path;
  uint8_t         *image;      /* mmap'ed contents                    */
  size_t           size;
  const Elf64_Ehdr *ehdr;
  const Elf64_Shdr *shdrs;
  const char      *shstrtab;
  size_t           nsections;
  InputSection   **sections;   /* NULL entries for ignored sections   */
  const Elf64_Sym *symtab;
  size_t           nsymbols;
  const char      *strtab;
  Symbol         **symbols;    /* per file symbol table (may be NULL) */
  bool             is_shared;
};

struct SharedFile {
  const char      *path;
  const char      *soname;
  const Elf64_Ehdr *ehdr;
  const Elf64_Shdr *shdrs;
  const char      *shstrtab;
  const Elf64_Sym *dynsym;
  size_t           ndynsym;
  const char      *dynstr;
  bool             needed;     /* recorded in DT_NEEDED               */
  bool             is_needed;  /* provides a symbol that is used      */
  uint32_t         dt_hash;
  uint32_t         dt_gnu_hash;
  uint32_t         dt_strtab;
  uint32_t         dt_syment;
};

/* ----------------------------------------------------------------- GOT / PLT */
typedef enum {
  GOT_NONE = 0,
  GOT_LOCAL,   /* resolved at link time, R_AARCH64_RELATIVE at run time */
  GOT_GLOBAL   /* R_AARCH64_GLOB_DAT                                 */
} GotKind;

struct GotEntry {
  Symbol  *sym;
  GotKind  kind;
  uint64_t addr;
  uint32_t index;
};

struct PltEntry {
  Symbol *sym;
  uint64_t addr;
  uint32_t index;
  uint32_t got_slot;   /* offset of the GOT slot inside .got.plt */
};

/* ------------------------------------------------------- dynamic relocations */
typedef enum {
  DYN_RELATIVE = 0,  /* base + addend                                  */
  DYN_GLOB_DAT,      /* resolved symbol (+ addend)                     */
  DYN_JUMP_SLOT,     /* resolved symbol, PLT entry                      */
  DYN_GOT_SLOT       /* the site is the GOT slot of the symbol          */
} DynRelocKind;

struct DynReloc {
  uint64_t    offset;      /* the site address (0 when still unknown)   */
  uint32_t    type;        /* R_AARCH64_*                                */
  uint32_t    sym_index;   /* index in .dynsym (0 for RELATIVE)          */
  int64_t     addend;
  DynRelocKind kind;
  Symbol     *sym;
  InputSection *isec;      /* the section that holds the site           */
  uint64_t    site_off;    /* the offset of the site inside that section*/
  const Relocation *rel;    /* the relocation this entry was made for   */
};

/* ------------------------------------------------------------------ context */
struct LdCtx {
  LdOptions    opt;
  OutputKind   kind;

  InputFile  **objects;    size_t nobj;      /* ET_REL inputs            */
  InputSection **extra;    size_t nextra;    /* synthetic input sections */
  SharedFile **shared;    size_t nshared;   /* ET_DYN inputs            */
  Archive    **archives;  size_t narchives; /* .a inputs, scanned later */

  Symbol    **symbols;    size_t nsymbols;  /* global symbol table      */
  size_t      cap_symbols;
  Symbol     *sym_null;                    /* index 0 of .dynsym        */

  OutSection **osecs;     size_t nosecs;
  size_t      cap_osecs;

  /* dynamic output */
  GotEntry   *got;        size_t ngot;      size_t cap_got;
  PltEntry   *plt;        size_t nplt;      size_t cap_plt;
  DynReloc   *dynrels;    size_t ndynrels;  size_t cap_dynrels;
  Symbol    **dynsyms;    size_t ndynsyms;  size_t cap_dynsyms;
  StrTable    dynstr;
  StrTable    shstrtab;
  StrTable    symstr;

  OutSection *o_text, *o_rodata, *o_data, *o_bss;
  OutSection *o_plt, *o_got, *o_gotplt, *o_dynamic, *o_reladyn, *o_relaplt;
  OutSection *o_dynsym, *o_dynstr, *o_hash, *o_gnu_hash, *o_interp;
  OutSection *o_note, *o_init_array, *o_fini_array, *o_eh_frame;
  OutSection *o_symtab, *o_strtab, *o_shstrtab_out;

  uint64_t    entry_addr;
  bool        has_dynamic;      /* output needs a dynamic section      */
  bool        has_interp;
  bool        textrel;
  bool        saw_undefined;

  Elf64_Phdr *phdrs;       size_t nphdrs;
  uint64_t    phdr_off;    /* file offset of the program header table */
  uint64_t    relro_start; /* start of the RELRO region                */
  uint64_t    relro_size;
  uint64_t    base_addr;   /* load address of the image               */
};

/* ------------------------------------------------------------------- options */
void options_init(LdOptions *o);
int  options_parse(LdCtx *ctx, int argc, char **argv);
void options_usage(FILE *out);
void options_version(FILE *out);

/* --------------------------------------------------------------------- input */
int  input_add_object(LdCtx *ctx, const char *path);
int  input_add_archive(LdCtx *ctx, const char *path);
int  input_add_shared(LdCtx *ctx, const char *path);
int  input_add_file(LdCtx *ctx, const char *path);
int  input_resolve_libraries(LdCtx *ctx);
int  input_pull_archives(LdCtx *ctx);

/* ------------------------------------------------------------------ symbols */
Symbol *sym_get(LdCtx *ctx, const char *name);       /* create if missing */
Symbol *sym_lookup(LdCtx *ctx, const char *name);
void    sym_define(LdCtx *ctx, Symbol *s, InputFile *file, InputSection *isec,
                   uint64_t value, uint64_t size, uint8_t bind, uint8_t type,
                   uint8_t visibility, SymState state);
void    sym_reference(LdCtx *ctx, const char *name, uint8_t bind);
uint64_t sym_address(LdCtx *ctx, Symbol *s);
void    sym_allocate_commons(LdCtx *ctx);
void    sym_report_undefined(LdCtx *ctx);
void    sym_define_linker_symbols(LdCtx *ctx);
void    sym_update_linker_symbols(LdCtx *ctx);

/* ------------------------------------------------------------------ sections */
OutSection *osec_get(LdCtx *ctx, const char *name, uint32_t type, uint64_t flags);
void        osec_add_member(OutSection *o, InputSection *is);
void        sections_create(LdCtx *ctx);
void        sections_prepare_strings(LdCtx *ctx);
void        sections_finalize(LdCtx *ctx);
void        symtab_fill(LdCtx *ctx);

/* -------------------------------------------------------------------- layout */
void layout(LdCtx *ctx);
void layout_write_headers(LdCtx *ctx, Buffer *out);

/* --------------------------------------------------------------- relocations */
void reloc_scan(LdCtx *ctx);
void reloc_apply(LdCtx *ctx);
uint64_t reloc_symbol_value(LdCtx *ctx, const Relocation *r);

/* ------------------------------------------------------------------ dynamic */
void dynamic_create(LdCtx *ctx);
void dynamic_fill(LdCtx *ctx);
uint32_t dynsym_add(LdCtx *ctx, Symbol *s);
void got_add(LdCtx *ctx, Symbol *s, GotKind kind);
void plt_add(LdCtx *ctx, Symbol *s);
void dynreloc_add(LdCtx *ctx, DynRelocKind kind, Symbol *s,
                   const Relocation *rel, int64_t addend, InputSection *isec,
                   uint64_t site_off);

/* ------------------------------------------------------------------- output */
void output_write(LdCtx *ctx, const char *path);
void output_map(LdCtx *ctx, FILE *out);

#endif /* LD_H */
