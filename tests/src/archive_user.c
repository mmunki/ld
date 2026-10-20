/* ============================================================================
 * The user of the static archive.
 * ==========================================================================*/
#include <stdio.h>

extern int archive_answer(void);

int main(void) {
  printf("ld: the archive answered %d\n", archive_answer());
  return 0;
}
