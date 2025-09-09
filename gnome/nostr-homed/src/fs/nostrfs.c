#include "nostrfs.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int nostrfs_run(const nostrfs_options *opts, int argc, char **argv){
  (void)argc; (void)argv;
  if (!opts || !opts->mountpoint) return -1;
  printf("nostrfs: mountpoint=%s ns=%s cache=%s writeback=%d\n",
         opts->mountpoint,
         opts->namespace_name ? opts->namespace_name : "personal",
         opts->cache_dir ? opts->cache_dir : "/var/cache/nostrfs",
         opts->writeback);
  return 0;
}

int main(int argc, char **argv){
  if (argc < 2) {
    fprintf(stderr, "Usage: %s MOUNTPOINT [--namespace=NAME] [--cache=DIR] [--writeback]\n", argv[0]);
    return 2;
  }
  nostrfs_options o; memset(&o, 0, sizeof(o));
  o.mountpoint = argv[1];
  o.namespace_name = "personal";
  o.cache_dir = "/var/cache/nostrfs";
  o.writeback = 1;
  for (int i=2;i<argc;i++){
    if (strncmp(argv[i], "--namespace=", 12)==0) o.namespace_name = argv[i]+12;
    else if (strncmp(argv[i], "--cache=", 8)==0) o.cache_dir = argv[i]+8;
    else if (strcmp(argv[i], "--writeback")==0) o.writeback = 1;
  }
  return nostrfs_run(&o, argc, argv) == 0 ? 0 : 1;
}
