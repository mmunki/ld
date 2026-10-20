/* ============================================================================
 * ld -- common utilities
 * ----------------------------------------------------------------------------
 * Memory helpers, growable byte buffers, string builders and the diagnostic
 * (error / warning) reporting used by every module of the linker.
 * ==========================================================================*/
#ifndef LD_COMMON_H
#define LD_COMMON_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------- diagnostics */
void ld_fatal(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
void ld_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void ld_warn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void ld_note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
unsigned ld_error_count(void);

/* ----------------------------------------------------------------- memory */
void *xmalloc(size_t size);
void *xcalloc(size_t nmemb, size_t size);
void *xrealloc(void *ptr, size_t size);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
char *xasprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* ---------------------------------------------------------------- buffers */
typedef struct {
  uint8_t *data;
  size_t   size;
  size_t   cap;
} Buffer;

void buf_init(Buffer *b);
void buf_free(Buffer *b);
void buf_reserve(Buffer *b, size_t extra);
void buf_append(Buffer *b, const void *data, size_t len);
void buf_zero(Buffer *b, size_t len);
void buf_fill(Buffer *b, uint8_t value, size_t len);
void buf_align(Buffer *b, size_t align);
void buf_put_u8(Buffer *b, uint8_t v);
void buf_put_u16(Buffer *b, uint16_t v);
void buf_put_u32(Buffer *b, uint32_t v);
void buf_put_u64(Buffer *b, uint64_t v);
void buf_put_str0(Buffer *b, const char *s);
void buf_patch_u32(Buffer *b, size_t off, uint32_t v);
void buf_patch_u64(Buffer *b, size_t off, uint64_t v);

/* --------------------------------------------------------- string builder */
typedef struct {
  Buffer buf;
} StrBuf;

void strbuf_init(StrBuf *s);
void strbuf_free(StrBuf *s);
void strbuf_add(StrBuf *s, const char *text);
void strbuf_addf(StrBuf *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* ------------------------------------------------------------- string table */
typedef struct {
  Buffer  data;
  uint32_t *offsets;    /* offset of each entry, index 0 is the empty string */
  size_t   count;
  size_t   cap;
} StrTable;

void strtab_init(StrTable *t);
void strtab_free(StrTable *t);
uint32_t strtab_add(StrTable *t, const char *s);
uint32_t strtab_find(StrTable *t, const char *s);

/* ------------------------------------------------------------------ misc */
uint64_t align_up(uint64_t value, uint64_t alignment);
const char *ld_basename(const char *path);

#endif /* LD_COMMON_H */
