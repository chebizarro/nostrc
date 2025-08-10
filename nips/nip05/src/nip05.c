#include "nip05.h"
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include "json.h"
#include "keys.h"
#include <ctype.h>

// POSIX ERE: no non-capturing groups; escape literal dots; explicit ranges
static const char *NIP05_REGEX = "^(([A-Za-z0-9._+-]+)@)?([A-Za-z0-9_-]+([.][A-Za-z0-9_-]+)+)$";

typedef struct {
    char *data;
    size_t size;
} MemoryStruct;

static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t total_size = size * nmemb;
    MemoryStruct *mem = (MemoryStruct *)userp;

    char *ptr = realloc(mem->data, mem->size + total_size + 1);
    if (ptr == NULL) {
        return 0;  // Out of memory
    }

    mem->data = ptr;
    memcpy(&(mem->data[mem->size]), contents, total_size);
    mem->size += total_size;
    mem->data[mem->size] = 0;

    return total_size;
}

static void to_lower_inplace(char *s) {
    if (!s) return;
    for (; *s; ++s) *s = (char)tolower((unsigned char)*s);
}

int nostr_nip05_parse_identifier(const char *identifier, char **out_name, char **out_domain) {
    if (!identifier || !out_name || !out_domain) return -1;
    regex_t regex; regmatch_t pmatch[4];
    if (regcomp(&regex, NIP05_REGEX, REG_EXTENDED) != 0) return -1;
    int ok = regexec(&regex, identifier, 4, pmatch, 0);
    regfree(&regex);
    if (ok != 0) return -1;

    // With regex: ^(([local]@)?)([domain]([.][seg])+)$
    // pmatch[2] = local (or -1 if absent), pmatch[3] = domain
    if (pmatch[2].rm_so == -1) {
        *out_name = strdup("_");
    } else {
        size_t nlen = (size_t)(pmatch[2].rm_eo - pmatch[2].rm_so);
        *out_name = (char *)malloc(nlen + 1);
        if (!*out_name) return -1;
        memcpy(*out_name, identifier + pmatch[2].rm_so, nlen);
        (*out_name)[nlen] = '\0';
    }
    size_t dlen = (size_t)(pmatch[3].rm_eo - pmatch[3].rm_so);
    *out_domain = (char *)malloc(dlen + 1);
    if (!*out_domain) { free(*out_name); return -1; }
    memcpy(*out_domain, identifier + pmatch[3].rm_so, dlen);
    (*out_domain)[dlen] = '\0';

    to_lower_inplace(*out_name);
    to_lower_inplace(*out_domain);
    return 0;
}

static long getenv_ms(const char *k, long dflt) {
    const char *v = getenv(k);
    if (!v || !*v) return dflt;
    char *end = NULL; long x = strtol(v, &end, 10);
    if (end == v) return dflt; return x;
}

int nostr_nip05_fetch_json(const char *domain, char **out_json, char **out_error) {
    if (!domain || !out_json) return -1;
    *out_json = NULL; if (out_error) *out_error = NULL;
    CURL *curl = curl_easy_init();
    if (!curl) { if (out_error) *out_error = strdup("curl init"); return -1; }
    char url[1024];
    snprintf(url, sizeof(url), "https://%s/.well-known/nostr.json", domain);

    MemoryStruct chunk = {0};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "libnostr-nip05/1.0");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    long tmo = getenv_ms("NIP05_TIMEOUT_MS", 5000);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, tmo);
    if (!getenv("NIP05_ALLOW_INSECURE")) {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    } else {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }
    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        if (out_error) *out_error = strdup(curl_easy_strerror(res));
        curl_easy_cleanup(curl);
        free(chunk.data);
        return -1;
    }
    *out_json = chunk.data; // caller frees
    curl_easy_cleanup(curl);
    return 0;
}

static int http_get_json(const char *url, char **out_json, char **out_error) {
    *out_json = NULL; if (out_error) *out_error = NULL;
    CURL *curl = curl_easy_init();
    if (!curl) { if (out_error) *out_error = strdup("curl init"); return -1; }
    MemoryStruct chunk = {0};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "libnostr-nip05/1.0");
    long tmo = getenv_ms("NIP05_TIMEOUT_MS", 5000);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, tmo);
    if (!getenv("NIP05_ALLOW_INSECURE")) {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    } else {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }
    if (getenv("NIP05_DEBUG")) fprintf(stderr, "[nip05] HTTP GET %s\n", url);
    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        if (out_error) *out_error = strdup(curl_easy_strerror(res));
        curl_easy_cleanup(curl);
        free(chunk.data);
        return -1;
    }
    if (getenv("NIP05_DEBUG")) {
        long code = 0; curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        fprintf(stderr, "[nip05] HTTP %ld, %zu bytes\n", code, chunk.size);
    }
    *out_json = chunk.data;
    curl_easy_cleanup(curl);
    return 0;
}

static int extract_pub_and_relays(const char *json_str, const char *name,
                                  char **out_hexpub, char ***out_relays, size_t *out_relays_count) {
    if (out_relays) *out_relays = NULL; if (out_relays_count) *out_relays_count = 0;
    *out_hexpub = NULL;
    // names[name] -> pubkey
    char *hex = NULL;
    if (getenv("NIP05_DEBUG")) fprintf(stderr, "[nip05] extract names['%s']\n", name);
    if (nostr_json_get_string_at(json_str, "names", name, &hex) == 0) {
        if (hex && nostr_key_is_valid_public_hex(hex)) {
            *out_hexpub = hex;
            // relays[pubkey] -> [urls]
            char **urls = NULL; size_t n = 0;
            if (getenv("NIP05_DEBUG")) fprintf(stderr, "[nip05] extract relays['%s']\n", *out_hexpub);
            if (nostr_json_get_string_array_at(json_str, "relays", *out_hexpub, &urls, &n) == 0) {
                if (out_relays) *out_relays = urls; else { for (size_t i=0;i<n;i++) free(urls[i]); free(urls);} 
                if (out_relays_count) *out_relays_count = n;
            }
            if (getenv("NIP05_DEBUG")) fprintf(stderr, "[nip05] found pubkey, relays=%zu\n", out_relays_count?*out_relays_count:0);
            return 0;
        }
        free(hex);
    }
    if (getenv("NIP05_DEBUG")) fprintf(stderr, "[nip05] names['%s'] not found or invalid\n", name);
    return -1;
}

int nostr_nip05_lookup(const char *identifier,
                       char **out_hexpub,
                       char ***out_relays,
                       size_t *out_relays_count,
                       char **out_error) {
    if (!identifier || !out_hexpub) return -1;
    if (out_error) *out_error = NULL; if (out_relays) *out_relays = NULL; if (out_relays_count) *out_relays_count = 0;
    char *name = NULL, *domain = NULL;
    if (nostr_nip05_parse_identifier(identifier, &name, &domain) != 0) { if (out_error) *out_error = strdup("bad id"); return -1; }

    // Try query endpoint first
    char url[1024];
    snprintf(url, sizeof(url), "https://%s/.well-known/nostr.json?name=%s", domain, name);
    char *json1 = NULL; int rc = http_get_json(url, &json1, out_error);
    if (rc == 0) {
        rc = extract_pub_and_relays(json1, name, out_hexpub, out_relays, out_relays_count);
        free(json1);
        if (rc == 0) { if (getenv("NIP05_DEBUG")) fprintf(stderr, "[nip05] success via query endpoint\n"); free(name); free(domain); return 0; }
    }
    // Fallback: fetch full document
    char *json2 = NULL;
    rc = nostr_nip05_fetch_json(domain, &json2, out_error);
    if (rc == 0) {
        rc = extract_pub_and_relays(json2, name, out_hexpub, out_relays, out_relays_count);
        free(json2);
        if (rc == 0) { if (getenv("NIP05_DEBUG")) fprintf(stderr, "[nip05] success via full document\n"); free(name); free(domain); return 0; }
    }
    free(name); free(domain);
    if (out_error && !*out_error) *out_error = strdup("not found");
    return -1;
}

int nostr_nip05_validate(const char *identifier, const char *hexpub, int *out_match, char **out_error) {
    if (out_match) *out_match = 0;
    if (!identifier || !hexpub) return -1;
    char *found = NULL; char **relays = NULL; size_t nrel = 0; char *err = NULL;
    int rc = nostr_nip05_lookup(identifier, &found, &relays, &nrel, &err);
    if (rc != 0) { if (out_error) *out_error = err; return -1; }
    int match = (found && strcasecmp(found, hexpub) == 0) ? 1 : 0;
    if (out_match) *out_match = match;
    if (!match && out_error) *out_error = strdup("mismatch");
    if (found) free(found);
    if (relays) { for (size_t i = 0; i < nrel; i++) free(relays[i]); free(relays); }
    return match ? 0 : 1;
}

int nostr_nip05_resolve_from_json(const char *name,
                                  const char *json,
                                  char **out_hexpub,
                                  char ***out_relays,
                                  size_t *out_relays_count,
                                  char **out_error) {
    if (out_error) *out_error = NULL;
    if (out_relays) *out_relays = NULL; if (out_relays_count) *out_relays_count = 0;
    if (!name || !json || !out_hexpub) {
        if (out_error) *out_error = strdup("bad args");
        return -1;
    }
    int rc = extract_pub_and_relays(json, name, out_hexpub, out_relays, out_relays_count);
    if (rc != 0) {
        if (out_error) *out_error = strdup("not found");
        return -1;
    }
    return 0;
}
