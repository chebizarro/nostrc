#pragma once
#include <stdint.h>
#include <stddef.h>
#include "../include/libgo/fiber.h"

/* Internal counters (optional) */
extern uint64_t gof_ctx_switches;
extern uint64_t gof_parks;
extern uint64_t gof_unparks;

/* Weak hooks declared in public header are defined as no-ops here unless overridden */
