#include <stdint.h>
#include <stddef.h>
#include "context.h"

#if !GOF_PORTABLE_CONTEXT
/* External assembly routines */
extern void gof_bootstrap_trampoline(void);

int gof_ctx_init_bootstrap(gof_context *ctx, void *stack_base, size_t stack_size, void (*entry)(void*), void *arg) {
  uintptr_t top = (uintptr_t)stack_base + stack_size;
#if defined(__aarch64__) || defined(_M_ARM64)
  // AArch64 requires 16-byte alignment on SP.
  top &= ~(uintptr_t)0xF; // align down to 16
  ctx->sp = (void*)top;
  ctx->entry = entry;
  ctx->arg = arg;
  // Initialize callee-saved registers so first swap will "return" into the trampoline.
  // x19 will hold the ctx pointer for the trampoline to load entry/arg from.
  // Set FP to 0, LR to trampoline address.
  for (int i = 0; i < 12; ++i) ctx->x19_x28_fp_lr[i] = 0;
  ctx->x19_x28_fp_lr[0] = (uint64_t)(uintptr_t)ctx;             // x19
  ctx->x19_x28_fp_lr[11] = (uint64_t)(uintptr_t)gof_bootstrap_trampoline; // lr (x30)
  // No special init needed for q8-q15.
  return 0;
#elif defined(__x86_64__) || defined(_M_X64)
  // SysV x86_64: ensure 16-byte alignment. Place the trampoline address as the return address.
  // To satisfy the SysV rule (RSP % 16 == 8 on function entry), set SP so that
  // after the RET pops 8 bytes, RSP becomes (aligned_top - 8).
  top &= ~(uintptr_t)0xF; // align down to 16
  uintptr_t ret_slot = top - 16; // place saved RIP at [top-16]
  *(uint64_t*)ret_slot = (uint64_t)(uintptr_t)gof_bootstrap_trampoline;
  ctx->sp = (void*)ret_slot;
  ctx->entry = entry;
  ctx->arg = arg;
  // Zero callee-saved set and stash ctx into r12 so the trampoline can load entry/arg.
  for (int i = 0; i < 6; ++i) ctx->rbx_rbp_r12_r15[i] = 0;
  ctx->rbx_rbp_r12_r15[2] = (uint64_t)(uintptr_t)ctx; // r12 = ctx
  ctx->rip_saved = (uint64_t)(uintptr_t)gof_bootstrap_trampoline; // informational
  return 0;
#else
#error Unsupported architecture
#endif
}

#endif
