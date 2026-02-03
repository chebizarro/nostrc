#pragma once
#include <stdint.h>
#include <stddef.h>
#include "../include/libgo/fiber.h"
#include "../sched/sched.h"

/* Internal counters (optional) */
extern uint64_t gof_ctx_switches;
extern uint64_t gof_parks;
extern uint64_t gof_unparks;

/* Fiber introspection registry (nostrc-l1no)
 * Called by scheduler on fiber create/destroy to maintain fiber list. */
void gof_introspect_register(gof_fiber *f);
void gof_introspect_unregister(gof_fiber *f);

/* Weak hooks declared in public header are defined as no-ops here unless overridden */
