/* ============================================================================
 * A program that uses the shared library and the C library.  It is linked as
 * a position independent executable.
 * ==========================================================================*/
#include <stdio.h>

extern int ld_add(int a, int b);
extern const char *ld_greeting;

int main(void) {
  printf("%s: %d\n", ld_greeting, ld_add(20, 22));
  return 0;
}
