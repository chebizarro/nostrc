#include "debug.h"
#include <unistd.h>
#include <string.h>

/* Minimal stub implementations for V1 - see nostrc-l1no for full impl */

void gof_set_name(const char *name) {
  (void)name; /* nostrc-l1no: store into current fiber when accessible */
}

size_t gof_list(gof_info *out, size_t max) {
  (void)out; (void)max; /* nostrc-l1no: track fibers in scheduler */
  return 0;
}

void gof_dump_stacks(int fd) {
  const char *msg = "gof_dump_stacks: not implemented in V1\n";
  (void)fd;
  (void)msg; /* silence unused variable warning until implemented */
  /* write(fd, msg, strlen(msg)); */
}
