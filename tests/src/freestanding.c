/* ============================================================================
 * A freestanding AArch64 program.  It uses raw system calls so that it can
 * be linked without any C library, which makes it a perfect test for the
 * static link path of the linker.
 * ==========================================================================*/
static inline long syscall3(long n, long a, long b, long c) {
  register long x8 __asm__("x8") = n;
  register long x0 __asm__("x0") = a;
  register long x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory");
  return x0;
}

static inline long syscall1(long n, long a) {
  register long x8 __asm__("x8") = n;
  register long x0 __asm__("x0") = a;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory");
  return x0;
}

static const char message[] = "ld: hello from a statically linked program\n";

void _start(void) {
  syscall3(64, 1, (long)message, sizeof(message) - 1);   /* write */
  syscall1(93, 0);                                      /* exit  */
  for (;;) { }
}
