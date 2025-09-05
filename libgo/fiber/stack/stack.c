#define _GNU_SOURCE
#include "stack.h"
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

int gof_stack_alloc(gof_stack *out, size_t size) {
  if (!out) return -1;
#if defined(_WIN32)
  SYSTEM_INFO si; GetSystemInfo(&si);
  size_t pagesz = si.dwPageSize ? si.dwPageSize : 4096;
  const size_t min_stack = 16 * 1024; // enforce a sane minimum
  if (size < min_stack) size = min_stack;
  size = (size + pagesz - 1) & ~(pagesz - 1);
  // Allocate with two guard pages (low and high)
  size_t total = size + 2 * pagesz;
  void *mem = VirtualAlloc(NULL, total, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  if (!mem) return -1;
  DWORD old;
  // Low guard
  VirtualProtect(mem, pagesz, PAGE_NOACCESS, &old);
  // High guard
  VirtualProtect((uint8_t*)mem + pagesz + size, pagesz, PAGE_NOACCESS, &old);
  out->guard = mem;               // record start for free()
  out->base = (uint8_t*)mem + pagesz; // usable base just after low guard
  out->size = size;               // usable size between guards
  return 0;
#else
  long pagesz = sysconf(_SC_PAGESIZE);
  if (pagesz <= 0) pagesz = 4096;
  const size_t min_stack = 16 * 1024; // enforce a sane minimum
  if (size < min_stack) size = min_stack;
  size = (size + (size_t)pagesz - 1) & ~((size_t)pagesz - 1);
  // Allocate with two guard pages (low and high)
  size_t total = size + (size_t)pagesz * 2;
  void *mem = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) return -1;
  // Low guard
  (void)mprotect(mem, (size_t)pagesz, PROT_NONE);
  // High guard at end of usable region
  (void)mprotect((uint8_t*)mem + (size_t)pagesz + size, (size_t)pagesz, PROT_NONE);
  out->guard = mem;
  out->base = (uint8_t*)mem + (size_t)pagesz;
  out->size = size;
  return 0;
#endif
}

void gof_stack_free(gof_stack *st) {
  if (!st || !st->base) return;
#if defined(_WIN32)
  void *mem = st->guard;
  VirtualFree(mem, 0, MEM_RELEASE);
#else
  long pagesz = sysconf(_SC_PAGESIZE);
  if (pagesz <= 0) pagesz = 4096;
  void *mem = st->guard;
  size_t total = st->size + (size_t)pagesz * 2;
  munmap(mem, total);
#endif
  st->base = NULL; st->size = 0; st->guard = NULL;
}

void* gof_stack_top(const gof_stack *st) {
  return st ? (void*)((uintptr_t)st->base + st->size) : NULL;
}
