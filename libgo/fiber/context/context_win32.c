/* Windows fiber context backend: the Fibers API.
 *
 * A hand-written ARM64 swap would have to keep x18 (the TEB pointer) intact
 * and swap the TEB's StackBase/StackLimit/DeallocationStack with every fiber
 * stack, or guard-page stack growth, __chkstk probes and SEH unwinding fault
 * on a fiber stack. SwitchToFiber does exactly that bookkeeping, so the
 * Windows build uses it on every architecture. Fibers may resume on any
 * worker thread; only the thread's own fiber (gof_ctx_thread_enter) is tied
 * to its thread. */
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdlib.h>
#include "context.h"

static VOID CALLBACK gof_win_fiber_main(LPVOID param) {
  gof_context *ctx = (gof_context *)param;
  ctx->entry(ctx->arg);
  /* The scheduler's entry trampoline switches away for good when the fiber
   * finishes and the worker deletes it; returning from a fiber routine would
   * exit the worker thread instead. */
  abort();
}

int gof_ctx_init_bootstrap(gof_context *ctx, void *stack_base, size_t stack_size,
                           void (*entry)(void*), void *arg) {
  (void)stack_base; /* GOF_CONTEXT_OWNS_STACK: the reservation is made here */
  ctx->entry = entry;
  ctx->arg = arg;
  ctx->converted = 0;
  /* Commit 0 takes the image's default initial commit; the rest of the
   * reservation is committed on demand through the fiber's guard page. */
  ctx->fiber = CreateFiberEx(0, stack_size, FIBER_FLAG_FLOAT_SWITCH, gof_win_fiber_main, ctx);
  return ctx->fiber ? 0 : -1;
}

void gof_ctx_swap(gof_context *from, gof_context *to) {
  (void)from; /* the current fiber's state is saved into its own fiber block */
  SwitchToFiber(to->fiber);
}

void gof_ctx_destroy(gof_context *ctx) {
  if (ctx && ctx->fiber) {
    DeleteFiber(ctx->fiber);
    ctx->fiber = NULL;
  }
}

void gof_ctx_thread_enter(gof_context *ctx) {
  if (IsThreadAFiber()) {
    /* Already a fiber (embedding application): borrow it, do not undo it. */
    ctx->fiber = GetCurrentFiber();
    ctx->converted = 0;
    return;
  }
  ctx->fiber = ConvertThreadToFiberEx(NULL, FIBER_FLAG_FLOAT_SWITCH);
  if (!ctx->fiber) abort(); /* no context to schedule fibers from */
  ctx->converted = 1;
}

void gof_ctx_thread_leave(gof_context *ctx) {
  if (ctx->converted) (void)ConvertFiberToThread();
  ctx->fiber = NULL;
  ctx->converted = 0;
}
#endif /* _WIN32 */
