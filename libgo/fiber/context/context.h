#ifndef LIBGO_FIBER_CONTEXT_CONTEXT_H
#define LIBGO_FIBER_CONTEXT_CONTEXT_H
#include <stdint.h>
#include <stddef.h>
#include <stdalign.h>
#if GOF_PORTABLE_CONTEXT
#  ifndef _XOPEN_SOURCE
#    define _XOPEN_SOURCE 700
#  endif
#  include <ucontext.h>
#endif

/*
 * Backend-neutral fiber context API. On the portable path (GOF_PORTABLE_CONTEXT=1),
 * we wrap POSIX ucontext. On the non-portable path (assembly), we define an
 * architecture-specific register save area. The public API remains identical.
 */

#if GOF_PORTABLE_CONTEXT
typedef struct gof_context {
  ucontext_t uc;
  void (*entry)(void*);
  void *arg;
} gof_context;
#else
/* Assembly backend (non-deprecated on macOS). Provide per-arch layouts. */
#  if defined(__aarch64__) || defined(_M_ARM64)
typedef struct gof_context {
  /* Stack pointer for this context */
  void *sp;
  /* Entrypoint for bootstrap and its argument */
  void (*entry)(void*);
  void *arg;
  /* Callee-saved general-purpose registers x19-x28, then fp(x29), lr(x30) */
  uint64_t x19_x28_fp_lr[12];
  /* Padding to align the following SIMD area to 16 bytes at offset 128 */
  uint64_t pad64_for_q;
  /* Callee-saved SIMD registers q8-q15 (16 bytes each). Ensure 16-byte alignment. */
  alignas(16) uint8_t q8_q15[8 * 16];
} gof_context;
#  elif defined(__x86_64__) || defined(_M_X64)
typedef struct gof_context {
  /* Stack pointer for this context */
  void *sp;
  /* Entrypoint for bootstrap and its argument */
  void (*entry)(void*);
  void *arg;
  /* Callee-saved GPRs on System V x86_64: rbx, rbp, r12, r13, r14, r15 */
  uint64_t rbx_rbp_r12_r15[6];
  /* Saved instruction pointer target to jump to when resuming (trampoline/return label) */
  uint64_t rip_saved;
} gof_context;
#  else
#    error "Unsupported architecture for assembly context backend"
#  endif
#endif

/* Initialize a new fiber context to start at `entry(arg)` on the provided stack. */
int gof_ctx_init_bootstrap(gof_context *ctx, void *stack_base, size_t stack_size, void (*entry)(void*), void *arg);
/* Swap from one context to another. Must preserve callee-saved state. */
void gof_ctx_swap(gof_context *from, gof_context *to);
#endif /* LIBGO_FIBER_CONTEXT_CONTEXT_H */
