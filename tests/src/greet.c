/* ============================================================================
 * A tiny shared library.  It is linked into a .so by the linker under test
 * and loaded at run time by the dynamic linker.
 * ==========================================================================*/
int ld_add(int a, int b) {
  return a + b;
}

const char *ld_greeting = "ld: hello from the shared library";
