#define _GNU_SOURCE
#include "stack.h"
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#if !defined(_WIN32)
#include <sys/mman.h>
#include <unistd.h>
#endif

int gof_stack_alloc(gof_stack *out, size_t size) {
  if (!out) return -1;
#if defined(_WIN32)
  /* GOF_CONTEXT_OWNS_STACK: CreateFiberEx reserves the stack (and maintains
   * its guard page), so only the size is recorded here. Reservations are
   * made in allocation-granularity (64 KiB) units. */
  const size_t min_stack = 64 * 1024;
  if (size < min_stack) size = min_stack;
  size = (size + min_stack - 1) & ~(min_stack - 1);
  out->guard = NULL;
  out->base = NULL;
  out->size = size;
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
  if (mprotect(mem, (size_t)pagesz, PROT_NONE) != 0) {
    munmap(mem, total);
    return -1;
  }
  // High guard at end of usable region
  if (mprotect((uint8_t*)mem + (size_t)pagesz + size, (size_t)pagesz, PROT_NONE) != 0) {
    munmap(mem, total);
    return -1;
  }
  out->guard = mem;
  out->base = (uint8_t*)mem + (size_t)pagesz;
  out->size = size;
  return 0;
#endif
}

void gof_stack_free(gof_stack *st) {
#if defined(_WIN32)
  if (st) st->size = 0; /* the fiber's stack goes with gof_ctx_destroy */
#else
  if (!st || !st->base) return;
  long pagesz = sysconf(_SC_PAGESIZE);
  if (pagesz <= 0) pagesz = 4096;
  void *mem = st->guard;
  size_t total = st->size + (size_t)pagesz * 2;
  munmap(mem, total);
  st->base = NULL; st->size = 0; st->guard = NULL;
#endif
}

void* gof_stack_top(const gof_stack *st) {
  return st ? (void*)((uintptr_t)st->base + st->size) : NULL;
}
