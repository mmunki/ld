/* ============================================================================
 * A program that only uses the C library.  It is linked as a position
 * independent executable and needs nothing but libc.
 * ==========================================================================*/
#include <stdio.h>

int main(void) {
  printf("ld: hello from a position independent executable\n");
  return 0;
}
