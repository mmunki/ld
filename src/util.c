/* ============================================================================
 * ld -- memory, buffers, string tables and small helpers
 * ==========================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ld/common.h"

/* ----------------------------------------------------------------- memory */
void *xmalloc(size_t size) {
  void *p = malloc(size ? size : 1);
  if (!p) {
    fprintf(stderr, "ld: error: out of memory\n");
    exit(1);
  }
  return p;
}

void *xcalloc(size_t nmemb, size_t size) {
  void *p = calloc(nmemb ? nmemb : 1, size ? size : 1);
  if (!p) {
    fprintf(stderr, "ld: error: out of memory\n");
    exit(1);
  }
  return p;
}

void *xrealloc(void *ptr, size_t size) {
  void *p = realloc(ptr, size ? size : 1);
  if (!p) {
    fprintf(stderr, "ld: error: out of memory\n");
    exit(1);
  }
  return p;
}

char *xstrdup(const char *s) {
  size_t n = strlen(s);
  char *p = xmalloc(n + 1);
  memcpy(p, s, n + 1);
  return p;
}

char *xstrndup(const char *s, size_t n) {
  char *p = xmalloc(n + 1);
  memcpy(p, s, n);
  p[n] = '\0';
  return p;
}

char *xasprintf(const char *fmt, ...) {
  va_list ap, ap2;
  va_start(ap, fmt);
  va_copy(ap2, ap);
  int n = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (n < 0) n = 0;
  char *out = xmalloc((size_t)n + 1);
  vsnprintf(out, (size_t)n + 1, fmt, ap2);
  va_end(ap2);
  return out;
}

/* ---------------------------------------------------------------- buffers */
void buf_init(Buffer *b) {
  b->data = NULL;
  b->size = 0;
  b->cap = 0;
}

void buf_free(Buffer *b) {
  free(b->data);
  buf_init(b);
}

void buf_reserve(Buffer *b, size_t extra) {
  if (b->size + extra <= b->cap) return;
  size_t cap = b->cap ? b->cap : 64;
  while (cap < b->size + extra) cap *= 2;
  b->data = xrealloc(b->data, cap);
  b->cap = cap;
}

void buf_append(Buffer *b, const void *data, size_t len) {
  if (!len) return;
  buf_reserve(b, len);
  memcpy(b->data + b->size, data, len);
  b->size += len;
}

void buf_zero(Buffer *b, size_t len) {
  buf_reserve(b, len);
  memset(b->data + b->size, 0, len);
  b->size += len;
}

void buf_fill(Buffer *b, uint8_t value, size_t len) {
  buf_reserve(b, len);
  memset(b->data + b->size, value, len);
  b->size += len;
}

void buf_align(Buffer *b, size_t align) {
  if (align < 2) return;
  size_t rem = b->size % align;
  if (rem) buf_zero(b, align - rem);
}

void buf_put_u8(Buffer *b, uint8_t v) { buf_append(b, &v, 1); }

void buf_put_u16(Buffer *b, uint16_t v) {
  uint8_t t[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
  buf_append(b, t, 2);
}

void buf_put_u32(Buffer *b, uint32_t v) {
  uint8_t t[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
  buf_append(b, t, 4);
}

void buf_put_u64(Buffer *b, uint64_t v) {
  uint8_t t[8];
  for (int i = 0; i < 8; i++) t[i] = (uint8_t)(v >> (8 * i));
  buf_append(b, t, 8);
}

void buf_put_str0(Buffer *b, const char *s) {
  buf_append(b, s, strlen(s) + 1);
}

void buf_patch_u32(Buffer *b, size_t off, uint32_t v) {
  b->data[off + 0] = (uint8_t)v;
  b->data[off + 1] = (uint8_t)(v >> 8);
  b->data[off + 2] = (uint8_t)(v >> 16);
  b->data[off + 3] = (uint8_t)(v >> 24);
}

void buf_patch_u64(Buffer *b, size_t off, uint64_t v) {
  for (int i = 0; i < 8; i++) b->data[off + i] = (uint8_t)(v >> (8 * i));
}

/* --------------------------------------------------------- string builder */
void strbuf_init(StrBuf *s) { buf_init(&s->buf); }
void strbuf_free(StrBuf *s) { buf_free(&s->buf); }

void strbuf_add(StrBuf *s, const char *text) {
  buf_append(&s->buf, text, strlen(text));
}

void strbuf_addf(StrBuf *s, const char *fmt, ...) {
  va_list ap, ap2;
  va_start(ap, fmt);
  va_copy(ap2, ap);
  int n = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (n < 0) n = 0;
  buf_reserve(&s->buf, (size_t)n + 1);
  vsnprintf((char *)s->buf.data + s->buf.size, (size_t)n + 1, fmt, ap2);
  va_end(ap2);
  s->buf.size += (size_t)n;
}

/* ------------------------------------------------------------- string table */
void strtab_init(StrTable *t) {
  buf_init(&t->data);
  t->offsets = NULL;
  t->count = 0;
  t->cap = 0;
  /* entry 0 is the empty string */
  buf_put_u8(&t->data, 0);
  t->cap = 16;
  t->offsets = xmalloc(sizeof(uint32_t) * t->cap);
  t->offsets[0] = 0;
  t->count = 1;
}

void strtab_free(StrTable *t) {
  buf_free(&t->data);
  free(t->offsets);
  t->offsets = NULL;
  t->count = t->cap = 0;
}

uint32_t strtab_add(StrTable *t, const char *s) {
  if (!s || !*s) return 0;
  for (size_t i = 1; i < t->count; i++)
    if (strcmp((const char *)t->data.data + t->offsets[i], s) == 0)
      return (uint32_t)t->offsets[i];

  if (t->count == t->cap) {
    t->cap *= 2;
    t->offsets = xrealloc(t->offsets, sizeof(uint32_t) * t->cap);
  }
  uint32_t off = (uint32_t)t->data.size;
  buf_put_str0(&t->data, s);
  t->offsets[t->count++] = off;
  return off;
}

uint32_t strtab_find(StrTable *t, const char *s) {
  if (!s || !*s) return 0;
  for (size_t i = 1; i < t->count; i++)
    if (strcmp((const char *)t->data.data + t->offsets[i], s) == 0)
      return (uint32_t)t->offsets[i];
  return 0;
}

/* -------------------------------------------------------------------- misc */
uint64_t align_up(uint64_t value, uint64_t alignment) {
  if (alignment <= 1) return value;
  return (value + alignment - 1) & ~(alignment - 1);
}

const char *ld_basename(const char *path) {
  const char *slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}
