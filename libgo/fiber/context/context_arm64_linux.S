.text
.p2align 2

// AArch64 Linux/ELF uses global symbols without leading underscores
.global gof_ctx_swap
.global gof_bootstrap_trampoline
.type gof_ctx_swap, %function
.type gof_bootstrap_trampoline, %function

// Offsets into gof_context for the assembly backend layout (LP64)
// struct gof_context {
//   void *sp;                           // +0
//   void (*entry)(void*);               // +8
//   void *arg;                          // +16
//   uint64_t x19_x28_fp_lr[12];         // +24 (x19 at +24)
//   uint8_t q8_q15[8*16];               // +24 + 96 = +120, size 128
// }

.set GOF_OFF_SP, 0
.set GOF_OFF_ENTRY, 8
.set GOF_OFF_ARG, 16
.set GOF_OFF_X19, 24
.set GOF_OFF_X20, 32
.set GOF_OFF_X21, 40
.set GOF_OFF_X22, 48
.set GOF_OFF_X23, 56
.set GOF_OFF_X24, 64
.set GOF_OFF_X25, 72
.set GOF_OFF_X26, 80
.set GOF_OFF_X27, 88
.set GOF_OFF_X28, 96
.set GOF_OFF_FP,  104
.set GOF_OFF_LR,  112
.set GOF_OFF_QBASE, 128

// void gof_ctx_swap(gof_context *from, gof_context *to)
// Save callee-saved state and SP into *from, load from *to, then branch to to->lr
gof_ctx_swap:
  // Arguments: x0 = from, x1 = to
  // Save callee-saved general-purpose registers into from
  str x19, [x0, #GOF_OFF_X19]
  str x20, [x0, #GOF_OFF_X20]
  str x21, [x0, #GOF_OFF_X21]
  str x22, [x0, #GOF_OFF_X22]
  str x23, [x0, #GOF_OFF_X23]
  str x24, [x0, #GOF_OFF_X24]
  str x25, [x0, #GOF_OFF_X25]
  str x26, [x0, #GOF_OFF_X26]
  str x27, [x0, #GOF_OFF_X27]
  str x28, [x0, #GOF_OFF_X28]
  str x29, [x0, #GOF_OFF_FP]
  str x30, [x0, #GOF_OFF_LR]
  mov x2, sp
  str x2, [x0, #GOF_OFF_SP]

  // Save callee-saved SIMD registers q8-q15 into from
  str q8,  [x0, #GOF_OFF_QBASE + 16*0]
  str q9,  [x0, #GOF_OFF_QBASE + 16*1]
  str q10, [x0, #GOF_OFF_QBASE + 16*2]
  str q11, [x0, #GOF_OFF_QBASE + 16*3]
  str q12, [x0, #GOF_OFF_QBASE + 16*4]
  str q13, [x0, #GOF_OFF_QBASE + 16*5]
  str q14, [x0, #GOF_OFF_QBASE + 16*6]
  str q15, [x0, #GOF_OFF_QBASE + 16*7]

  // Restore callee-saved state from to
  ldr x19, [x1, #GOF_OFF_X19]
  ldr x20, [x1, #GOF_OFF_X20]
  ldr x21, [x1, #GOF_OFF_X21]
  ldr x22, [x1, #GOF_OFF_X22]
  ldr x23, [x1, #GOF_OFF_X23]
  ldr x24, [x1, #GOF_OFF_X24]
  ldr x25, [x1, #GOF_OFF_X25]
  ldr x26, [x1, #GOF_OFF_X26]
  ldr x27, [x1, #GOF_OFF_X27]
  ldr x28, [x1, #GOF_OFF_X28]
  ldr x29, [x1, #GOF_OFF_FP]
  ldr x30, [x1, #GOF_OFF_LR]
  ldr x2,  [x1, #GOF_OFF_SP]
  mov sp, x2

  ldr q8,  [x1, #GOF_OFF_QBASE + 16*0]
  ldr q9,  [x1, #GOF_OFF_QBASE + 16*1]
  ldr q10, [x1, #GOF_OFF_QBASE + 16*2]
  ldr q11, [x1, #GOF_OFF_QBASE + 16*3]
  ldr q12, [x1, #GOF_OFF_QBASE + 16*4]
  ldr q13, [x1, #GOF_OFF_QBASE + 16*5]
  ldr q14, [x1, #GOF_OFF_QBASE + 16*6]
  ldr q15, [x1, #GOF_OFF_QBASE + 16*7]

  // Branch to to->lr
  ret
.size gof_ctx_swap, .-gof_ctx_swap

// Bootstrap trampoline: expects x19 = ctx
// Loads entry and arg from ctx, calls entry(arg), then spins
gof_bootstrap_trampoline:
  // x19 holds ctx
  mov x0, x19               // preserve ctx in x0 if needed
  ldr x2, [x19, #GOF_OFF_ENTRY] // x2 = entry
  ldr x1, [x19, #GOF_OFF_ARG]   // x1 = arg
  mov x0, x1                    // arg in x0 per AAPCS64
  blr x2                        // call entry(arg)
1:
  b 1b                          // do not return
.size gof_bootstrap_trampoline, .-gof_bootstrap_trampoline
