#pragma once
#include <stdint.h>

/* Simple placeholders for park/unpark integration with netpoll/timers. */
void gof_park_current_until(uint64_t deadline_ns);
void gof_unpark_ready(void);
