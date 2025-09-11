#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include "nostr_blossom.h"
#include <openssl/evp.h>

static size_t sink(void *ptr, size_t size, size_t nmemb, void *userdata){ (void)ptr; (void)userdata; return size*nmemb; }

static int sha256_file_hex(const char *path, char out_hex[65]){
  int ret = -1;
  FILE *fp = fopen(path, "rb"); if (!fp) return -1;
  unsigned char buf[8192]; unsigned char dig[32];
  EVP_MD_CTX *mdctx = EVP_MD_CTX_new(); if (!mdctx){ fclose(fp); return -1; }
  const EVP_MD *md = EVP_sha256();
  if (EVP_DigestInit_ex(mdctx, md, NULL) != 1) goto done;
  for(;;){ size_t n = fread(buf,1,sizeof buf,fp); if (n>0){ if (EVP_DigestUpdate(mdctx, buf, n) != 1) goto done; } if (n < sizeof buf){ if (ferror(fp)) goto done; break; } }
  unsigned int dl = 0; if (EVP_DigestFinal_ex(mdctx, dig, &dl) != 1 || dl != 32) goto done;
  {
    static const char *hex = "0123456789abcdef";
    for (unsigned int i=0;i<dl;i++){ out_hex[i*2]=hex[(dig[i]>>4)&0xF]; out_hex[i*2+1]=hex[dig[i]&0xF]; }
    out_hex[64]='\0';
  }
  ret = 0;
done:
  EVP_MD_CTX_free(mdctx);
  fclose(fp);
  return ret;
}

typedef struct { FILE *fp; } upload_src_t;
static size_t file_read(void *ptr, size_t size, size_t nmemb, void *userdata){
  upload_src_t *s = (upload_src_t*)userdata; if (!s || !s->fp) return 0; return fread(ptr, size, nmemb, s->fp);
}

int nh_blossom_upload(const char *base_url, const char *src_path, char **out_cid){
  if (out_cid) *out_cid = NULL;
  if (!base_url || !src_path || !out_cid) return -1;
  char cid[65]; if (sha256_file_hex(src_path, cid) != 0) return -1;
  CURL *c = curl_easy_init(); if (!c) return -1;
  char url[1024]; snprintf(url, sizeof url, "%s/%s", base_url, cid);
  upload_src_t s = {0}; s.fp = fopen(src_path, "rb"); if (!s.fp){ curl_easy_cleanup(c); return -1; }
  struct stat st; if (stat(src_path, &st) != 0){ fclose(s.fp); curl_easy_cleanup(c); return -1; }
  curl_easy_setopt(c, CURLOPT_URL, url);
  curl_easy_setopt(c, CURLOPT_READFUNCTION, file_read);
  curl_easy_setopt(c, CURLOPT_READDATA, &s);
  curl_easy_setopt(c, CURLOPT_UPLOAD, 1L);
  curl_easy_setopt(c, CURLOPT_INFILESIZE_LARGE, (curl_off_t)st.st_size);
  CURLcode rc = curl_easy_perform(c);
  long code=0; if (rc==CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
  fclose(s.fp);
  curl_easy_cleanup(c);
  if (rc==CURLE_OK && (code==200 || code==201 || code==204)){
    *out_cid = strdup(cid);
    return *out_cid ? 0 : -1;
  }
  return -1;
}

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
