#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include "nostr_blossom.h"

static size_t sink(void *ptr, size_t size, size_t nmemb, void *userdata){ (void)ptr; (void)userdata; return size*nmemb; }

int nh_blossom_head(const char *base_url, const char *cid){
  if (!base_url || !cid) return -1;
  CURL *c = curl_easy_init(); if (!c) return -1;
  char url[1024]; snprintf(url, sizeof url, "%s/%s", base_url, cid);
  curl_easy_setopt(c, CURLOPT_URL, url);
  curl_easy_setopt(c, CURLOPT_NOBODY, 1L);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, sink);
  CURLcode rc = curl_easy_perform(c);
  long code=0; if (rc==CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
  curl_easy_cleanup(c);
  return (rc==CURLE_OK && code==200) ? 0 : -1;
}

typedef struct {
  FILE *fp;
} file_sink_t;

static size_t file_write(void *ptr, size_t size, size_t nmemb, void *userdata){
  file_sink_t *s = (file_sink_t*)userdata;
  if (!s || !s->fp) return 0;
  return fwrite(ptr, size, nmemb, s->fp);
}

static int ensure_parent_dirs(const char *path){
  char tmp[1024];
  size_t len = strnlen(path, sizeof tmp);
  if (len >= sizeof tmp) return -1;
  memcpy(tmp, path, len+1);
  char *p = strrchr(tmp, '/'); if (!p) return 0; *p = 0;
  if (tmp[0] == '\0') return 0;
  /* mkdir -p */
  for (char *q = tmp+1; *q; q++){
    if (*q == '/') { *q = 0; (void)mkdir(tmp, 0700); *q = '/'; }
  }
  if (mkdir(tmp, 0700) != 0 && errno != EEXIST) return -1;
  return 0;
}

int nh_blossom_fetch(const char *base_url, const char *cid, const char *dest_path){
  if (!base_url || !cid || !dest_path) return -1;
  if (ensure_parent_dirs(dest_path) != 0) return -1;
  char url[1024]; snprintf(url, sizeof url, "%s/%s", base_url, cid);
  char tmppath[1060]; snprintf(tmppath, sizeof tmppath, "%s.tmp", dest_path);
  CURL *c = curl_easy_init(); if (!c) return -1;
  FILE *fp = fopen(tmppath, "wb"); if (!fp) { curl_easy_cleanup(c); return -1; }
  file_sink_t s = { .fp = fp };
  curl_easy_setopt(c, CURLOPT_URL, url);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, file_write);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &s);
  CURLcode rc = curl_easy_perform(c);
  int ret = -1;
  if (rc == CURLE_OK){
    fflush(fp);
    fsync(fileno(fp));
    fclose(fp); fp=NULL;
    if (rename(tmppath, dest_path) == 0) ret = 0;
  }
  if (fp) fclose(fp);
  curl_easy_cleanup(c);
  if (ret != 0) unlink(tmppath);
  return ret;
}
