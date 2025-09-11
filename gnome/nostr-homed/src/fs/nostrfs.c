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
#include "nostr_blossom.h"
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <jansson.h>
#include <gio/gio.h>
#include "nostr-event.h"
#include "nostr-relay.h"
#include "nostr_dbus.h"
#include "relay_fetch.h"
#include "nostr/nip19/nip19.h"
#include "go.h"
#include "channel.h"
#include "wait_group.h"

typedef struct {
  nostrfs_options opts;
  nh_manifest manifest;
  int manifest_loaded;
  /* libgo-based manifest manager (actor) */
  GoChannel *req_chan; /* channel of manager_req* */
} nostrfs_ctx;

/* --- Helpers (DBus signer and publish) at file scope --- */
static uint64_t now_millis(void){
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

static int decode_npub_hex(const char *npub, char out_hex[65]){
  uint8_t pk[32]; if (nostr_nip19_decode_npub(npub, pk) != 0) return -1;
  static const char *hex = "0123456789abcdef";
  for (int i=0;i<32;i++){ out_hex[i*2]=hex[(pk[i]>>4)&0xF]; out_hex[i*2+1]=hex[pk[i]&0xF]; }
  out_hex[64]='\0'; return 0;
}

static int dbus_get_npub(char **out_npub){
  if (out_npub) *out_npub = NULL; const char *busname = nh_signer_bus_name();
  GError *err=NULL; GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
  if (!bus) { if (err) g_error_free(err); return -1; }
  GVariant *ret = g_dbus_connection_call_sync(bus, busname, "/org/nostr/Signer", "org.nostr.Signer", "GetPublicKey",
                   NULL, G_VARIANT_TYPE_TUPLE, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &err);
  int rc=-1; if (ret){ const char *npub=NULL; g_variant_get(ret, "(s)", &npub); if (npub){ *out_npub = strdup(npub); rc=0; } g_variant_unref(ret);} if (err) g_error_free(err);
  g_object_unref(bus); return rc;
}

static int dbus_sign_event_set_sig(NostrEvent *ev){
  char *json = nostr_event_serialize_compact(ev); if (!json) return -1;
  const char *busname = nh_signer_bus_name();
  GError *err=NULL; GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
  if (!bus){ if (err) g_error_free(err); free(json); return -1; }
  /* current_user/app_id left empty for now */
  GVariant *ret = g_dbus_connection_call_sync(bus, busname, "/org/nostr/Signer", "org.nostr.Signer", "SignEvent",
                   g_variant_new("(sss)", json, "", "nostrfs"), G_VARIANT_TYPE_TUPLE, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &err);
  free(json);
  int rc=-1; if (ret){ const char *sig=NULL; g_variant_get(ret, "(s)", &sig); if (sig){ nostr_event_set_sig(ev, sig); rc=0; } g_variant_unref(ret);} if (err) g_error_free(err);
  g_object_unref(bus); return rc;
}

static void publish_best_effort(const char *content_json){
  static uint64_t last_pub_ms = 0;
  uint64_t now = now_millis();
  if (last_pub_ms != 0 && (now - last_pub_ms) < 200) return; /* debounce */
  last_pub_ms = now;
  /* Build event */
  NostrEvent *ev = nostr_event_new(); if (!ev) return;
  nostr_event_set_kind(ev, 30081);
  nostr_event_set_created_at(ev, (int64_t)time(NULL));
  nostr_event_set_content(ev, content_json);
  char *npub=NULL; if (dbus_get_npub(&npub)==0 && npub){ char pkh[65]; if (decode_npub_hex(npub, pkh)==0) nostr_event_set_pubkey(ev, pkh); free(npub);} else { /* no pubkey; skip */ nostr_event_free(ev); return; }
  if (dbus_sign_event_set_sig(ev) != 0){ nostr_event_free(ev); return; }
  /* Build a base bootstrap relay list from RELAYS_DEFAULT or fallback */
  const char *env = getenv("RELAYS_DEFAULT"); const char *fallback = "wss://relay.damus.io";
  const char *base_list[16]; size_t base_count = 0;
  char buf[1024]; if (env && *env){ strncpy(buf, env, sizeof buf - 1); buf[sizeof buf - 1] = '\0'; }
  else { strncpy(buf, fallback, sizeof buf - 1); buf[sizeof buf - 1] = '\0'; }
  {
    char *saveptr=NULL; char *tok = strtok_r(buf, ",", &saveptr);
    while (tok && base_count < 16){ while (*tok==' ') tok++; if (*tok) base_list[base_count++] = tok; tok = strtok_r(NULL, ",", &saveptr); }
  }
  /* Try to fetch profile relays (30078) using bootstrap relays */
  char **profile_relays = NULL; size_t profile_count = 0;
  if (base_count > 0){ (void)nh_fetch_profile_relays(base_list, base_count, &profile_relays, &profile_count); }
  const char **pub_urls = NULL; size_t pub_count = 0;
  if (profile_count > 0){ pub_urls = (const char **)profile_relays; pub_count = profile_count; }
  else { pub_urls = base_list; pub_count = base_count; }
  for (size_t i=0;i<pub_count;i++){
    const char *url = pub_urls[i]; if (!url || !*url) continue;
    NostrRelay *r = nostr_relay_new(NULL, url, NULL);
    if (r){ if (nostr_relay_connect(r, NULL)) { nostr_relay_publish(r, ev); nostr_relay_close(r, NULL);} nostr_relay_free(r); }
  }
  if (profile_relays){ for (size_t i=0;i<profile_count;i++) free(profile_relays[i]); free(profile_relays); }
  nostr_event_free(ev);
}

static const char *readme_path = "/README.txt";
static const char *readme_body = "This is nostrfs (readonly, manifest-backed).\n";

static nostrfs_ctx *get_ctx(void){
  return (nostrfs_ctx*)fuse_get_context()->private_data;
}

/* --- Manifest manager actor scaffolding (libgo) --- */
typedef enum {
  NFS_REQ_NOP = 0,
  NFS_REQ_COMMIT,
  NFS_REQ_RENAME,
  NFS_REQ_UNLINK,
  NFS_REQ_CHMOD,
  NFS_REQ_CHOWN,
} nfs_req_kind;

typedef struct manager_req {
  nfs_req_kind kind;
  GoChannel *reply; /* replies with an int status code for now */
  /* commit params */
  const char *path;       /* file path in FS, e.g., /foo.txt */
  const char *tmp_path;   /* temp local file to upload */
  uid_t uid;              /* caller uid for CAS path */
  size_t final_size;
  /* rename */
  const char *new_path;
  /* chmod/chown */
  mode_t mode;
  gid_t gid;
} manager_req;

static void *manifest_manager(void *arg){
  nostrfs_ctx *ctx = (nostrfs_ctx*)arg;
  if (!ctx || !ctx->req_chan) return NULL;
  for(;;){
    manager_req *rq = NULL;
    if (go_channel_receive(ctx->req_chan, (void**)&rq) != 0) break;
    int rc = 0;
    if (!rq){ rc = -1; goto reply; }
    if (rq->kind == NFS_REQ_COMMIT){
      const char *base = getenv("BLOSSOM_BASE_URL"); if (!base || !*base) base = "https://blossom.example.org";
      char *cid = NULL;
      if (nh_blossom_upload(base, rq->tmp_path, &cid) != 0){ rc = -EIO; goto reply; }
      /* Move file into CAS path: cache_dir/uid/cid */
      char casdir[512]; snprintf(casdir, sizeof casdir, "%s/%u", ctx->opts.cache_dir ? ctx->opts.cache_dir : "/var/cache/nostrfs", (unsigned)rq->uid);
      char caspath[1024]; snprintf(caspath, sizeof caspath, "%s/%s", casdir, cid);
      /* Ensure dir exists */
      (void)mkdir(ctx->opts.cache_dir ? ctx->opts.cache_dir : "/var/cache/nostrfs", 0700);
      (void)mkdir(casdir, 0700);
      if (rename(rq->tmp_path, caspath) != 0){ rc = -EIO; free(cid); goto reply; }
      /* Update manifest: find or append entry for rq->path */
      if (!ctx->manifest_loaded){ memset(&ctx->manifest, 0, sizeof(ctx->manifest)); ctx->manifest.version = 2; ctx->manifest_loaded = 1; }
      nh_entry *e = NULL;
      for (size_t i=0;i<ctx->manifest.entries_len;i++){
        if (ctx->manifest.entries[i].path && strcmp(ctx->manifest.entries[i].path, rq->path)==0){ e = &ctx->manifest.entries[i]; break; }
      }
      if (!e){
        size_t n = ctx->manifest.entries_len + 1;
        nh_entry *neu = (nh_entry*)realloc(ctx->manifest.entries, n * sizeof(nh_entry));
        if (!neu){ rc = -ENOMEM; free(cid); goto reply; }
        ctx->manifest.entries = neu; e = &ctx->manifest.entries[ctx->manifest.entries_len]; ctx->manifest.entries_len = n;
        memset(e, 0, sizeof(*e)); e->path = strdup(rq->path);
      }
      if (e->cid) free(e->cid); e->cid = cid; e->size = rq->final_size; e->mode = 0644; e->uid = (uint32_t)rq->uid; e->gid = (uint32_t)getgid(); e->mtime = (uint64_t)time(NULL);
      /* Persist manifest to cache settings as compact JSON */
      json_t *root = json_object(); json_object_set_new(root, "version", json_integer(2));
      json_t *arr = json_array();
      for (size_t i=0;i<ctx->manifest.entries_len;i++){
        nh_entry *ee = &ctx->manifest.entries[i]; if (!ee->path || !ee->cid) continue;
        json_t *o = json_object();
        json_object_set_new(o, "path", json_string(ee->path));
        json_object_set_new(o, "cid", json_string(ee->cid));
        json_object_set_new(o, "size", json_integer((json_int_t)ee->size));
        json_object_set_new(o, "mode", json_integer((json_int_t)ee->mode));
        json_object_set_new(o, "uid", json_integer((json_int_t)ee->uid));
        json_object_set_new(o, "gid", json_integer((json_int_t)ee->gid));
        json_object_set_new(o, "mtime", json_integer((json_int_t)ee->mtime));
        json_array_append_new(arr, o);
      }
      json_object_set_new(root, "entries", arr);
      json_object_set_new(root, "links", json_array());
      char *dump = json_dumps(root, JSON_COMPACT);
      json_decref(root);
      if (dump){
        nh_cache cset; if (nh_cache_open_configured(&cset, "/etc/nss_nostr.conf")==0){
          nh_cache_set_setting(&cset, "manifest.personal", dump);
          nh_cache_close(&cset);
        }
        /* Best-effort publish replaceable manifest */
        publish_best_effort(dump);
        free(dump);
      }
      /* Publish latest 30081 can be triggered here (best-effort) */
      rc = 0;
    }
    else if (rq->kind == NFS_REQ_RENAME){
      if (!ctx->manifest_loaded){ rc = -ENOENT; goto reply; }
      nh_entry *e = NULL;
      for (size_t i=0;i<ctx->manifest.entries_len;i++) if (ctx->manifest.entries[i].path && strcmp(ctx->manifest.entries[i].path, rq->path)==0){ e=&ctx->manifest.entries[i]; break; }
      if (!e){ rc = -ENOENT; goto reply; }
      free(e->path); e->path = strdup(rq->new_path);
      /* Persist manifest */
      json_t *root = json_object(); json_object_set_new(root, "version", json_integer(2));
      json_t *arr = json_array();
      for (size_t i=0;i<ctx->manifest.entries_len;i++){
        nh_entry *ee = &ctx->manifest.entries[i]; if (!ee->path || !ee->cid) continue;
        json_t *o = json_object();
        json_object_set_new(o, "path", json_string(ee->path));
        json_object_set_new(o, "cid", json_string(ee->cid));
        json_object_set_new(o, "size", json_integer((json_int_t)ee->size));
        json_object_set_new(o, "mode", json_integer((json_int_t)ee->mode));
        json_object_set_new(o, "uid", json_integer((json_int_t)ee->uid));
        json_object_set_new(o, "gid", json_integer((json_int_t)ee->gid));
        json_object_set_new(o, "mtime", json_integer((json_int_t)ee->mtime));
        json_array_append_new(arr, o);
      }
      json_object_set_new(root, "entries", arr);
      json_object_set_new(root, "links", json_array());
      char *dump = json_dumps(root, JSON_COMPACT); json_decref(root);
      if (dump){ nh_cache cset; if (nh_cache_open_configured(&cset, "/etc/nss_nostr.conf")==0){ nh_cache_set_setting(&cset, "manifest.personal", dump); nh_cache_close(&cset);} free(dump);} rc = 0;
    }
    else if (rq->kind == NFS_REQ_UNLINK){
      if (!ctx->manifest_loaded){ rc = -ENOENT; goto reply; }
      size_t idx = (size_t)-1;
      for (size_t i=0;i<ctx->manifest.entries_len;i++) if (ctx->manifest.entries[i].path && strcmp(ctx->manifest.entries[i].path, rq->path)==0){ idx=i; break; }
      if (idx==(size_t)-1){ rc = -ENOENT; goto reply; }
      nh_entry *ee = &ctx->manifest.entries[idx]; free(ee->path); free(ee->cid);
      memmove(&ctx->manifest.entries[idx], &ctx->manifest.entries[idx+1], (ctx->manifest.entries_len-idx-1)*sizeof(nh_entry));
      ctx->manifest.entries_len--; if (ctx->manifest.entries_len==0){ free(ctx->manifest.entries); ctx->manifest.entries=NULL; }
      /* Persist */
      json_t *root = json_object(); json_object_set_new(root, "version", json_integer(2)); json_t *arr = json_array();
      for (size_t i=0;i<ctx->manifest.entries_len;i++){
        nh_entry *e2 = &ctx->manifest.entries[i]; if (!e2->path || !e2->cid) continue; json_t *o=json_object();
        json_object_set_new(o, "path", json_string(e2->path)); json_object_set_new(o, "cid", json_string(e2->cid));
        json_object_set_new(o, "size", json_integer((json_int_t)e2->size)); json_object_set_new(o, "mode", json_integer((json_int_t)e2->mode));
        json_object_set_new(o, "uid", json_integer((json_int_t)e2->uid)); json_object_set_new(o, "gid", json_integer((json_int_t)e2->gid));
        json_object_set_new(o, "mtime", json_integer((json_int_t)e2->mtime)); json_array_append_new(arr, o);
      }
      json_object_set_new(root, "entries", arr); json_object_set_new(root, "links", json_array()); char *dump=json_dumps(root, JSON_COMPACT); json_decref(root);
      if (dump){ nh_cache cset; if (nh_cache_open_configured(&cset, "/etc/nss_nostr.conf")==0){ nh_cache_set_setting(&cset, "manifest.personal", dump); nh_cache_close(&cset);} free(dump);} rc=0;
    }
    else if (rq->kind == NFS_REQ_CHMOD){
      if (!ctx->manifest_loaded){ rc = -ENOENT; goto reply; }
      nh_entry *e = NULL; for (size_t i=0;i<ctx->manifest.entries_len;i++) if (ctx->manifest.entries[i].path && strcmp(ctx->manifest.entries[i].path, rq->path)==0){ e=&ctx->manifest.entries[i]; break; }
      if (!e){ rc = -ENOENT; goto reply; }
      e->mode = (uint32_t)rq->mode; e->mtime = (uint64_t)time(NULL);
      json_t *root = json_object(); json_object_set_new(root, "version", json_integer(2)); json_t *arr = json_array();
      for (size_t i=0;i<ctx->manifest.entries_len;i++){
        nh_entry *e2 = &ctx->manifest.entries[i]; if (!e2->path || !e2->cid) continue; json_t *o=json_object();
        json_object_set_new(o, "path", json_string(e2->path)); json_object_set_new(o, "cid", json_string(e2->cid));
        json_object_set_new(o, "size", json_integer((json_int_t)e2->size)); json_object_set_new(o, "mode", json_integer((json_int_t)e2->mode));
        json_object_set_new(o, "uid", json_integer((json_int_t)e2->uid)); json_object_set_new(o, "gid", json_integer((json_int_t)e2->gid));
        json_object_set_new(o, "mtime", json_integer((json_int_t)e2->mtime)); json_array_append_new(arr, o);
      }
      json_object_set_new(root, "entries", arr); json_object_set_new(root, "links", json_array()); char *dump=json_dumps(root, JSON_COMPACT); json_decref(root);
      if (dump){ nh_cache cset; if (nh_cache_open_configured(&cset, "/etc/nss_nostr.conf")==0){ nh_cache_set_setting(&cset, "manifest.personal", dump); nh_cache_close(&cset);} free(dump);} rc=0;
    }
    else if (rq->kind == NFS_REQ_CHOWN){
      if (!ctx->manifest_loaded){ rc = -ENOENT; goto reply; }
      nh_entry *e = NULL; for (size_t i=0;i<ctx->manifest.entries_len;i++) if (ctx->manifest.entries[i].path && strcmp(ctx->manifest.entries[i].path, rq->path)==0){ e=&ctx->manifest.entries[i]; break; }
      if (!e){ rc = -ENOENT; goto reply; }
      e->uid = (uint32_t)rq->uid; e->gid = (uint32_t)rq->gid; e->mtime = (uint64_t)time(NULL);
      json_t *root = json_object(); json_object_set_new(root, "version", json_integer(2)); json_t *arr = json_array();
      for (size_t i=0;i<ctx->manifest.entries_len;i++){
        nh_entry *e2 = &ctx->manifest.entries[i]; if (!e2->path || !e2->cid) continue; json_t *o=json_object();
        json_object_set_new(o, "path", json_string(e2->path)); json_object_set_new(o, "cid", json_string(e2->cid));
        json_object_set_new(o, "size", json_integer((json_int_t)e2->size)); json_object_set_new(o, "mode", json_integer((json_int_t)e2->mode));
        json_object_set_new(o, "uid", json_integer((json_int_t)e2->uid)); json_object_set_new(o, "gid", json_integer((json_int_t)e2->gid));
        json_object_set_new(o, "mtime", json_integer((json_int_t)e2->mtime)); json_array_append_new(arr, o);
      }
      json_object_set_new(root, "entries", arr); json_object_set_new(root, "links", json_array()); char *dump=json_dumps(root, JSON_COMPACT); json_decref(root);
      if (dump){ nh_cache cset; if (nh_cache_open_configured(&cset, "/etc/nss_nostr.conf")==0){ nh_cache_set_setting(&cset, "manifest.personal", dump); nh_cache_close(&cset);} free(dump);} rc=0;
    }
reply:
    if (rq && rq->reply){ (void)go_channel_send(rq->reply, (void*)(intptr_t)rc); }
    if (rq) free(rq);
  }
  return NULL;
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

/* --- Write-back path (create/write/flush/release) --- */
typedef struct nfs_write_handle {
  char *path;      /* FS path */
  char *tmp_path;  /* temp file path */
  int fd;
  size_t size;
  int dirty;
} nfs_write_handle;

static int nfs_create(const char *path, mode_t mode, struct fuse_file_info *fi){
  nostrfs_ctx *ctx = get_ctx(); if (!ctx || !ctx->opts.writeback) return -EACCES;
  (void)mode;
  /* Create temp file in cache/tmp */
  uid_t uid = getuid();
  char tmpdir[512]; snprintf(tmpdir, sizeof tmpdir, "%s/%u/tmp", ctx->opts.cache_dir ? ctx->opts.cache_dir : "/var/cache/nostrfs", (unsigned)uid);
  (void)mkdir(ctx->opts.cache_dir ? ctx->opts.cache_dir : "/var/cache/nostrfs", 0700);
  (void)mkdir(tmpdir, 0700);
  char tmppath[1024]; snprintf(tmppath, sizeof tmppath, "%s/obj.XXXXXX", tmpdir);
  int fd = mkstemp(tmppath); if (fd < 0) return -EIO;
  nfs_write_handle *h = (nfs_write_handle*)calloc(1, sizeof(*h)); if (!h){ close(fd); unlink(tmppath); return -ENOMEM; }
  h->path = strdup(path); h->tmp_path = strdup(tmppath); h->fd = fd; h->size = 0; h->dirty = 0;
  fi->fh = (uint64_t)(uintptr_t)h;
  return 0;
}

static int nfs_write(const char *path, const char *buf, size_t size, off_t off, struct fuse_file_info *fi){
  (void)path; nfs_write_handle *h = (nfs_write_handle*)(uintptr_t)fi->fh; if (!h) return -EBADF;
  if (lseek(h->fd, off, SEEK_SET) == (off_t)-1) return -EIO;
  ssize_t wr = write(h->fd, buf, size);
  if (wr < 0) return -EIO;
  size_t end = (size_t)(off + wr); if (end > h->size) h->size = end; h->dirty = 1; return (int)wr;
}

static int nfs_flush(const char *path, struct fuse_file_info *fi){
  (void)path; nfs_write_handle *h = (nfs_write_handle*)(uintptr_t)fi->fh; if (!h) return 0;
  if (!h->dirty) return 0;
  /* Ask actor to commit */
  nostrfs_ctx *ctx = get_ctx(); if (!ctx || !ctx->req_chan) return -EIO;
  fsync(h->fd); /* ensure data on disk before moving */
  close(h->fd); h->fd = -1;
  manager_req *rq = (manager_req*)calloc(1, sizeof(*rq)); if (!rq) return -ENOMEM;
  rq->kind = NFS_REQ_COMMIT; rq->path = h->path; rq->tmp_path = h->tmp_path; rq->uid = getuid(); rq->final_size = h->size;
  GoChannel *reply = go_channel_create(1); rq->reply = reply;
  go_channel_send(ctx->req_chan, rq);
  int rc = (int)(intptr_t)({ void *v=NULL; go_channel_receive(reply, &v)==0 ? v : (void*)(intptr_t)-EIO; });
  go_channel_close(reply); go_channel_free(reply);
  if (rc == 0){ h->dirty = 0; }
  return rc;
}

static void free_write_handle(nfs_write_handle *h){ if (!h) return; if (h->fd>=0) close(h->fd); if (h->tmp_path) { unlink(h->tmp_path); free(h->tmp_path);} free(h->path); free(h); }

static int nfs_release(const char *path, struct fuse_file_info *fi){
  (void)path; nfs_write_handle *h = (nfs_write_handle*)(uintptr_t)fi->fh; if (!h) return 0;
  int rc = 0; if (h->dirty){ rc = nfs_flush(path, fi); }
  free_write_handle(h); fi->fh = 0; return rc;
}

static int nfs_rename(const char *from, const char *to, unsigned int flags){
  (void)flags; nostrfs_ctx *ctx = get_ctx(); if (!ctx || !ctx->opts.writeback) return -EACCES;
  if (!ctx->req_chan) return -EIO;
  manager_req *rq = (manager_req*)calloc(1, sizeof(*rq)); if (!rq) return -ENOMEM;
  rq->kind = NFS_REQ_RENAME; rq->path = from; rq->new_path = to;
  GoChannel *reply = go_channel_create(1); rq->reply = reply;
  go_channel_send(ctx->req_chan, rq);
  int rc = (int)(intptr_t)({ void *v=NULL; go_channel_receive(reply, &v)==0 ? v : (void*)(intptr_t)-EIO; });
  go_channel_close(reply); go_channel_free(reply);
  return rc;
}

static int nfs_unlink(const char *path){
  nostrfs_ctx *ctx = get_ctx(); if (!ctx || !ctx->opts.writeback) return -EACCES;
  if (!ctx->req_chan) return -EIO;
  manager_req *rq = (manager_req*)calloc(1, sizeof(*rq)); if (!rq) return -ENOMEM;
  rq->kind = NFS_REQ_UNLINK; rq->path = path;
  GoChannel *reply = go_channel_create(1); rq->reply = reply;
  go_channel_send(ctx->req_chan, rq);
  int rc = (int)(intptr_t)({ void *v=NULL; go_channel_receive(reply, &v)==0 ? v : (void*)(intptr_t)-EIO; });
  go_channel_close(reply); go_channel_free(reply);
  return rc;
}

static int nfs_chmod(const char *path, mode_t mode, struct fuse_file_info *fi){
  (void)fi; nostrfs_ctx *ctx = get_ctx(); if (!ctx || !ctx->opts.writeback) return -EACCES;
  if (!ctx->req_chan) return -EIO;
  manager_req *rq = (manager_req*)calloc(1, sizeof(*rq)); if (!rq) return -ENOMEM;
  rq->kind = NFS_REQ_CHMOD; rq->path = path; rq->mode = mode;
  GoChannel *reply = go_channel_create(1); rq->reply = reply;
  go_channel_send(ctx->req_chan, rq);
  int rc = (int)(intptr_t)({ void *v=NULL; go_channel_receive(reply, &v)==0 ? v : (void*)(intptr_t)-EIO; });
  go_channel_close(reply); go_channel_free(reply);
  return rc;
}

static int nfs_chown(const char *path, uid_t uid, gid_t gid, struct fuse_file_info *fi){
  (void)fi; nostrfs_ctx *ctx = get_ctx(); if (!ctx || !ctx->opts.writeback) return -EACCES;
  if (!ctx->req_chan) return -EIO;
  manager_req *rq = (manager_req*)calloc(1, sizeof(*rq)); if (!rq) return -ENOMEM;
  rq->kind = NFS_REQ_CHOWN; rq->path = path; rq->uid = uid; rq->gid = gid;
  GoChannel *reply = go_channel_create(1); rq->reply = reply;
  go_channel_send(ctx->req_chan, rq);
  int rc = (int)(intptr_t)({ void *v=NULL; go_channel_receive(reply, &v)==0 ? v : (void*)(intptr_t)-EIO; });
  go_channel_close(reply); go_channel_free(reply);
  return rc;
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
      if (e->path && strcmp(e->path, path)==0){
        if ((fi->flags & O_ACCMODE) != O_RDONLY){ if (!ctx->opts.writeback) return -EACCES; }
        return 0;
      }
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
          /* On-demand fetch from Blossom into CAS, then retry */
          const char *base = getenv("BLOSSOM_BASE_URL");
          if (!base || !*base) base = "https://blossom.example.org";
          if (nh_blossom_head(base, e->cid) == 0){
            if (nh_blossom_fetch(base, e->cid, caspath) == 0){
              int fd2 = open(caspath, O_RDONLY);
              if (fd2 >= 0){
                if (lseek(fd2, off, SEEK_SET) == (off_t)-1){ close(fd2); return -EIO; }
                ssize_t rd2 = read(fd2, buf, size);
                close(fd2);
                if (rd2 < 0) return -EIO; return (int)rd2;
              }
            }
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
  .create  = nfs_create,
  .write   = nfs_write,
  .flush   = nfs_flush,
  .release = nfs_release,
  .rename  = nfs_rename,
  .unlink  = nfs_unlink,
  .chmod   = nfs_chmod,
  .chown   = nfs_chown,
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

  /* Start manifest manager actor */
  ctx.req_chan = go_channel_create(64);
  go(manifest_manager, &ctx);

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
  if (ctx.req_chan){ go_channel_close(ctx.req_chan); go_channel_free(ctx.req_chan); ctx.req_chan=NULL; }
  if (ctx.manifest_loaded) nh_manifest_free(&ctx.manifest);
  return ret == 0 ? 0 : 1;
}
