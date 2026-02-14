#ifndef LIBGO_FIBER_STACK_STACK_H
#define LIBGO_FIBER_STACK_STACK_H
#include <stddef.h>
#include <stdint.h>

typedef struct gof_stack {
  void   *base;      /* base address (lowest) */
  size_t  size;      /* bytes */
  void   *guard;     /* guard page address (if any) */
} gof_stack;

int  gof_stack_alloc(gof_stack *out, size_t size);
void gof_stack_free(gof_stack *st);
void* gof_stack_top(const gof_stack *st);
#endif /* LIBGO_FIBER_STACK_STACK_H */
