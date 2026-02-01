#include "debug.h"
#include <unistd.h>
#include <string.h>

/* Minimal stub implementations for V1 */

void gof_set_name(const char *name) {
  (void)name; /* nostrc-n63f: V1 stub - fiber naming requires scheduler access */
}

size_t gof_list(gof_info *out, size_t max) {
  (void)out; (void)max; /* nostrc-n63f: V1 stub - fiber tracking requires scheduler instrumentation */
  return 0;
}

void gof_dump_stacks(int fd) {
  const char *msg = "gof_dump_stacks: not implemented in V1\n";
  (void)fd;
  (void)msg; /* silence unused variable warning until implemented */
  /* write(fd, msg, strlen(msg)); */
}
