/* ============================================================================
 * ld -- ELF64 definitions for AArch64 (Android)
 * ----------------------------------------------------------------------------
 * Self contained ELF64 definitions.  The linker is a cross tool: it always
 * reads and writes little endian AArch64 objects, no matter what the host
 * architecture is, therefore the on-disk structures are described here
 * instead of relying on the host <elf.h>.
 * ==========================================================================*/
#ifndef LD_ELF_H
#define LD_ELF_H

#include <stdint.h>

typedef uint64_t Elf64_Addr;
typedef uint64_t Elf64_Off;
typedef uint16_t Elf64_Half;
typedef uint32_t Elf64_Word;
typedef int32_t  Elf64_Sword;
typedef uint64_t Elf64_Xword;
typedef int64_t  Elf64_Sxword;

#define EI_NIDENT 16

typedef struct {
  unsigned char e_ident[EI_NIDENT];
  Elf64_Half    e_type;
  Elf64_Half    e_machine;
  Elf64_Word    e_version;
  Elf64_Addr    e_entry;
  Elf64_Off     e_phoff;
  Elf64_Off     e_shoff;
  Elf64_Word    e_flags;
  Elf64_Half    e_ehsize;
  Elf64_Half    e_phentsize;
  Elf64_Half    e_phnum;
  Elf64_Half    e_shentsize;
  Elf64_Half    e_shnum;
  Elf64_Half    e_shstrndx;
} Elf64_Ehdr;

typedef struct {
  Elf64_Word  sh_name;
  Elf64_Word  sh_type;
  Elf64_Xword sh_flags;
  Elf64_Addr  sh_addr;
  Elf64_Off   sh_offset;
  Elf64_Xword sh_size;
  Elf64_Word  sh_link;
  Elf64_Word  sh_info;
  Elf64_Xword sh_addralign;
  Elf64_Xword sh_entsize;
} Elf64_Shdr;

typedef struct {
  Elf64_Word    st_name;
  unsigned char st_info;
  unsigned char st_other;
  Elf64_Half    st_shndx;
  Elf64_Addr    st_value;
  Elf64_Xword   st_size;
} Elf64_Sym;

typedef struct {
  Elf64_Addr   r_offset;
  Elf64_Xword  r_info;
  Elf64_Sxword r_addend;
} Elf64_Rela;

typedef struct {
  Elf64_Addr   r_offset;
  Elf64_Xword  r_info;
} Elf64_Rel;

typedef struct {
  Elf64_Word  p_type;
  Elf64_Word  p_flags;
  Elf64_Off   p_offset;
  Elf64_Addr  p_vaddr;
  Elf64_Addr  p_paddr;
  Elf64_Xword p_filesz;
  Elf64_Xword p_memsz;
  Elf64_Xword p_align;
} Elf64_Phdr;

typedef struct {
  Elf64_Sxword d_tag;
  union {
    Elf64_Xword d_val;
    Elf64_Addr  d_ptr;
  } d_un;
} Elf64_Dyn;

/* --------------------------------------------------------------- constants */
#define EI_MAG0        0
#define EI_MAG1        1
#define EI_MAG2        2
#define EI_MAG3        3
#define EI_CLASS       4
#define EI_DATA        5
#define EI_VERSION     6
#define EI_OSABI       7
#define EI_ABIVERSION  8

#define ELFMAG0        0x7f
#define ELFMAG1        'E'
#define ELFMAG2        'L'
#define ELFMAG3        'F'

#define ELFCLASS64     2
#define ELFDATA2LSB    1
#define EV_CURRENT     1

#define ET_NONE        0
#define ET_REL         1
#define ET_EXEC        2
#define ET_DYN         3
#define ET_CORE        4

#define EM_AARCH64     183

#define ELFOSABI_NONE  0
#define ELFOSABI_ANDROID 98

/* section types */
#define SHT_NULL           0
#define SHT_PROGBITS       1
#define SHT_SYMTAB         2
#define SHT_STRTAB         3
#define SHT_RELA           4
#define SHT_HASH           5
#define SHT_DYNAMIC        6
#define SHT_NOTE           7
#define SHT_NOBITS         8
#define SHT_REL            9
#define SHT_SHLIB          10
#define SHT_DYNSYM         11
#define SHT_INIT_ARRAY     14
#define SHT_FINI_ARRAY     15
#define SHT_PREINIT_ARRAY  16
#define SHT_GROUP          17
#define SHT_SYMTAB_SHNDX   18
#define SHT_GNU_HASH       0x6ffffff6
#define SHT_GNU_verdef     0x6ffffffd
#define SHT_GNU_verneed    0x6ffffffe
#define SHT_GNU_versym     0x6fffffff

/* section flags */
#define SHF_WRITE            0x1
#define SHF_ALLOC            0x2
#define SHF_EXECINSTR        0x4
#define SHF_MERGE            0x10
#define SHF_STRINGS          0x20
#define SHF_INFO_LINK        0x40
#define SHF_LINK_ORDER       0x80
#define SHF_OS_NONCONFORMING 0x100
#define SHF_GROUP            0x200
#define SHF_TLS              0x400
#define SHF_COMPRESSED       0x800
#define SHF_GNU_RETAIN       0x200000
#define SHF_EXCLUDE          0x80000000

#define SHN_UNDEF     0
#define SHN_LORESERVE 0xff00
#define SHN_ABS       0xfff1
#define SHN_COMMON    0xfff2
#define SHN_XINDEX    0xffff

/* symbol bindings / types / visibility */
#define STB_LOCAL   0
#define STB_GLOBAL  1
#define STB_WEAK    2
#define STB_GNU_UNIQUE 10

#define STT_NOTYPE  0
#define STT_OBJECT  1
#define STT_FUNC    2
#define STT_SECTION 3
#define STT_FILE    4
#define STT_COMMON  5
#define STT_TLS     6
#define STT_GNU_IFUNC 10

#define STV_DEFAULT   0
#define STV_INTERNAL  1
#define STV_HIDDEN    2
#define STV_PROTECTED 3

#define ELF64_ST_BIND(i)      ((i) >> 4)
#define ELF64_ST_TYPE(i)      ((i) & 0xf)
#define ELF64_ST_INFO(b, t)   (((b) << 4) | ((t) & 0xf))
#define ELF64_ST_VISIBILITY(o) ((o) & 0x3)

#define ELF64_R_SYM(i)   ((uint32_t)((i) >> 32))
#define ELF64_R_TYPE(i)  ((uint32_t)((i) & 0xffffffffULL))
#define ELF64_R_INFO(s, t) ((((uint64_t)(s)) << 32) | ((uint64_t)(t) & 0xffffffffULL))

/* program header types */
#define PT_NULL         0
#define PT_LOAD         1
#define PT_DYNAMIC      2
#define PT_INTERP       3
#define PT_NOTE         4
#define PT_SHLIB        5
#define PT_PHDR         6
#define PT_TLS          7
#define PT_GNU_EH_FRAME 0x6474e550
#define PT_GNU_STACK    0x6474e551
#define PT_GNU_RELRO    0x6474e552
#define PT_GNU_PROPERTY 0x6474e553

#define PF_X 0x1
#define PF_W 0x2
#define PF_R 0x4

/* dynamic tags */
#define DT_NULL            0
#define DT_NEEDED          1
#define DT_PHDR            3
#define DT_PLTRELSZ        2
#define DT_PLTGOT          3
#define DT_HASH            4
#define DT_STRTAB          5
#define DT_SYMTAB          6
#define DT_RELA            7
#define DT_RELASZ          8
#define DT_RELAENT         9
#define DT_STRSZ           10
#define DT_SYMENT          11
#define DT_INIT            12
#define DT_FINI            13
#define DT_SONAME          14
#define DT_RPATH           15
#define DT_SYMBOLIC        16
#define DT_REL             17
#define DT_RELSZ           18
#define DT_RELENT          19
#define DT_PLTREL          20
#define DT_DEBUG           21
#define DT_TEXTREL         22
#define DT_JMPREL          23
#define DT_BIND_NOW        24
#define DT_INIT_ARRAY      25
#define DT_FINI_ARRAY      26
#define DT_INIT_ARRAYSZ    27
#define DT_FINI_ARRAYSZ    28
#define DT_RUNPATH         29
#define DT_FLAGS           30
#define DT_PREINIT_ARRAY   32
#define DT_PREINIT_ARRAYSZ 33
#define DT_GNU_HASH        0x6ffffef5
#define DT_RELACOUNT       0x6ffffff9
#define DT_RELCOUNT        0x6ffffffa
#define DT_FLAGS_1         0x6ffffffb
#define DT_VERSYM          0x6ffffff0
#define DT_VERDEF          0x6ffffffc
#define DT_VERNEED         0x6ffffffe
#define DT_VERNEEDNUM      0x6fffffff
#define DT_ANDROID_REL      0x6000000f
#define DT_ANDROID_RELSZ    0x60000010
#define DT_ANDROID_RELA     0x60000011
#define DT_ANDROID_RELASZ   0x60000012
#define DT_ANDROID_RELR     0x6fffe000
#define DT_ANDROID_RELRSZ   0x6fffe001
#define DT_ANDROID_RELRENT  0x6fffe003
#define DT_TLSDESC_PLT     0x6ffffef6
#define DT_TLSDESC_GOT     0x6ffffef7

/* dynamic flags */
#define DF_ORIGIN     0x1
#define DF_SYMBOLIC   0x2
#define DF_TEXTREL    0x4
#define DF_BIND_NOW   0x8
#define DF_STATIC_TLS 0x10

#define DF_1_NOW  0x1
#define DF_1_GLOBAL 0x2
#define DF_1_GROUP 0x4
#define DF_1_NODELETE 0x8
#define DF_1_PIE 0x08000000

/* ------------------------------------------------- AArch64 relocation types */
#define R_AARCH64_NONE                   0
#define R_AARCH64_ABS64                  0x101
#define R_AARCH64_ABS32                  0x102
#define R_AARCH64_ABS16                  0x103
#define R_AARCH64_PREL64                 0x104
#define R_AARCH64_PREL32                 0x105
#define R_AARCH64_PREL16                 0x106
#define R_AARCH64_MOVW_UABS_G0           0x107
#define R_AARCH64_MOVW_UABS_G0_NC        0x108
#define R_AARCH64_MOVW_UABS_G1           0x109
#define R_AARCH64_MOVW_UABS_G1_NC        0x10a
#define R_AARCH64_MOVW_UABS_G2           0x10b
#define R_AARCH64_MOVW_UABS_G2_NC        0x10c
#define R_AARCH64_MOVW_UABS_G3           0x10d
#define R_AARCH64_MOVW_SABS_G0           0x10e
#define R_AARCH64_MOVW_SABS_G1           0x10f
#define R_AARCH64_MOVW_SABS_G2           0x110
#define R_AARCH64_LD_PREL_LO19           0x111
#define R_AARCH64_ADR_PREL_LO21          0x112
#define R_AARCH64_ADR_PREL_PG_HI21       0x113
#define R_AARCH64_ADR_PREL_PG_HI21_NC    0x114
#define R_AARCH64_ADD_ABS_LO12_NC        0x115
#define R_AARCH64_LDST8_ABS_LO12_NC      0x116
#define R_AARCH64_TSTBR14                0x117
#define R_AARCH64_CONDBR19               0x118
#define R_AARCH64_JUMP26                 0x11a
#define R_AARCH64_CALL26                 0x11b
#define R_AARCH64_LDST16_ABS_LO12_NC     0x11c
#define R_AARCH64_LDST32_ABS_LO12_NC     0x11d
#define R_AARCH64_LDST64_ABS_LO12_NC     0x11e
#define R_AARCH64_MOVW_PREL_G0           0x11f
#define R_AARCH64_MOVW_PREL_G0_NC        0x120
#define R_AARCH64_MOVW_PREL_G1           0x121
#define R_AARCH64_MOVW_PREL_G1_NC        0x122
#define R_AARCH64_MOVW_PREL_G2           0x123
#define R_AARCH64_MOVW_PREL_G2_NC        0x124
#define R_AARCH64_MOVW_PREL_G3           0x125
#define R_AARCH64_LDST128_ABS_LO12_NC    0x12b
#define R_AARCH64_MOVW_GOTOFF_G0         0x12c
#define R_AARCH64_MOVW_GOTOFF_G0_NC      0x12d
#define R_AARCH64_MOVW_GOTOFF_G1         0x12e
#define R_AARCH64_MOVW_GOTOFF_G1_NC      0x12f
#define R_AARCH64_MOVW_GOTOFF_G2         0x130
#define R_AARCH64_MOVW_GOTOFF_G2_NC      0x131
#define R_AARCH64_MOVW_GOTOFF_G3         0x132
#define R_AARCH64_GOTREL64               0x133
#define R_AARCH64_GOTREL32               0x134
#define R_AARCH64_GOT_LD_PREL19          0x135
#define R_AARCH64_LD64_GOTOFF_LO15       0x136
#define R_AARCH64_ADR_GOT_PAGE           0x137
#define R_AARCH64_LD64_GOT_LO12_NC       0x138
#define R_AARCH64_LD64_GOTPAGE_LO15      0x139
#define R_AARCH64_PLT32                  0x13a
#define R_AARCH64_GOTPCREL32             0x13b
#define R_AARCH64_TLSGD_ADR_PREL21       0x200
#define R_AARCH64_TLSGD_ADR_PAGE21       0x201
#define R_AARCH64_TLSGD_ADD_LO12_NC      0x202
#define R_AARCH64_TLSGD_MOVW_G1          0x203
#define R_AARCH64_TLSGD_MOVW_G0_NC       0x204
#define R_AARCH64_TLSLD_ADR_PREL21       0x205
#define R_AARCH64_TLSLD_ADR_PAGE21       0x206
#define R_AARCH64_TLSLD_ADD_LO12_NC      0x207
#define R_AARCH64_TLSLD_MOVW_G1          0x208
#define R_AARCH64_TLSLD_MOVW_G0_NC       0x209
#define R_AARCH64_TLSLD_LD_PREL19        0x20a
#define R_AARCH64_TLSLD_MOVW_DTPREL_G2   0x20b
#define R_AARCH64_TLSLD_MOVW_DTPREL_G1   0x20c
#define R_AARCH64_TLSLD_MOVW_DTPREL_G1_NC 0x20d
#define R_AARCH64_TLSLD_MOVW_DTPREL_G0   0x20e
#define R_AARCH64_TLSLD_MOVW_DTPREL_G0_NC 0x20f
#define R_AARCH64_TLSLD_ADD_DTPREL_HI12  0x210
#define R_AARCH64_TLSLD_ADD_DTPREL_LO12  0x211
#define R_AARCH64_TLSLD_ADD_DTPREL_LO12_NC 0x212
#define R_AARCH64_TLSLD_LDST8_DTPREL_LO12 0x213
#define R_AARCH64_TLSLD_LDST16_DTPREL_LO12 0x215
#define R_AARCH64_TLSLD_LDST32_DTPREL_LO12 0x217
#define R_AARCH64_TLSLD_LDST64_DTPREL_LO12 0x219
#define R_AARCH64_TLSLE_MOVW_TPREL_G2    0x220
#define R_AARCH64_TLSLE_MOVW_TPREL_G1    0x221
#define R_AARCH64_TLSLE_MOVW_TPREL_G1_NC 0x222
#define R_AARCH64_TLSLE_MOVW_TPREL_G0    0x223
#define R_AARCH64_TLSLE_MOVW_TPREL_G0_NC 0x224
#define R_AARCH64_TLSLE_ADD_TPREL_HI12   0x225
#define R_AARCH64_TLSLE_ADD_TPREL_LO12   0x226
#define R_AARCH64_TLSLE_ADD_TPREL_LO12_NC 0x227
#define R_AARCH64_TLSLE_LDST8_TPREL_LO12 0x228
#define R_AARCH64_TLSLE_LDST16_TPREL_LO12 0x22a
#define R_AARCH64_TLSLE_LDST32_TPREL_LO12 0x22c
#define R_AARCH64_TLSLE_LDST64_TPREL_LO12 0x22e
#define R_AARCH64_TLSDESC_LD_PREL19      0x230
#define R_AARCH64_TLSDESC_ADR_PREL21     0x231
#define R_AARCH64_TLSDESC_ADR_PAGE21     0x232
#define R_AARCH64_TLSDESC_LD64_LO12      0x233
#define R_AARCH64_TLSDESC_ADD_LO12       0x234
#define R_AARCH64_TLSDESC_OFF_G1         0x235
#define R_AARCH64_TLSDESC_OFF_G0_NC      0x236
#define R_AARCH64_TLSDESC_LDR            0x237
#define R_AARCH64_TLSDESC_ADD            0x238
#define R_AARCH64_TLSDESC_CALL           0x239
#define R_AARCH64_AUTH_ABS64             0x244

/* dynamic relocation types */
#define R_AARCH64_COPY           0x400
#define R_AARCH64_GLOB_DAT       0x401
#define R_AARCH64_JUMP_SLOT      0x402
#define R_AARCH64_RELATIVE       0x403
#define R_AARCH64_TLS_DTPMOD64   0x404
#define R_AARCH64_TLS_DTPREL64   0x405
#define R_AARCH64_TLS_TPREL64    0x406
#define R_AARCH64_TLSDESC        0x407
#define R_AARCH64_IRELATIVE      0x408
#define R_AARCH64_AUTH_RELATIVE  0x411

#endif /* LD_ELF_H */
