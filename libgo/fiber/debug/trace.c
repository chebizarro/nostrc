#include "debug.h"
#include <stdio.h>
#include <inttypes.h>

uint64_t gof_ctx_switches = 0;
uint64_t gof_parks = 0;
uint64_t gof_unparks = 0;

#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak)) void gof_trace_on_switch(uint64_t old_id, uint64_t new_id) {
  (void)old_id; (void)new_id;
}
__attribute__((weak)) void gof_trace_on_block(int fd, int ev) {
  (void)fd; (void)ev;
}
__attribute__((weak)) void gof_trace_on_unblock(int fd, int ev) {
  (void)fd; (void)ev;
}
#else
void gof_trace_on_switch(uint64_t old_id, uint64_t new_id) {(void)old_id;(void)new_id;}
void gof_trace_on_block(int fd, int ev) {(void)fd;(void)ev;}
void gof_trace_on_unblock(int fd, int ev) {(void)fd;(void)ev;}
#endif
