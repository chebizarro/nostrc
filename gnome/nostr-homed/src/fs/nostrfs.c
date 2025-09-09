/* NOTE: This target is only compiled when FUSE3 is found by CMake. */
#include "nostrfs.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <fuse3/fuse.h>
#include <sys/stat.h>
#include "nostr_manifest.h"
#include "nostr_cache.h"
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

typedef struct {
  nostrfs_options opts;
  nh_manifest manifest;
  int manifest_loaded;
} nostrfs_ctx;

static const char *readme_path = "/README.txt";
static const char *readme_body = "This is nostrfs (readonly, manifest-backed).\n";

static nostrfs_ctx *get_ctx(void){
  return (nostrfs_ctx*)fuse_get_context()->private_data;
}

static int nfs_getattr(const char *path, struct stat *st, struct fuse_file_info *fi){
  (void)fi; memset(st, 0, sizeof(*st));
  if (strcmp(path, "/")==0){
    st->st_mode = S_IFDIR | 0755; st->st_nlink = 2; return 0;
  }
  if (strcmp(path, readme_path)==0){ st->st_mode = S_IFREG | 0444; st->st_nlink = 1; st->st_size = (off_t)strlen(readme_body); return 0; }
  /* Look for a file entry in manifest */
  nostrfs_ctx *ctx = get_ctx();
  if (ctx && ctx->manifest_loaded){
    for (size_t i=0;i<ctx->manifest.entries_len;i++){
      nh_entry *e = &ctx->manifest.entries[i];
      if (e->path && strcmp(e->path, path)==0){
        st->st_mode = S_IFREG | 0444; st->st_nlink = 1; st->st_size = (off_t)e->size; return 0;
      }
    }
    /* If any entry has a prefix path matching a directory, treat as dir */
    size_t plen = strlen(path);
    if (plen>1){
      for (size_t i=0;i<ctx->manifest.entries_len;i++){
        nh_entry *e = &ctx->manifest.entries[i];
        if (!e->path) continue;
        if (strncmp(e->path, path, plen)==0 && e->path[plen]=='/'){
          st->st_mode = S_IFDIR | 0755; st->st_nlink = 2; return 0;
        }
      }
    }
  }
  return -ENOENT;
}

static int nfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t off, struct fuse_file_info *fi, enum fuse_readdir_flags flags){
  (void)off; (void)fi; (void)flags;
  if (strcmp(path, "/")!=0) return -ENOENT;
  filler(buf, ".", NULL, 0, 0);
  filler(buf, "..", NULL, 0, 0);
  filler(buf, readme_path+1, NULL, 0, 0);
  /* List immediate children from manifest */
  nostrfs_ctx *ctx = get_ctx();
  if (ctx && ctx->manifest_loaded){
    size_t base_len = 1; /* root '/' */
    for (size_t i=0;i<ctx->manifest.entries_len;i++){
      nh_entry *e = &ctx->manifest.entries[i];
      if (!e->path || e->path[0]!='/') continue;
      const char *p = e->path + base_len; const char *slash = strchr(p, '/');
      size_t name_len = slash ? (size_t)(slash - p) : strlen(p);
      if (name_len == 0) continue;
      char name[256]; if (name_len >= sizeof name) continue;
      memcpy(name, p, name_len); name[name_len] = '\0';
      filler(buf, name, NULL, 0, 0);
    }
  }
  return 0;
}

static int nfs_open(const char *path, struct fuse_file_info *fi){
  if (strcmp(path, readme_path)==0){ if ((fi->flags & O_ACCMODE) != O_RDONLY) return -EACCES; return 0; }
  /* Only regular files from manifest are openable */
  nostrfs_ctx *ctx = get_ctx();
  if (ctx && ctx->manifest_loaded){
    for (size_t i=0;i<ctx->manifest.entries_len;i++){
      nh_entry *e = &ctx->manifest.entries[i];
      if (e->path && strcmp(e->path, path)==0){ if ((fi->flags & O_ACCMODE) != O_RDONLY) return -EACCES; return 0; }
    }
  }
  return -ENOENT;
}

static int nfs_read(const char *path, char *buf, size_t size, off_t off, struct fuse_file_info *fi){
  (void)fi;
  if (strcmp(path, readme_path)==0){
    size_t len = strlen(readme_body);
    if ((size_t)off >= len) return 0;
    if (off + (off_t)size > (off_t)len) size = (size_t)(len - (size_t)off);
    memcpy(buf, readme_body + off, size); return (int)size;
  }
  /* For now, return a small placeholder line with CID until CAS is wired. */
  nostrfs_ctx *ctx = get_ctx();
  if (ctx && ctx->manifest_loaded){
    for (size_t i=0;i<ctx->manifest.entries_len;i++){
      nh_entry *e = &ctx->manifest.entries[i];
      if (e->path && strcmp(e->path, path)==0){
        /* Attempt CAS-backed read if present: <cache_dir>/<uid>/<cid> */
        if (e->cid && *e->cid){
          uid_t uid = getuid();
          char caspath[1024];
          snprintf(caspath, sizeof caspath, "%s/%u/%s", ctx->opts.cache_dir ? ctx->opts.cache_dir : "/var/cache/nostrfs", (unsigned)uid, e->cid);
          int fd = open(caspath, O_RDONLY);
          if (fd >= 0){
            off_t end = off + (off_t)size; ssize_t rd = 0;
            if (lseek(fd, off, SEEK_SET) == (off_t)-1){ close(fd); return -EIO; }
            rd = read(fd, buf, size);
            close(fd);
            if (rd < 0) return -EIO; return (int)rd;
          }
        }
        char tmp[256]; snprintf(tmp, sizeof tmp, "CID:%s\n", e->cid ? e->cid : "");
        size_t len = strlen(tmp);
        if ((size_t)off >= len) return 0; if (off + (off_t)size > (off_t)len) size = (size_t)(len - (size_t)off);
        memcpy(buf, tmp + off, size); return (int)size;
      }
    }
  }
  return -ENOENT;
}

static const struct fuse_operations nfs_ops = {
  .getattr = nfs_getattr,
  .readdir = nfs_readdir,
  .open    = nfs_open,
  .read    = nfs_read,
};

int nostrfs_run(const nostrfs_options *opts, int argc, char **argv){
  if (!opts || !opts->mountpoint) return -1;
  (void)argc; (void)argv;
  return 0;
}

int main(int argc, char **argv){
  if (argc < 2){
    fprintf(stderr, "Usage: %s MOUNTPOINT [--namespace=NAME] [--cache=DIR] [--writeback]\n", argv[0]);
    return 2;
  }
  nostrfs_ctx ctx; memset(&ctx, 0, sizeof(ctx));
  ctx.opts.mountpoint = argv[1];
  ctx.opts.namespace_name = "personal";
  ctx.opts.cache_dir = "/var/cache/nostrfs";
  ctx.opts.writeback = 0; /* readonly */
  for (int i=2;i<argc;i++){
    if (strncmp(argv[i], "--namespace=", 12)==0) ctx.opts.namespace_name = argv[i]+12;
    else if (strncmp(argv[i], "--cache=", 8)==0) ctx.opts.cache_dir = argv[i]+8;
  }

  /* Load manifest JSON from cache settings */
  ctx.manifest_loaded = 0; memset(&ctx.manifest, 0, sizeof(ctx.manifest));
  nh_cache c; if (nh_cache_open_configured(&c, "/etc/nss_nostr.conf")==0){
    char *json = NULL; size_t cap = 8192; char *bufjson = (char*)malloc(cap);
    if (bufjson){
      if (nh_cache_get_setting(&c, "manifest.personal", bufjson, cap) == 0){
        if (nh_manifest_parse_json(bufjson, &ctx.manifest) == 0) ctx.manifest_loaded = 1;
      }
      free(bufjson);
    }
    nh_cache_close(&c);
  }

  struct fuse_args args = FUSE_ARGS_INIT(0, NULL);
  struct fuse_cmdline_opts fops;
  memset(&fops, 0, sizeof(fops));
  fuse_opt_parse(&args, &fops, NULL, NULL);
  struct fuse *f = fuse_new(&args, &nfs_ops, sizeof(nfs_ops), &ctx);
  if (!f){ fprintf(stderr, "nostrfs: fuse_new failed\n"); fuse_opt_free_args(&args); return 1; }
  if (fuse_mount(f, ctx.opts.mountpoint) != 0){ fprintf(stderr, "nostrfs: mount failed\n"); fuse_destroy(f); fuse_opt_free_args(&args); return 1; }
  int ret = fuse_loop(f);
  fuse_unmount(f);
  fuse_destroy(f);
  fuse_opt_free_args(&args);
  if (ctx.manifest_loaded) nh_manifest_free(&ctx.manifest);
  return ret == 0 ? 0 : 1;
}
