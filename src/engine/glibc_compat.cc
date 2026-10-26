// What the engine would otherwise take from a newer glibc than 2.17, the
// oldest it runs on (Linux).
//
// Rust's standard library refers to getrandom() weakly, falling back to the
// system call when it is missing. The reference still carries the version
// glibc gave it, GLIBC_2.25, and glibc 2.17's dynamic linker refuses a
// library that names a version it lacks, weak or not. Defined here, inside
// the engine and not exported, the reference is resolved as the engine is
// linked: the system call itself, which fails with ENOSYS on kernels before
// 3.17, where Rust reads /dev/urandom instead.

#include <stddef.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

extern "C" __attribute__((visibility("hidden"))) ssize_t
getrandom(void* buffer, size_t length, unsigned int flags) {
  return syscall(SYS_getrandom, buffer, length, flags);
}
