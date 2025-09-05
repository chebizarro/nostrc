/* Ensure XOPEN is defined via header. */
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "context.h"

/* Simple debug macro */
#ifdef GOF_DEBUG
#define GOF_LOG(...) fprintf(stderr, __VA_ARGS__)
#else
#define GOF_LOG(...) do {} while(0)
#endif

/* macOS makecontext expects int-sized args; pass pointer split as two ints */
static void gof_trampoline(int low, int high) {
  uintptr_t p = ((uintptr_t)(uint32_t)high << 32) | (uintptr_t)(uint32_t)low;
  gof_context *ctx = (gof_context*)p;
  GOF_LOG("[gof] trampoline enter ctx=%p entry=%p arg=%p\n", (void*)ctx, (void*)ctx->entry, ctx->arg);
  void (*entry)(void*) = ctx->entry;
  void *arg = ctx->arg;
  entry(arg);
  /* If user function returns, there is no uc_link; simply exit this context. */
  /* In our scheduler, fiber entry will switch back to scheduler explicitly. */
  for(;;) {}
}

int gof_ctx_init_bootstrap(gof_context *ctx, void *stack_base, size_t stack_size, void (*entry)(void*), void *arg) {
#if defined(__APPLE__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
  if (getcontext(&ctx->uc) != 0) return -1;
  ctx->entry = entry;
  ctx->arg = arg;
  ctx->uc.uc_stack.ss_sp = stack_base;
  ctx->uc.uc_stack.ss_size = stack_size;
  ctx->uc.uc_link = NULL;
  /* Pass pointer as two int-sized args for ABI safety */
  uintptr_t p = (uintptr_t)ctx;
  int low = (int)(uint32_t)(p & 0xFFFFFFFFu);
  int high = (int)(uint32_t)((p >> 32) & 0xFFFFFFFFu);
  GOF_LOG("[gof] ctx_init ctx=%p stack=[%p..+%zu] entry=%p arg=%p low=%d high=%d\n", (void*)ctx, stack_base, stack_size, (void*)gof_trampoline, arg, low, high);
  makecontext(&ctx->uc, (void (*)(void))gof_trampoline, 2, low, high);
#if defined(__APPLE__)
#pragma clang diagnostic pop
#endif
  return 0;
}

void gof_ctx_swap(gof_context *from, gof_context *to) {
  GOF_LOG("[gof] ctx_swap from=%p to=%p\n", (void*)from, (void*)to);
#if defined(__APPLE__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
  swapcontext(&from->uc, &to->uc);
#if defined(__APPLE__)
#pragma clang diagnostic pop
#endif
}
