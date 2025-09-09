#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

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
