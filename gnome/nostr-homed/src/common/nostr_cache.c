#include "nostr_cache.h"
#include <openssl/sha.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static int exec_schema(sqlite3 *db){
  const char *schema =
    "PRAGMA journal_mode=WAL;"
    "CREATE TABLE IF NOT EXISTS users("
    " uid INTEGER PRIMARY KEY, npub TEXT UNIQUE, username TEXT UNIQUE, gid INTEGER, home TEXT, updated_at INTEGER);"
    "CREATE TABLE IF NOT EXISTS groups("
    " gid INTEGER PRIMARY KEY, name TEXT UNIQUE);"
    "CREATE TABLE IF NOT EXISTS blobs("
    " cid TEXT PRIMARY KEY, size INTEGER, mtime INTEGER, path TEXT, present INTEGER);"
    "CREATE TABLE IF NOT EXISTS settings(key TEXT PRIMARY KEY, value TEXT);";
  char *errmsg=NULL; int rc = sqlite3_exec(db, schema, NULL, NULL, &errmsg);
  if (rc != SQLITE_OK) { if (errmsg) sqlite3_free(errmsg); return -1; }
  return 0;
}

int nh_cache_open(nh_cache *c, const char *path){
  if (!c || !path) return -1;
  memset(c, 0, sizeof(*c));
  if (sqlite3_open(path, &c->db) != SQLITE_OK) return -1;
  if (exec_schema(c->db) != 0) { sqlite3_close(c->db); c->db=NULL; return -1; }
  c->uid_base = 100000; c->uid_range = 100000; /* defaults */
  return 0;
}

void nh_cache_close(nh_cache *c){ if (!c) return; if (c->db) sqlite3_close(c->db); c->db=NULL; }

int nh_cache_set_uid_policy(nh_cache *c, uint32_t base, uint32_t range){ if (!c||!c->db||range==0) return -1; c->uid_base=base; c->uid_range=range; return 0; }

uint32_t nh_cache_map_npub_to_uid(const nh_cache *c, const char *npub_hex){
  if (!c || !npub_hex || !*npub_hex || c->uid_range==0) return 0;
  unsigned char dig[32]; SHA256((const unsigned char*)npub_hex, strlen(npub_hex), dig);
  uint32_t v = ((uint32_t)dig[0]<<24)|((uint32_t)dig[1]<<16)|((uint32_t)dig[2]<<8)|((uint32_t)dig[3]);
  return c->uid_base + (v % c->uid_range);
}

static int parse_kv_line(const char *line, char *key, size_t ksz, char *val, size_t vsz){
  const char *eq = strchr(line, '='); if (!eq) return -1;
  size_t kl = (size_t)(eq - line);
  if (kl >= ksz) {
    return -1;
  }
  memcpy(key, line, kl);
  key[kl] = 0;
  const char *v = eq+1; size_t vl = strlen(v);
  while (vl && (v[vl-1]=='\n' || v[vl-1]=='\r')) vl--;
  if (vl >= vsz) {
    return -1;
  }
  memcpy(val, v, vl);
  val[vl] = 0;
  return 0;
}

int nh_cache_open_configured(nh_cache *c, const char *conf_path){
  const char *default_db = "/var/lib/nostr-homed/cache.db";
  uint32_t base = 100000, range = 100000;
  char db_path[512]; strncpy(db_path, default_db, sizeof db_path); db_path[sizeof db_path - 1] = 0;
  FILE *f = conf_path ? fopen(conf_path, "r") : NULL;
  if (f){
    char line[512]; char key[128], val[384];
    while (fgets(line, sizeof line, f)){
      if (line[0]=='#' || line[0]=='\n') continue;
      if (parse_kv_line(line, key, sizeof key, val, sizeof val)==0){
        if (strcmp(key, "db_path")==0) { strncpy(db_path, val, sizeof db_path); db_path[sizeof db_path-1]=0; }
        else if (strcmp(key, "uid_base")==0) { base = (uint32_t)strtoul(val, NULL, 10); }
        else if (strcmp(key, "uid_range")==0) { range = (uint32_t)strtoul(val, NULL, 10); }
      }
    }
    fclose(f);
  }
  if (nh_cache_open(c, db_path) != 0) return -1;
  if (nh_cache_set_uid_policy(c, base, range) != 0) { nh_cache_close(c); return -1; }
  return 0;
}

int nh_cache_lookup_name(nh_cache *c, const char *name, unsigned int *uid, unsigned int *gid, char *home_out, size_t home_len){
  if (!c || !c->db || !name) return -1;
  const char *sql = "SELECT uid,gid,home FROM users WHERE username=?";
  sqlite3_stmt *st = NULL; if (sqlite3_prepare_v2(c->db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
  sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
  int rc = sqlite3_step(st);
  int ret = -1;
  if (rc == SQLITE_ROW){
    if (uid) *uid = (unsigned int)sqlite3_column_int(st, 0);
    if (gid) *gid = (unsigned int)sqlite3_column_int(st, 1);
    const unsigned char *h = sqlite3_column_text(st, 2);
    if (home_out && home_len){ strncpy(home_out, h ? (const char*)h : "", home_len); if (home_len) home_out[home_len-1]=0; }
    ret = 0;
  }
  sqlite3_finalize(st);
  return ret;
}

int nh_cache_lookup_uid(nh_cache *c, unsigned int uid, char *name_out, size_t name_len, unsigned int *gid, char *home_out, size_t home_len){
  if (!c || !c->db) return -1;
  const char *sql = "SELECT username,gid,home FROM users WHERE uid=?";
  sqlite3_stmt *st = NULL; if (sqlite3_prepare_v2(c->db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
  sqlite3_bind_int(st, 1, (int)uid);
  int rc = sqlite3_step(st); int ret = -1;
  if (rc == SQLITE_ROW){
    const unsigned char *n = sqlite3_column_text(st, 0);
    if (name_out && name_len){ strncpy(name_out, n ? (const char*)n : "", name_len); if (name_len) name_out[name_len-1]=0; }
    if (gid) *gid = (unsigned int)sqlite3_column_int(st, 1);
    const unsigned char *h = sqlite3_column_text(st, 2);
    if (home_out && home_len){ strncpy(home_out, h ? (const char*)h : "", home_len); if (home_len) home_out[home_len-1]=0; }
    ret = 0;
  }
  sqlite3_finalize(st);
  return ret;
}

int nh_cache_upsert_user(nh_cache *c, unsigned int uid, const char *npub, const char *username, unsigned int gid, const char *home){
  if (!c || !c->db || !username) return -1;
  const char *sql = "INSERT INTO users(uid,npub,username,gid,home,updated_at) VALUES(?,?,?,?,?,strftime('%s','now'))"
                    " ON CONFLICT(uid) DO UPDATE SET npub=excluded.npub, username=excluded.username, gid=excluded.gid, home=excluded.home, updated_at=strftime('%s','now')";
  sqlite3_stmt *st = NULL; if (sqlite3_prepare_v2(c->db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
  sqlite3_bind_int(st, 1, (int)uid);
  sqlite3_bind_text(st, 2, npub ? npub : "", -1, SQLITE_STATIC);
  sqlite3_bind_text(st, 3, username, -1, SQLITE_STATIC);
  sqlite3_bind_int(st, 4, (int)gid);
  sqlite3_bind_text(st, 5, home ? home : "", -1, SQLITE_STATIC);
  int rc = sqlite3_step(st);
  sqlite3_finalize(st);
  return (rc == SQLITE_DONE) ? 0 : -1;
}

int nh_cache_group_lookup_name(nh_cache *c, const char *name, unsigned int *gid){
  if (!c || !c->db || !name || !gid) return -1;
  const char *sql = "SELECT gid FROM groups WHERE name=?";
  sqlite3_stmt *st = NULL; if (sqlite3_prepare_v2(c->db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
  sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
  int rc = sqlite3_step(st);
  int ret = -1;
  if (rc == SQLITE_ROW){ *gid = (unsigned int)sqlite3_column_int(st, 0); ret = 0; }
  sqlite3_finalize(st);
  return ret;
}

int nh_cache_group_lookup_gid(nh_cache *c, unsigned int gid, char *name_out, size_t name_len){
  if (!c || !c->db || !name_out || name_len==0) return -1;
  name_out[0] = '\0';
  const char *sql = "SELECT name FROM groups WHERE gid=?";
  sqlite3_stmt *st = NULL; if (sqlite3_prepare_v2(c->db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
  sqlite3_bind_int(st, 1, (int)gid);
  int rc = sqlite3_step(st);
  int ret = -1;
  if (rc == SQLITE_ROW){ const unsigned char *n = sqlite3_column_text(st, 0); if (n){ strncpy(name_out, (const char*)n, name_len); name_out[name_len-1]=0; ret=0; } }
  sqlite3_finalize(st);
  return ret;
}

int nh_cache_ensure_primary_group(nh_cache *c, const char *username, unsigned int gid){
  if (!c || !c->db || !username) return -1;
  /* group name convention: same as username */
  const char *sql = "INSERT INTO groups(gid,name) VALUES(?,?) ON CONFLICT(gid) DO UPDATE SET name=excluded.name";
  sqlite3_stmt *st = NULL; if (sqlite3_prepare_v2(c->db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
  sqlite3_bind_int(st, 1, (int)gid);
  sqlite3_bind_text(st, 2, username, -1, SQLITE_STATIC);
  int rc = sqlite3_step(st);
  sqlite3_finalize(st);
  return (rc == SQLITE_DONE) ? 0 : -1;
}

int nh_cache_set_setting(nh_cache *c, const char *key, const char *value){
  if (!c || !c->db || !key) return -1;
  const char *sql = "INSERT INTO settings(key,value) VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value";
  sqlite3_stmt *st=NULL; if (sqlite3_prepare_v2(c->db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
  sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
  sqlite3_bind_text(st, 2, value ? value : "", -1, SQLITE_STATIC);
  int rc = sqlite3_step(st);
  sqlite3_finalize(st);
  return (rc == SQLITE_DONE) ? 0 : -1;
}

int nh_cache_get_setting(nh_cache *c, const char *key, char *out, size_t outlen){
  if (!c || !c->db || !key || !out || outlen==0) return -1;
  out[0] = '\0';
  const char *sql = "SELECT value FROM settings WHERE key=?";
  sqlite3_stmt *st=NULL; if (sqlite3_prepare_v2(c->db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
  sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
  int rc = sqlite3_step(st);
  int ret = -1;
  if (rc == SQLITE_ROW){
    const unsigned char *v = sqlite3_column_text(st, 0);
    if (v){ strncpy(out, (const char*)v, outlen); out[outlen-1] = '\0'; ret = 0; }
  }
  sqlite3_finalize(st);
  return ret;
}
