.text
.align 4

# x86_64 Darwin/Mach-O uses leading underscores for global symbols
.globl _gof_ctx_swap
.globl _gof_bootstrap_trampoline

# Offsets into gof_context (LP64)
# struct gof_context {
#   void *sp;                 // +0
#   void (*entry)(void*);     // +8
#   void *arg;                // +16
#   uint64_t rbx..r15[6];     // +24 (rbx at +24)
#   uint64_t rip_saved;       // +72 (unused by swap; we ret to saved stack)
# }

.set GOF_OFF_SP,   0
.set GOF_OFF_ENTRY,8
.set GOF_OFF_ARG,  16
.set GOF_OFF_RBX,  24
.set GOF_OFF_RBP,  32
.set GOF_OFF_R12,  40
.set GOF_OFF_R13,  48
.set GOF_OFF_R14,  56
.set GOF_OFF_R15,  64

# void gof_ctx_swap(gof_context *from, gof_context *to)
# SysV ABI: rdi=from, rsi=to
_gof_ctx_swap:
  # Save callee-saved registers into from
  movq %rbx, GOF_OFF_RBX(%rdi)
  movq %rbp, GOF_OFF_RBP(%rdi)
  movq %r12, GOF_OFF_R12(%rdi)
  movq %r13, GOF_OFF_R13(%rdi)
  movq %r14, GOF_OFF_R14(%rdi)
  movq %r15, GOF_OFF_R15(%rdi)
  movq %rsp, GOF_OFF_SP(%rdi)

  # Restore callee-saved and stack from to
  movq GOF_OFF_RBX(%rsi), %rbx
  movq GOF_OFF_RBP(%rsi), %rbp
  movq GOF_OFF_R12(%rsi), %r12
  movq GOF_OFF_R13(%rsi), %r13
  movq GOF_OFF_R14(%rsi), %r14
  movq GOF_OFF_R15(%rsi), %r15
  movq GOF_OFF_SP(%rsi),  %rsp

  # Return into the restored stack's return address
  ret

# Bootstrap trampoline: expects r12 = ctx
# Loads entry and arg from ctx, calls entry(arg), then spins
_gof_bootstrap_trampoline:
  movq %r12, %rdi                # keep ctx in rdi temporarily if needed
  movq GOF_OFF_ENTRY(%r12), %rax # rax = entry
  movq GOF_OFF_ARG(%r12), %rdi   # rdi = arg (SysV first arg)
  call *%rax
1:
  jmp 1b
