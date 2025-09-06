#include <stdio.h>
#include <time.h>
#include "relayd_ctx.h"
#include "retention.h"

/* Placeholder retention policy: log tick for now. In future, trigger background
 * compaction or TTL deletes via storage driver when exposed. */
void retention_tick(const RelaydCtx *ctx) {
  (void)ctx;
  static time_t last_log = 0;
  time_t now = time(NULL);
  if (last_log == 0 || now - last_log >= 60) {
    fprintf(stderr, "relayd: retention tick\n");
    last_log = now;
  }
}
