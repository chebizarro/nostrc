#include "nip11.h"
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include "json.h"

typedef struct {
    char *data;
    size_t size;
} MemoryStruct;

static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t total = size * nmemb;
    MemoryStruct *mem = (MemoryStruct *)userp;
    char *p = realloc(mem->data, mem->size + total + 1);
    if (!p) return 0;
    mem->data = p;
    memcpy(mem->data + mem->size, contents, total);
    mem->size += total;
    mem->data[mem->size] = '\0';
    return total;
}

/* Build a minimal NIP-11 JSON from an in-memory document. Caller must free. */
char *nostr_nip11_build_info_json(const RelayInformationDocument *info) {
    if (!info) return NULL;
    /* Conservative buffer; for production, prefer libjson building. */
    size_t cap = 2048;
    char *out = (char*)malloc(cap);
    if (!out) return NULL;
    const char *name = info->name ? info->name : "";
    const char *software = info->software ? info->software : "nostrc";
    const char *version = info->version ? info->version : "0.1";
    int nips_n = info->supported_nips_count;
    /* Build supported_nips array string */
    char nips[512]; size_t noff = 0; nips[0] = '\0';
    if (info->supported_nips && nips_n > 0) {
        noff += (size_t)snprintf(nips+noff, sizeof(nips)-noff, "[");
        for (int i = 0; i < nips_n; i++) {
            noff += (size_t)snprintf(nips+noff, sizeof(nips)-noff, "%s%d", (i?",":""), info->supported_nips[i]);
            if (noff >= sizeof(nips)) break;
        }
        (void)snprintf(nips+noff, sizeof(nips)-noff, "]");
    } else {
        (void)snprintf(nips, sizeof(nips), "[]");
    }
    /* Optionally include a minimal limitation object if present */
    char lim[512]; lim[0] = '\0';
    if (info->limitation) {
        size_t loff = 0;
        loff += (size_t)snprintf(lim+loff, sizeof(lim)-loff, ",\"limitation\":{");
        int first = 1;
        if (info->limitation->max_filters) {
            loff += (size_t)snprintf(lim+loff, sizeof(lim)-loff, "%s\"max_filters\":%d", first?"":",", info->limitation->max_filters); first = 0;
        }
        if (info->limitation->max_limit) {
            loff += (size_t)snprintf(lim+loff, sizeof(lim)-loff, "%s\"max_limit\":%d", first?"":",", info->limitation->max_limit); first = 0;
        }
        if (!first) (void)snprintf(lim+loff, sizeof(lim)-loff, "}"); else lim[0] = '\0';
    }
    /* Optional extra fields */
    const char *desc = info->description ? info->description : NULL;
    const char *contact = info->contact ? info->contact : NULL;
    if (desc || contact) {
        int n = snprintf(out, cap,
            "{\"name\":\"%s\",\"software\":\"%s\",\"version\":\"%s\",\"supported_nips\":%s%s%s%s%s}",
            name, software, version, nips, lim,
            desc?",\"description\":\"":"", desc?desc:"",
            contact?"\",\"contact\":\"":"");
        if (n < 0 || (size_t)n >= cap) { free(out); return NULL; }
        /* append contact if both set */
        if (contact && desc) {
            size_t cur = (size_t)n;
            size_t left = cap - cur;
            int m = snprintf(out+cur, left, "\"%s\"", contact);
            if (m < 0 || (size_t)m >= left) { free(out); return NULL; }
        }
        return out;
    }
    int n = snprintf(out, cap,
        "{\"name\":\"%s\",\"software\":\"%s\",\"version\":\"%s\",\"supported_nips\":%s%s}",
        name, software, version, nips, lim);
    if (n < 0 || (size_t)n >= cap) { free(out); return NULL; }
    return out;
}

/* Parsing is implemented using libnostr JSON helpers (backend-agnostic). */

static RelayInformationDocument *parse_json_to_doc(const char *json, const char *url_opt) {
    if (!json) return NULL;
    /* NIP-11 requires the document to be a JSON object */
    if (!nostr_json_is_object_str(json)) return NULL;
    RelayInformationDocument *info = (RelayInformationDocument *)calloc(1, sizeof(RelayInformationDocument));
    if (!info) return NULL;
    if (url_opt) info->url = strdup(url_opt);
    // Simple string fields
    (void)nostr_json_get_string(json, "name", &info->name);
    (void)nostr_json_get_string(json, "description", &info->description);
    (void)nostr_json_get_string(json, "pubkey", &info->pubkey);
    (void)nostr_json_get_string(json, "contact", &info->contact);
    (void)nostr_json_get_string(json, "software", &info->software);
    (void)nostr_json_get_string(json, "version", &info->version);
    // supported_nips
    size_t nips_n = 0;
    (void)nostr_json_get_int_array(json, "supported_nips", &info->supported_nips, &nips_n);
    info->supported_nips_count = (int)nips_n;
    // limitation (optional subset)
    RelayLimitationDocument *L = (RelayLimitationDocument *)calloc(1, sizeof(RelayLimitationDocument));
    if (L) {
        int iv; bool bv;
        if (nostr_json_get_int_at(json, "limitation", "max_message_length", &iv) == 0) L->max_message_length = iv;
        if (nostr_json_get_int_at(json, "limitation", "max_subscriptions", &iv) == 0) L->max_subscriptions = iv;
        if (nostr_json_get_int_at(json, "limitation", "max_filters", &iv) == 0) L->max_filters = iv;
        if (nostr_json_get_int_at(json, "limitation", "max_limit", &iv) == 0) L->max_limit = iv;
        if (nostr_json_get_int_at(json, "limitation", "max_subid_length", &iv) == 0) L->max_subid_length = iv;
        if (nostr_json_get_int_at(json, "limitation", "max_event_tags", &iv) == 0) L->max_event_tags = iv;
        if (nostr_json_get_int_at(json, "limitation", "max_content_length", &iv) == 0) L->max_content_length = iv;
        if (nostr_json_get_int_at(json, "limitation", "min_pow_difficulty", &iv) == 0) L->min_pow_difficulty = iv;
        if (nostr_json_get_bool_at(json, "limitation", "auth_required", &bv) == 0) L->auth_required = bv;
        if (nostr_json_get_bool_at(json, "limitation", "payment_required", &bv) == 0) L->payment_required = bv;
        if (nostr_json_get_bool_at(json, "limitation", "restricted_writes", &bv) == 0) L->restricted_writes = bv;
        // Only assign if something was actually set; otherwise free it to keep NULL when absent
        if (L->max_message_length || L->max_subscriptions || L->max_filters || L->max_limit ||
            L->max_subid_length || L->max_event_tags || L->max_content_length || L->min_pow_difficulty ||
            L->auth_required || L->payment_required || L->restricted_writes) {
            info->limitation = L;
        } else {
            free(L);
        }
    }

    // Optional arrays of strings
    size_t arr_n = 0;
    (void)nostr_json_get_string_array(json, "relay_countries", &info->relay_countries, &arr_n);
    info->relay_countries_count = (int)arr_n;
    arr_n = 0;
    (void)nostr_json_get_string_array(json, "language_tags", &info->language_tags, &arr_n);
    info->language_tags_count = (int)arr_n;
    arr_n = 0;
    (void)nostr_json_get_string_array(json, "tags", &info->tags, &arr_n);
    info->tags_count = (int)arr_n;

    // Optional strings
    (void)nostr_json_get_string(json, "posting_policy", &info->posting_policy);
    (void)nostr_json_get_string(json, "payments_url", &info->payments_url);
    (void)nostr_json_get_string(json, "icon", &info->icon);

    // fees parsing (admission/subscription arrays and first publication entry)
    size_t len = 0;
    int tmpi = 0;
    char *tmps = NULL;
    if (nostr_json_get_array_length_at(json, "fees", "admission", &len) == 0 && len > 0) {
        if (!info->fees) info->fees = (RelayFeesDocument *)calloc(1, sizeof(RelayFeesDocument));
        if (info->fees) {
            info->fees->admission.count = (int)len;
            info->fees->admission.items = (Fee *)calloc(len, sizeof(Fee));
            for (size_t i = 0; i < len; i++) {
                if (nostr_json_get_int_in_object_array_at(json, "fees", "admission", i, "amount", &tmpi) == 0)
                    info->fees->admission.items[i].amount = tmpi;
                if (nostr_json_get_string_in_object_array_at(json, "fees", "admission", i, "unit", &tmps) == 0)
                    info->fees->admission.items[i].unit = tmps;
                tmps = NULL;
            }
        }
    }
    if (nostr_json_get_array_length_at(json, "fees", "subscription", &len) == 0 && len > 0) {
        if (!info->fees) info->fees = (RelayFeesDocument *)calloc(1, sizeof(RelayFeesDocument));
        if (info->fees) {
            info->fees->subscription.count = (int)len;
            info->fees->subscription.items = (Fee *)calloc(len, sizeof(Fee));
            for (size_t i = 0; i < len; i++) {
                if (nostr_json_get_int_in_object_array_at(json, "fees", "subscription", i, "amount", &tmpi) == 0)
                    info->fees->subscription.items[i].amount = tmpi;
                if (nostr_json_get_string_in_object_array_at(json, "fees", "subscription", i, "unit", &tmps) == 0)
                    info->fees->subscription.items[i].unit = tmps;
                tmps = NULL;
            }
        }
    }
    // publication: parse only the first entry if present
    if (nostr_json_get_array_length_at(json, "fees", "publication", &len) == 0 && len > 0) {
        if (!info->fees) info->fees = (RelayFeesDocument *)calloc(1, sizeof(RelayFeesDocument));
        if (info->fees) {
            size_t kinds_n = 0; int *kinds = NULL;
            (void)nostr_json_get_int_array_in_object_array_at(json, "fees", "publication", 0, "kinds", &kinds, &kinds_n);
            if (kinds && kinds_n > 0) {
                info->fees->publication.kinds = kinds;
                info->fees->publication.count = (int)kinds_n;
            }
            if (nostr_json_get_int_in_object_array_at(json, "fees", "publication", 0, "amount", &tmpi) == 0)
                info->fees->publication.amount = tmpi;
            if (nostr_json_get_string_in_object_array_at(json, "fees", "publication", 0, "unit", &tmps) == 0)
                info->fees->publication.unit = tmps;
            tmps = NULL;
        }
    }
    return info;
}

RelayInformationDocument* nostr_nip11_fetch_info(const char *url) {
    if (!url) return NULL;
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    MemoryStruct chunk = {0};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "libnostr-nip11/1.0");

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Accept: application/nostr+json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    RelayInformationDocument *info = NULL;
    CURLcode res = curl_easy_perform(curl);
    if (res == CURLE_OK && chunk.data) {
        info = parse_json_to_doc(chunk.data, url);
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(chunk.data);
    return info;
}

RelayInformationDocument* nostr_nip11_parse_info(const char *json) {
    if (!json) return NULL;
    return parse_json_to_doc(json, NULL);
}

void nostr_nip11_free_info(RelayInformationDocument *info) {
    if (!info) return;
    if (info->url) free(info->url);
    if (info->name) free(info->name);
    if (info->description) free(info->description);
    if (info->pubkey) free(info->pubkey);
    if (info->contact) free(info->contact);
    if (info->software) free(info->software);
    if (info->version) free(info->version);
    if (info->supported_nips) free(info->supported_nips);
    if (info->relay_countries) {
        for (int i = 0; i < info->relay_countries_count; i++) free(info->relay_countries[i]);
        free(info->relay_countries);
    }
    if (info->language_tags) {
        for (int i = 0; i < info->language_tags_count; i++) free(info->language_tags[i]);
        free(info->language_tags);
    }
    if (info->tags) {
        for (int i = 0; i < info->tags_count; i++) free(info->tags[i]);
        free(info->tags);
    }
    if (info->posting_policy) free(info->posting_policy);
    if (info->payments_url) free(info->payments_url);
    if (info->icon) free(info->icon);
    if (info->limitation) free(info->limitation);
    if (info->fees) {
        if (info->fees->admission.items) {
            for (int i = 0; i < info->fees->admission.count; i++) {
                if (info->fees->admission.items[i].unit) free(info->fees->admission.items[i].unit);
            }
            free(info->fees->admission.items);
        }
        if (info->fees->subscription.items) {
            for (int i = 0; i < info->fees->subscription.count; i++) {
                if (info->fees->subscription.items[i].unit) free(info->fees->subscription.items[i].unit);
            }
            free(info->fees->subscription.items);
        }
        if (info->fees->publication.kinds) free(info->fees->publication.kinds);
        if (info->fees->publication.unit) free(info->fees->publication.unit);
        free(info->fees);
    }
    free(info);
}
