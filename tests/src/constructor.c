/* ============================================================================
 * A program with a global constructor.  The constructor has to be called by
 * the dynamic linker through the .init_array.
 * ==========================================================================*/
#include <stdio.h>

__attribute__((constructor)) static void constructor(void) {
  printf("ld: the constructor ran\n");
}

int main(void) {
  printf("ld: main ran\n");
  return 0;
}
