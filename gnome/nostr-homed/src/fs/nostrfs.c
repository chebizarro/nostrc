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
#include <openssl/evp.h>
#include <sys/statvfs.h>
#include <dirent.h>
#include "nostr-event.h"
#include "nostr-tag.h"
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
  /* worker pool for Blossom uploads */
  GoChannel *upload_q; /* channel of upload_req* */
  /* last persisted manifest json for final publish */
  char *last_manifest_json;
  /* worker pool for Blossom downloads */
  GoChannel *download_q; /* channel of download_req* */
  /* coalesced publish generation counters */
  uint64_t pub_gen;
  uint64_t pub_sent_gen;
} nostrfs_ctx;

/* Upload queue message types */
typedef struct upload_req {
  char *base_url;     /* BLOSSOM base URL */
  char *tmp_path;     /* temp file path to upload */
  GoChannel *reply;   /* replies with upload_res* */
} upload_req;

typedef struct upload_res {
  int rc;             /* 0 on success */
  char *cid;          /* allocated CID hex on success */
} upload_res;

static void *upload_worker(void *arg){
  nostrfs_ctx *ctx = (nostrfs_ctx*)arg;
  if (!ctx || !ctx->upload_q) return NULL;
  for(;;){
    upload_req *ur = NULL;
    if (go_channel_receive(ctx->upload_q, (void**)&ur) != 0) break;
    if (!ur) continue;
    upload_res *res = (upload_res*)calloc(1, sizeof(*res));
    if (!res){
      if (ur->reply) (void)go_channel_send(ur->reply, (void*)(intptr_t)-ENOMEM);
      free(ur->base_url); free(ur); continue;
    }

/* Persist manifest JSON under settings.manifest.<namespace> using a simple safe-replace strategy. */
static void persist_manifest_ns(nostrfs_ctx *ctx, const char *dump){
  if (!ctx || !dump) return;
  nh_cache cset;
  if (nh_cache_open_configured(&cset, "/etc/nss_nostr.conf")==0){
    char key_final[128];
    char key_tmp[128];
    snprintf(key_final, sizeof key_final, "manifest.%s", ctx->opts.namespace_name ? ctx->opts.namespace_name : "personal");
    snprintf(key_tmp, sizeof key_tmp, "%s.tmp", key_final);
    /* Write tmp first, then final key to reduce partial read windows */
    (void)nh_cache_set_setting(&cset, key_tmp, dump);
    (void)nh_cache_set_setting(&cset, key_final, dump);
    nh_cache_close(&cset);
  }
  if (ctx->last_manifest_json){ free(ctx->last_manifest_json); ctx->last_manifest_json = NULL; }
  ctx->last_manifest_json = strdup(dump);
  ctx->pub_gen++;
}
    char *cid = NULL;
    int rc = nh_blossom_upload(ur->base_url ? ur->base_url : "https://blossom.example.org", ur->tmp_path, &cid);
    res->rc = rc; res->cid = cid;
    if (ur->reply){ (void)go_channel_send(ur->reply, res); }
    else { if (cid) free(cid); free(res); }
    free(ur->base_url);
    free(ur);
  }
  return NULL;
}

/* Compute SHA-256 of a file and write lowercase hex into out_hex[65]; returns 0 on success */
static int sha256_file_hex_local(const char *path, char out_hex[65]){
  int ret = -1; FILE *f = fopen(path, "rb"); if (!f) return -1;
  EVP_MD_CTX *mdctx = EVP_MD_CTX_new(); if (!mdctx){ fclose(f); return -1; }
  const EVP_MD *md = EVP_sha256(); if (!md){ EVP_MD_CTX_free(mdctx); fclose(f); return -1; }
  if (EVP_DigestInit_ex(mdctx, md, NULL) != 1){ EVP_MD_CTX_free(mdctx); fclose(f); return -1; }
  unsigned char buf[8192]; size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0){ if (EVP_DigestUpdate(mdctx, buf, n) != 1){ goto done; } }
  if (ferror(f)) goto done;
  unsigned char mdval[EVP_MAX_MD_SIZE]; unsigned int mdlen = 0;
  if (EVP_DigestFinal_ex(mdctx, mdval, &mdlen) != 1) goto done;
  static const char *hex = "0123456789abcdef";
  for (unsigned int i=0;i<mdlen;i++){ out_hex[i*2]=hex[(mdval[i]>>4)&0xF]; out_hex[i*2+1]=hex[mdval[i]&0xF]; }
  out_hex[mdlen*2]='\0'; ret = 0;
done:
  EVP_MD_CTX_free(mdctx); fclose(f); return ret;
}

/* --- Helpers (DBus signer and publish) at file scope --- */
static uint64_t now_millis(void){
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

/* --- CAS quota enforcement (simple LRU by mtime) --- */
static uint64_t get_cas_max_bytes(void){
  const char *b = getenv("NOSTRFS_CAS_MAX_BYTES");
  if (b && *b){ unsigned long long v = strtoull(b, NULL, 10); if (v > 0) return (uint64_t)v; }
  const char *mb = getenv("NOSTRFS_CAS_MAX_MB");
  if (mb && *mb){ unsigned long long v = strtoull(mb, NULL, 10); if (v > 0) return (uint64_t)v * 1024ull * 1024ull; }
  return 512ull * 1024ull * 1024ull; /* default 512MB */
}

typedef struct cas_file_entry {
  char path[1024];
  off_t size;
  time_t mtime;
} cas_file_entry;

static void enforce_cas_quota(nostrfs_ctx *ctx, const char *cas_dir){
  if (!ctx || !cas_dir) return;
  uint64_t maxb = get_cas_max_bytes();
  DIR *d = opendir(cas_dir); if (!d) return;
  cas_file_entry *list = NULL; size_t n=0, cap=0; uint64_t total=0;
  struct dirent *de;
  while ((de = readdir(d)) != NULL){
    if (de->d_name[0]=='.') continue;
    char p[1024]; snprintf(p, sizeof p, "%s/%s", cas_dir, de->d_name);
    struct stat st; if (stat(p, &st) != 0) continue;
    if (!S_ISREG(st.st_mode)) continue;
    if (n==cap){ cap = cap?cap*2:64; list = (cas_file_entry*)realloc(list, cap*sizeof(*list)); if (!list){ closedir(d); return; } }
    strncpy(list[n].path, p, sizeof list[n].path - 1); list[n].path[sizeof list[n].path - 1] = '\0';
    list[n].size = st.st_size; list[n].mtime = st.st_mtime; total += (uint64_t)st.st_size; n++;
  }
  closedir(d);
  if (total <= maxb){ free(list); return; }
  /* Evict oldest by mtime until under */
  while (total > maxb && n > 0){
    size_t oldest = 0; for (size_t i=1;i<n;i++) if (list[i].mtime < list[oldest].mtime) oldest = i;
    if (unlink(list[oldest].path) == 0){ total -= (uint64_t)list[oldest].size; }
    /* Remove from array */
    if (oldest != n-1){ list[oldest] = list[n-1]; }
    n--;
  }
  free(list);
}

/* Simple throttled stderr logger (one message per key at most every 5 seconds) */
static void warn_throttle(const char *key, const char *msg){
  static uint64_t last_ms_db = 0, last_ms_sig = 0, last_ms_pub = 0, last_ms_up = 0;
  uint64_t *slot = NULL;
  if (strcmp(key, "dbus") == 0) slot = &last_ms_db;
  else if (strcmp(key, "sign") == 0) slot = &last_ms_sig;
  else if (strcmp(key, "pub") == 0) slot = &last_ms_pub;
  else if (strcmp(key, "upl") == 0) slot = &last_ms_up;
  else return;
  uint64_t now = now_millis();
  if (*slot == 0 || now - *slot > 5000){
    fprintf(stderr, "[nostrfs][warn][%s] %s\n", key, msg);
    *slot = now;
  }
}

/* Basic path sanitizer: require absolute path, disallow //, /./, /../ components */
static int valid_path(const char *path){
  if (!path || path[0] != '/') return 0;
  const char *p = path;
  while (*p){
    if (p[0]=='/' && p[1]=='/') return 0;
    if (p[0]=='/' && p[1]=='.' && (p[2]=='/' || p[2]=='\0')) return 0;
    if (p[0]=='/' && p[1]=='.' && p[2]=='.' && (p[3]=='/' || p[3]=='\0')) return 0;
    p++;
  }
  return 1;
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
  if (!bus) { if (err) g_error_free(err); warn_throttle("dbus", "failed to connect to session bus for signer"); return -1; }
  GVariant *ret = g_dbus_connection_call_sync(bus, busname, "/org/nostr/Signer", "org.nostr.Signer", "GetPublicKey",
                   NULL, G_VARIANT_TYPE_TUPLE, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &err);
  int rc=-1; if (ret){ const char *npub=NULL; g_variant_get(ret, "(s)", &npub); if (npub){ *out_npub = strdup(npub); rc=0; } g_variant_unref(ret);} if (err){ warn_throttle("dbus", "GetPublicKey failed"); g_error_free(err);} 
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
  int rc=-1; if (ret){ const char *sig=NULL; g_variant_get(ret, "(s)", &sig); if (sig){ nostr_event_set_sig(ev, sig); rc=0; } g_variant_unref(ret);} if (err){ warn_throttle("sign", "SignEvent failed"); g_error_free(err);} 
  g_object_unref(bus); return rc;
}

static void publish_best_effort(nostrfs_ctx *ctx, const char *content_json){
  static uint64_t last_pub_ms = 0;
  uint64_t now = now_millis();
  if (last_pub_ms != 0 && (now - last_pub_ms) < 200) return; /* debounce */
  last_pub_ms = now;
  /* Build event */
  NostrEvent *ev = nostr_event_new(); if (!ev) return;
  nostr_event_set_kind(ev, 30081);
  nostr_event_set_created_at(ev, (int64_t)time(NULL));
  nostr_event_set_content(ev, content_json);
  /* Add namespace tag: ["d", namespace] */
  if (ctx && ctx->opts.namespace_name){
    NostrTag *t = nostr_tag_new("d", ctx->opts.namespace_name, NULL);
    if (t){ NostrTags *tags = nostr_tags_new(1, t); if (tags) nostr_event_set_tags(ev, tags); }
  }
  char *npub=NULL; if (dbus_get_npub(&npub)==0 && npub){ char pkh[65]; if (decode_npub_hex(npub, pkh)==0) nostr_event_set_pubkey(ev, pkh); free(npub);} else { /* no pubkey; skip */ nostr_event_free(ev); return; }
  if (dbus_sign_event_set_sig(ev) != 0){ nostr_event_free(ev); return; }
  /* Build a base bootstrap relay list from RELAYS_DEFAULT or fallback */
  const char *env = getenv("RELAYS_DEFAULT"); const char *fallback = "wss://relay.damus.io,wss://nostr.wine";
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
  /* Publish with simple retry/backoff */
  int attempts = 0, max_attempts = 3;
  while (attempts < max_attempts){
    int published_any = 0;
    for (size_t i=0;i<pub_count;i++){
      const char *url = pub_urls[i]; if (!url || !*url) continue;
      NostrRelay *r = nostr_relay_new(NULL, url, NULL);
      if (r){ if (nostr_relay_connect(r, NULL)) { nostr_relay_publish(r, ev); nostr_relay_close(r, NULL); published_any = 1; } nostr_relay_free(r); }
    }
    if (published_any) break;
    attempts++;
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 200 * 1000000L }; /* 200ms */
    nanosleep(&ts, NULL);
  }
  if (attempts >= max_attempts){ warn_throttle("pub", "publish attempts exhausted"); }
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
  NFS_REQ_MKDIR,
  NFS_REQ_RMDIR,
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
      /* Enqueue upload to worker pool */
      upload_req *ur = (upload_req*)calloc(1, sizeof(*ur)); if (!ur){ rc = -ENOMEM; goto reply; }
      ur->base_url = strdup(base); ur->tmp_path = (char*)rq->tmp_path; /* borrow */
      GoChannel *ureply = go_channel_create(1); ur->reply = ureply;
      go_channel_send(ctx->upload_q, ur);
      void *vres = NULL; if (go_channel_receive(ureply, &vres) != 0){ go_channel_close(ureply); go_channel_free(ureply); rc = -EIO; goto reply; }
      go_channel_close(ureply); go_channel_free(ureply);
      upload_res *res = (upload_res*)vres;
      if (!res){ rc = -EIO; goto reply; }
      if (res->rc != 0){ free(res->cid); free(res); rc = -EIO; goto reply; }
      char *cid = res->cid; free(res);
      /* Move file into CAS path: cache_dir/uid/cid */
      char casdir[512]; snprintf(casdir, sizeof casdir, "%s/%u", ctx->opts.cache_dir ? ctx->opts.cache_dir : "/var/cache/nostrfs", (unsigned)rq->uid);
      char caspath[1024]; snprintf(caspath, sizeof caspath, "%s/%s", casdir, cid);
      /* Ensure dir exists */
      (void)mkdir(ctx->opts.cache_dir ? ctx->opts.cache_dir : "/var/cache/nostrfs", 0700);
      (void)mkdir(casdir, 0700);
      if (rename(rq->tmp_path, caspath) != 0){ rc = -EIO; free(cid); goto reply; }
      /* Enforce CAS quota after adding new object */
      enforce_cas_quota(ctx, casdir);
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
      if (dump){ persist_manifest_ns(ctx, dump); free(dump); }
      /* Publish latest 30081 can be triggered here (best-effort) */
      rc = 0;
    }
    else if (rq->kind == NFS_REQ_RENAME){
      if (!ctx->manifest_loaded){ rc = -ENOENT; goto reply; }
      nh_entry *e = NULL;
      for (size_t i=0;i<ctx->manifest.entries_len;i++) if (ctx->manifest.entries[i].path && strcmp(ctx->manifest.entries[i].path, rq->path)==0){ e=&ctx->manifest.entries[i]; break; }
      if (!e){ rc = -ENOENT; goto reply; }
      /* Directory rename must update children */
      const char *oldp = rq->path; const char *newp = rq->new_path;
      size_t oldlen = strlen(oldp);
      int is_dir = ((e->mode & S_IFDIR) == S_IFDIR);
      if (is_dir){
        /* prevent moving directory into its own subtree */
        size_t newlen = strlen(newp);
        if (newlen > oldlen && strncmp(newp, oldp, oldlen)==0 && newp[oldlen]=='/') { rc = -EINVAL; goto reply; }
        /* Update the directory entry itself */
        free(e->path); e->path = strdup(newp); e->mtime = (uint64_t)time(NULL);
        /* Update children prefix: oldp + '/' */
        char oldpref[512]; snprintf(oldpref, sizeof oldpref, "%s/", oldp);
        char newpref[512]; snprintf(newpref, sizeof newpref, "%s/", newp);
        size_t oldpreflen = strlen(oldpref);
        for (size_t i=0;i<ctx->manifest.entries_len;i++){
          nh_entry *c = &ctx->manifest.entries[i]; if (!c->path) continue;
          if (strncmp(c->path, oldpref, oldpreflen)==0){
            size_t tail_len = strlen(c->path) - oldpreflen;
            char *np = (char*)malloc(strlen(newpref) + tail_len + 1);
            if (!np){ rc = -ENOMEM; goto reply; }
            memcpy(np, newpref, strlen(newpref)); memcpy(np+strlen(newpref), c->path+oldpreflen, tail_len+1);
            free(c->path); c->path = np; c->mtime = (uint64_t)time(NULL);
          }
        }
      } else {
        free(e->path); e->path = strdup(rq->new_path); e->mtime = (uint64_t)time(NULL);
      }
      /* Persist manifest */
      json_t *root = json_object(); json_object_set_new(root, "version", json_integer(2));
      json_t *arr = json_array();
      for (size_t i=0;i<ctx->manifest.entries_len;i++){
        nh_entry *ee = &ctx->manifest.entries[i]; if (!ee->path || (!ee->cid && (ee->mode & S_IFDIR)!=S_IFDIR)) continue;
        json_t *o = json_object();
        json_object_set_new(o, "path", json_string(ee->path));
        json_object_set_new(o, "cid", json_string(ee->cid));
        json_object_set_new(o, "size", json_integer((json_int_t)ee->size));
        json_object_set_new(o, "mode", json_integer((json_int_t)ee->mode));
      }
      json_object_set_new(root, "entries", arr); json_object_set_new(root, "links", json_array()); char *dump=json_dumps(root, JSON_COMPACT); json_decref(root);
      if (dump){ persist_manifest_ns(ctx, dump); free(dump);} rc = 0;
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
      if (dump){ persist_manifest_ns(ctx, dump); free(dump);} rc=0;
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
      if (dump){ persist_manifest_ns(ctx, dump); free(dump);} rc=0;
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
  if (!valid_path(path)) return -EINVAL;
  if (strcmp(path, "/")==0){
    st->st_mode = S_IFDIR | 0755; st->st_nlink = 2; st->st_uid = getuid(); st->st_gid = getgid(); return 0;
  }
  if (strcmp(path, readme_path)==0){ st->st_mode = S_IFREG | 0444; st->st_nlink = 1; st->st_size = (off_t)strlen(readme_body); return 0; }
  /* Look for a file entry in manifest */
  nostrfs_ctx *ctx = get_ctx();
  if (ctx && ctx->manifest_loaded){
    /* First check for an exact entry match */
    for (size_t i=0;i<ctx->manifest.entries_len;i++){
      nh_entry *e = &ctx->manifest.entries[i];
      if (e->path && strcmp(e->path, path)==0){
        if ((e->mode & S_IFDIR) == S_IFDIR){
          st->st_mode = S_IFDIR | (e->mode & 0777);
          st->st_nlink = 2;
          st->st_uid = (uid_t)e->uid; st->st_gid = (gid_t)e->gid; st->st_mtime = (time_t)e->mtime; st->st_size = 0;
          return 0;
        }
        st->st_mode = S_IFREG | (e->mode ? (e->mode & 0777) : 0444);
        st->st_nlink = 1; st->st_size = (off_t)e->size; st->st_uid = (uid_t)e->uid; st->st_gid = (gid_t)e->gid; st->st_mtime = (time_t)e->mtime; return 0;
      }
    }
    /* If any entry has a prefix path matching a directory, treat as dir */
    size_t plen = strlen(path);
    if (plen>1){
      for (size_t i=0;i<ctx->manifest.entries_len;i++){
        nh_entry *e = &ctx->manifest.entries[i];
        if (!e->path) continue;
        if (strncmp(e->path, path, plen)==0 && e->path[plen]=='/'){
          st->st_mode = S_IFDIR | 0755; st->st_nlink = 2; st->st_uid = getuid(); st->st_gid = getgid(); st->st_size = 0; return 0;
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
  if (!valid_path(path)) return -EINVAL;
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

static int nfs_fsync(const char *path, int datasync, struct fuse_file_info *fi){
  (void)datasync; nfs_write_handle *h = (nfs_write_handle*)(uintptr_t)fi->fh; if (!h) return 0;
  if (h->fd >= 0){ if (datasync) fdatasync(h->fd); else fsync(h->fd); }
  if (h->dirty) return nfs_flush(path, fi);
  return 0;
}

static int nfs_statfs(const char *path, struct statvfs *stbuf){
  (void)path; memset(stbuf, 0, sizeof(*stbuf));
  nostrfs_ctx *ctx = get_ctx();
  const char *dir = "/";
  char buf[512];
  if (ctx){
    uid_t uid = getuid();
    const char *base = ctx->opts.cache_dir ? ctx->opts.cache_dir : "/var/cache/nostrfs";
    snprintf(buf, sizeof buf, "%s/%u", base, (unsigned)uid);
    dir = buf;
  }
  struct statvfs sv;
  if (statvfs(dir, &sv) == 0){ *stbuf = sv; return 0; }
  return -errno;
}

static void *nfs_init(struct fuse_conn_info *conn, struct fuse_config *cfg){
  (void)conn; nostrfs_ctx *ctx = get_ctx();
  if (cfg){
    cfg->kernel_cache = 1;
    cfg->entry_timeout = 0.5;
    cfg->attr_timeout = 0.5;
    cfg->negative_timeout = 0.0;
  }
  if (ctx){
    const char *base = ctx->opts.cache_dir ? ctx->opts.cache_dir : "/var/cache/nostrfs";
    (void)mkdir(base, 0700);
    char udir[512]; snprintf(udir, sizeof udir, "%s/%u", base, (unsigned)getuid()); (void)mkdir(udir, 0700);
    char tdir[512]; snprintf(tdir, sizeof tdir, "%s/tmp", udir); (void)mkdir(tdir, 0700);
  }
  return ctx;
}

static int nfs_rename(const char *from, const char *to, unsigned int flags){
  (void)flags; nostrfs_ctx *ctx = get_ctx(); if (!ctx || !ctx->opts.writeback) return -EACCES;
  if (!valid_path(from) || !valid_path(to)) return -EINVAL;
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
  if (!valid_path(path)) return -EINVAL;
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
  if (!valid_path(path)) return -EINVAL;
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
  if (!valid_path(path)) return -EINVAL;
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
  if (!valid_path(path)) return -EINVAL;
  filler(buf, ".", NULL, 0, 0);
  filler(buf, "..", NULL, 0, 0);
  if (strcmp(path, "/")==0) filler(buf, readme_path+1, NULL, 0, 0);
  /* List immediate children under 'path' from manifest */
  nostrfs_ctx *ctx = get_ctx();
  if (ctx && ctx->manifest_loaded){
    /* seen list to avoid duplicates */
    const size_t max_seen = 512; /* cap for simplicity */
    char seen[max_seen][256]; size_t seen_cnt = 0;
    size_t plen = strlen(path);
    /* Ensure base ends with slash unless root */
    char base[512];
    if (strcmp(path, "/")==0){ strcpy(base, "/"); }
    else { if (plen >= sizeof base - 2) return 0; strcpy(base, path); if (base[plen-1] != '/') { base[plen] = '/'; base[plen+1] = '\0'; plen++; } }
    for (size_t i=0;i<ctx->manifest.entries_len;i++){
      nh_entry *e = &ctx->manifest.entries[i]; if (!e->path) continue;
      const char *p = NULL;
      if (strcmp(base, "/")==0){ if (e->path[0]!='/') continue; p = e->path + 1; }
      else {
        if (strncmp(e->path, base, plen) != 0) continue; p = e->path + plen;
      }
      const char *slash = strchr(p, '/');
      size_t name_len = slash ? (size_t)(slash - p) : strlen(p);
      if (name_len == 0) continue;
      if (name_len >= 256) continue;
      char name[256]; memcpy(name, p, name_len); name[name_len] = '\0';
      int already = 0; for (size_t j=0;j<seen_cnt;j++){ if (strcmp(seen[j], name)==0){ already = 1; break; } }
      if (already) continue;
      if (seen_cnt < max_seen){ strcpy(seen[seen_cnt++], name); }
      filler(buf, name, NULL, 0, 0);
    }
  }
  return 0;
}

static int nfs_open(const char *path, struct fuse_file_info *fi){
  if (!valid_path(path)) return -EINVAL;
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

static int nfs_mkdir(const char *path, mode_t mode){
  (void)mode; nostrfs_ctx *ctx = get_ctx(); if (!ctx || !ctx->opts.writeback) return -EACCES; if (!valid_path(path)) return -EINVAL; if (!ctx->req_chan) return -EIO;
  manager_req *rq = (manager_req*)calloc(1, sizeof(*rq)); if (!rq) return -ENOMEM;
  rq->kind = NFS_REQ_MKDIR; rq->path = path; rq->uid = getuid();
  GoChannel *reply = go_channel_create(1); rq->reply = reply; go_channel_send(ctx->req_chan, rq);
  int rc = (int)(intptr_t)({ void *v=NULL; go_channel_receive(reply, &v)==0 ? v : (void*)(intptr_t)-EIO; });
  go_channel_close(reply); go_channel_free(reply); return rc;
}

static int nfs_rmdir(const char *path){
  nostrfs_ctx *ctx = get_ctx(); if (!ctx || !ctx->opts.writeback) return -EACCES; if (!valid_path(path)) return -EINVAL; if (!ctx->req_chan) return -EIO;
  manager_req *rq = (manager_req*)calloc(1, sizeof(*rq)); if (!rq) return -ENOMEM;
  rq->kind = NFS_REQ_RMDIR; rq->path = path; rq->uid = getuid();
  GoChannel *reply = go_channel_create(1); rq->reply = reply; go_channel_send(ctx->req_chan, rq);
  int rc = (int)(intptr_t)({ void *v=NULL; go_channel_receive(reply, &v)==0 ? v : (void*)(intptr_t)-EIO; });
  go_channel_close(reply); go_channel_free(reply); return rc;
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
            /* Dispatch download to worker pool and wait */
            download_req *dr = (download_req*)calloc(1, sizeof(*dr)); if (!dr) return -EIO;
            dr->base_url = strdup(base); dr->cid = strdup(e->cid); dr->dest_path = strdup(caspath);
            GoChannel *dreply = go_channel_create(1); dr->reply = dreply; go_channel_send(ctx->download_q, dr);
            void *vres = NULL; if (go_channel_receive(dreply, &vres) != 0){ go_channel_close(dreply); go_channel_free(dreply); return -EIO; }
            go_channel_close(dreply); go_channel_free(dreply);
            int frc = (int)(intptr_t)vres;
            if (frc == 0){
              /* Verify CAS integrity matches CID */
              char hex[65];
              if (sha256_file_hex_local(caspath, hex) != 0 || strcmp(hex, e->cid) != 0){
                unlink(caspath);
              } else {
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
  .init    = nfs_init,
  .getattr = nfs_getattr,
  .readdir = nfs_readdir,
  .open    = nfs_open,
  .read    = nfs_read,
  .create  = nfs_create,
  .mkdir   = nfs_mkdir,
  .rmdir   = nfs_rmdir,
  .write   = nfs_write,
  .flush   = nfs_flush,
  .fsync   = nfs_fsync,
  .statfs  = nfs_statfs,
  .release = nfs_release,
  .rename  = nfs_rename,
  .unlink  = nfs_unlink,
  .chmod   = nfs_chmod,
  .chown   = nfs_chown,
};

/* --- FUSE lifecycle wiring --- */
static void nfs_destroy(void *private_data){
nostrfs_ctx *ctx = (nostrfs_ctx*)private_data; if (!ctx) return;
if (ctx->req_chan){ go_channel_close(ctx->req_chan); go_channel_free(ctx->req_chan); ctx->req_chan=NULL; }
 if (ctx->upload_q){ go_channel_close(ctx->upload_q); go_channel_free(ctx->upload_q); ctx->upload_q=NULL; }
 /* Try to flush a last publish if we have a recent manifest dump */
 if (ctx->last_manifest_json){ publish_best_effort(ctx, ctx->last_manifest_json); free(ctx->last_manifest_json); ctx->last_manifest_json=NULL; }
 if (ctx->download_q){ go_channel_close(ctx->download_q); go_channel_free(ctx->download_q); ctx->download_q=NULL; }
}

int nostrfs_run(const nostrfs_options *opts, int argc, char **argv){
  if (!opts || !opts->mountpoint) return -1;
  /* Build context */
  nostrfs_ctx *ctx = (nostrfs_ctx*)calloc(1, sizeof(*ctx)); if (!ctx) return -1;
  ctx->opts = *opts; ctx->manifest_loaded = 0; ctx->req_chan = go_channel_create(64); ctx->upload_q = go_channel_create(64); ctx->download_q = go_channel_create(64);
  /* Load manifest JSON from cache settings */
  nh_cache c; if (nh_cache_open_configured(&c, "/etc/nss_nostr.conf")==0){
    size_t cap = 8192; char *bufjson = (char*)malloc(cap);
    if (bufjson){
      char keybuf[128]; snprintf(keybuf, sizeof keybuf, "manifest.%s", ctx->opts.namespace_name ? ctx->opts.namespace_name : "personal");
      if (nh_cache_get_setting(&c, keybuf, bufjson, cap) == 0){
        if (nh_manifest_parse_json(bufjson, &ctx->manifest) == 0) ctx->manifest_loaded = 1;
      }
      free(bufjson);
    }
    nh_cache_close(&c);
  }
  /* Start actor fiber */
  go(manifest_manager, ctx);
  /* Start upload workers */
  extern void *upload_worker(void *arg); /* forward */
  for (int i=0;i<4;i++) go(upload_worker, ctx);
  /* Start download workers */
  extern void *download_worker(void *arg);
  for (int i=0;i<4;i++) go(download_worker, ctx);
  /* Start background publisher */
  extern void *publish_worker(void *arg);
  go(publish_worker, ctx);
  /* Register destroy for cleanup */
  struct fuse_operations ops = nfs_ops; ops.destroy = nfs_destroy;
  /* Hand off to fuse_main (argv contains only FUSE args here) */
  int rc = fuse_main(argc, argv, &ops, ctx);
  return rc;
}

/* Basic CLI that parses --writeback, --cache=, --namespace= and forwards remaining args to FUSE */
int main(int argc, char **argv){
  if (argc < 2){
    fprintf(stderr, "Usage: %s <mountpoint> [--writeback] [--cache=DIR] [--namespace=NAME] [FUSE options...]\n", argv[0]);
    return 2;
  }
  nostrfs_options opts; memset(&opts, 0, sizeof(opts));
  opts.mountpoint = NULL;
  opts.cache_dir = getenv("NOSTRFS_CACHE"); if (!opts.cache_dir) opts.cache_dir = "/var/cache/nostrfs";
  opts.namespace_name = getenv("NOSTRFS_NAMESPACE"); if (!opts.namespace_name) opts.namespace_name = "personal";
  opts.writeback = 0;
  char *fuse_argv[128]; int fuse_argc = 0; fuse_argv[fuse_argc++] = argv[0];
  for (int i=1;i<argc;i++){
    const char *a = argv[i];
    if (!opts.mountpoint && a[0] != '-') { opts.mountpoint = a; fuse_argv[fuse_argc++] = argv[i]; continue; }
    if (strcmp(a, "--writeback")==0){ opts.writeback = 1; continue; }
    if (strncmp(a, "--cache=", 8)==0){ opts.cache_dir = a+8; continue; }
    if (strncmp(a, "--namespace=", 12)==0){ opts.namespace_name = a+12; continue; }
    fuse_argv[fuse_argc++] = argv[i];
  }
  if (!opts.mountpoint){ fprintf(stderr, "Missing mountpoint.\n"); return 2; }
  return nostrfs_run(&opts, fuse_argc, fuse_argv);
}
