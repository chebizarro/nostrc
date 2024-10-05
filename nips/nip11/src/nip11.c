#include "nostr/nip11.h"
#include "json_interface.h"
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>

static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct MemoryStruct *mem = (struct MemoryStruct *)userp;

    char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if(ptr == NULL) {
        return 0;
    }

    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;

    return realsize;
}

RelayInformationDocument* fetch_relay_info(const char *url) {
    CURL *curl_handle;
    CURLcode res;

    struct MemoryStruct chunk;
    chunk.memory = malloc(1);
    chunk.size = 0;

    curl_global_init(CURL_GLOBAL_ALL);
    curl_handle = curl_easy_init();

    curl_easy_setopt(curl_handle, CURLOPT_URL, url);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)&chunk);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Accept: application/nostr+json");
    curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, headers);

    res = curl_easy_perform(curl_handle);

    RelayInformationDocument *info = NULL;

    if(res == CURLE_OK) {
        info = nostr_json_deserialize(chunk.memory);
    }

    curl_easy_cleanup(curl_handle);
    free(chunk.memory);
    curl_global_cleanup();

    return info;
}

void free_relay_info(RelayInformationDocument *info) {
    if (info->supported_nips) free(info->supported_nips);
    if (info->name) free(info->name);
    if (info->description) free(info->description);
    if (info->pubkey) free(info->pubkey);
    if (info->contact) free(info->contact);
    if (info->software) free(info->software);
    if (info->version) free(info->version);
    if (info->limitation) free(info->limitation);
    if (info->relay_countries) free(info->relay_countries);
    if (info->language_tags) free(info->language_tags);
    if (info->tags) free(info->tags);
    if (info->posting_policy) free(info->posting_policy);
    if (info->payments_url) free(info->payments_url);
    if (info->fees) free(info->fees);
    if (info->icon) free(info->icon);
    free(info);
}
