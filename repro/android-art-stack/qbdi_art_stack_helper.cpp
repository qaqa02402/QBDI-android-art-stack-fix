#include <setjmp.h>

#include <cstdio>

extern "C" __attribute__((visibility("default"))) int helper_cpp_throw() {
  std::printf("[helper] helper_cpp_throw about to throw\n");
  throw 42;
}

extern "C" __attribute__((visibility("default"))) void
helper_longjmp(void *env, int value) {
  std::printf("[helper] helper_longjmp value=%d\n", value);
  longjmp(*reinterpret_cast<jmp_buf *>(env), value);
}
