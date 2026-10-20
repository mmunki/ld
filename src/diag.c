/* ============================================================================
 * ld -- diagnostics
 * ----------------------------------------------------------------------------
 * Every message goes to stderr and is prefixed with the tool name so that it
 * is easy to tell linker messages apart from compiler or runtime messages.
 * ==========================================================================*/
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "ld/common.h"

static unsigned g_errors = 0;

void ld_fatal(const char *fmt, ...) {
  va_list ap;
  fputs("ld: fatal error: ", stderr);
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
  exit(1);
}

void ld_error(const char *fmt, ...) {
  va_list ap;
  fputs("ld: error: ", stderr);
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
  g_errors++;
}

void ld_warn(const char *fmt, ...) {
  va_list ap;
  fputs("ld: warning: ", stderr);
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
}

void ld_note(const char *fmt, ...) {
  va_list ap;
  fputs("ld: note: ", stderr);
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
}

unsigned ld_error_count(void) { return g_errors; }
