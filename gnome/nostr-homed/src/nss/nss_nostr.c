#define _GNU_SOURCE
#include "nss_nostr.h"
#include "nostr_cache.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

static nh_cache g_cache;
static int g_inited = 0;

static void ensure_init(void){
  if (g_inited) return;
  if (nh_cache_open_configured(&g_cache, "/etc/nss_nostr.conf") == 0) g_inited = 1;
}

static enum nss_status fill_pwd(struct passwd *pwd, char *buffer, size_t buflen,
                                const char *name, uid_t uid, gid_t gid, const char *home){
  (void)buffer; (void)buflen;
  if (!pwd) return NSS_STATUS_TRYAGAIN;
  memset(pwd, 0, sizeof(*pwd));
  pwd->pw_name = (char*)(name ? name : "nostr");
  pwd->pw_passwd = (char*)"x";
  pwd->pw_uid = uid;
  pwd->pw_gid = gid ? gid : uid;
  pwd->pw_gecos = (char*)"Nostr User";
  pwd->pw_dir = (char*)(home ? home : "/home/nostr");
  pwd->pw_shell = (char*)"/bin/bash";
  return NSS_STATUS_SUCCESS;
}

enum nss_status _nss_nostr_getpwnam_r(const char *name, struct passwd *pwd,
                                      char *buffer, size_t buflen, int *errnop){
  ensure_init();
  if (!g_inited) { if (errnop) *errnop = EAGAIN; return NSS_STATUS_UNAVAIL; }
  unsigned int uid=0,gid=0; char home[256];
  if (nh_cache_lookup_name(&g_cache, name, &uid, &gid, home, sizeof home) == 0) {
    return fill_pwd(pwd, buffer, buflen, name, (uid_t)uid, (gid_t)gid, home);
  }
  /* If not found, deterministically map to a UID (policy), but do not fabricate a record. */
  return NSS_STATUS_NOTFOUND;
}

enum nss_status _nss_nostr_getpwuid_r(uid_t uid, struct passwd *pwd,
                                      char *buffer, size_t buflen, int *errnop){
  ensure_init();
  if (!g_inited) { if (errnop) *errnop = EAGAIN; return NSS_STATUS_UNAVAIL; }
  char name[128]; unsigned int gid=0; char home[256];
  if (nh_cache_lookup_uid(&g_cache, (unsigned int)uid, name, sizeof name, &gid, home, sizeof home) == 0) {
    return fill_pwd(pwd, buffer, buflen, name, (uid_t)uid, (gid_t)gid, home);
  }
  return NSS_STATUS_NOTFOUND;
}

enum nss_status _nss_nostr_getgrnam_r(const char *name, struct group *grp,
                                      char *buffer, size_t buflen, int *errnop){
  (void)buffer; (void)buflen;
  if (!grp || !name) return NSS_STATUS_TRYAGAIN;
  ensure_init(); if (!g_inited){ if (errnop) *errnop = EAGAIN; return NSS_STATUS_UNAVAIL; }
  unsigned int gid=0; if (nh_cache_group_lookup_name(&g_cache, name, &gid) != 0) return NSS_STATUS_NOTFOUND;
  memset(grp, 0, sizeof(*grp));
  grp->gr_name = (char*)name;
  grp->gr_passwd = (char*)"x";
  grp->gr_gid = (gid_t)gid;
  /* Minimal membership list: empty (PAM/NSS can fall back to passwd primary gid) */
  static char *empty_mem[1] = { NULL };
  grp->gr_mem = empty_mem;
  return NSS_STATUS_SUCCESS;
}

enum nss_status _nss_nostr_getgrgid_r(gid_t gid, struct group *grp,
                                      char *buffer, size_t buflen, int *errnop){
  (void)buffer; (void)buflen;
  if (!grp) return NSS_STATUS_TRYAGAIN;
  ensure_init(); if (!g_inited){ if (errnop) *errnop = EAGAIN; return NSS_STATUS_UNAVAIL; }
  char name[128]=""; if (nh_cache_group_lookup_gid(&g_cache, (unsigned int)gid, name, sizeof name) != 0) return NSS_STATUS_NOTFOUND;
  memset(grp, 0, sizeof(*grp));
  grp->gr_name = (char*)strdup(name);
  grp->gr_passwd = (char*)"x";
  grp->gr_gid = gid;
  static char *empty_mem[1] = { NULL };
  grp->gr_mem = empty_mem;
  return NSS_STATUS_SUCCESS;
}

/* Provide minimal initgroups: ensure primary group is present */
enum nss_status _nss_nostr_initgroups_dyn(const char *user, gid_t group,
    long int *start, long int *size, gid_t **groupsp, long int limit, int *errnop){
  (void)user; (void)limit; (void)errnop;
  if (!start || !size || !groupsp) return NSS_STATUS_TRYAGAIN;
  long n = *start;
  if (n >= *size){
    long newsize = (*size) ? (*size) * 2 : 8;
    gid_t *ng = realloc(*groupsp, (size_t)newsize * sizeof(gid_t));
    if (!ng) return NSS_STATUS_TRYAGAIN;
    *groupsp = ng; *size = newsize;
  }
  (*groupsp)[n++] = group; *start = n;
  return NSS_STATUS_SUCCESS;
}
