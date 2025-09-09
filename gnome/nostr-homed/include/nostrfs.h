#ifndef NOSTRFS_H
#define NOSTRFS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  const char *mountpoint;
  const char *namespace_name; /* e.g., "personal" */
  const char *cache_dir;      /* e.g., /var/cache/nostrfs/$uid */
  int writeback;              /* nonzero to enable writes */
} nostrfs_options;

int nostrfs_run(const nostrfs_options *opts, int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* NOSTRFS_H */
