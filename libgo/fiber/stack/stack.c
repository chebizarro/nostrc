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
  size_t pagesz = si.dwPageSize;
  size = (size + pagesz - 1) & ~(pagesz - 1);
  void *mem = VirtualAlloc(NULL, size + pagesz, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  if (!mem) return -1;
  DWORD old;
  VirtualProtect(mem, pagesz, PAGE_NOACCESS, &old);
  out->guard = mem;
  out->base = (uint8_t*)mem + pagesz;
  out->size = size;
  return 0;
#else
  long pagesz = sysconf(_SC_PAGESIZE);
  if (pagesz <= 0) pagesz = 4096;
  size = (size + pagesz - 1) & ~(pagesz - 1);
  size_t total = size + pagesz;
  void *mem = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) return -1;
  mprotect(mem, pagesz, PROT_NONE);
  out->guard = mem;
  out->base = (uint8_t*)mem + pagesz;
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
  size_t total = st->size + (size_t)pagesz;
  munmap(mem, total);
#endif
  st->base = NULL; st->size = 0; st->guard = NULL;
}

void* gof_stack_top(const gof_stack *st) {
  return st ? (void*)((uintptr_t)st->base + st->size) : NULL;
}
