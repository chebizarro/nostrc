#include "nip05.h"
#include "utils.h"
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <json-c/json.h>

static const char *NIP05_REGEX = "^(?:([\\w.+-]+)@)?([\\w_-]+(\\.[\\w_-]+)+)$";

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

bool is_valid_identifier(const char *input) {
    regex_t regex;
    regcomp(&regex, NIP05_REGEX, REG_EXTENDED);
    int result = regexec(&regex, input, 0, NULL, 0);
    regfree(&regex);
    return result == 0;
}

int parse_identifier(const char *fullname, char **name, char **domain) {
    regex_t regex;
    regmatch_t pmatch[4];
    regcomp(&regex, NIP05_REGEX, REG_EXTENDED);

    if (regexec(&regex, fullname, 4, pmatch, 0) != 0) {
        regfree(&regex);
        return -1; // Invalid identifier
    }

    if (pmatch[1].rm_so == -1) {
        *name = strdup("_");
    } else {
        *name = strndup(fullname + pmatch[1].rm_so, pmatch[1].rm_eo - pmatch[1].rm_so);
    }
    *domain = strndup(fullname + pmatch[2].rm_so, pmatch[2].rm_eo - pmatch[2].rm_so);

    regfree(&regex);
    return 0;
}

int fetch_nip05(const char *fullname, WellKnownResponse *response, char **name) {
    char *domain;
    if (parse_identifier(fullname, name, &domain) != 0) {
        return -1; // Failed to parse identifier
    }

    CURL *curl;
    CURLcode res;
    MemoryStruct chunk = {0};

    curl = curl_easy_init();
    if (!curl) {
        free(domain);
        return -1; // Failed to initialize curl
    }

    char url[256];
    snprintf(url, sizeof(url), "https://%s/.well-known/nostr.json?name=%s", domain, *name);
    free(domain);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);

    res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        curl_easy_cleanup(curl);
        free(chunk.data);
        return -1; // Failed to perform request
    }

    json_object *json_response = json_tokener_parse(chunk.data);
    free(chunk.data);
    if (!json_response) {
        curl_easy_cleanup(curl);
        return -1; // Failed to parse JSON
    }

    json_object *json_names = json_object_object_get(json_response, "names");
    json_object *json_relays = json_object_object_get(json_response, "relays");

    if (json_names) {
        response->names = NULL;
        int name_count = json_object_object_length(json_names);
        response->names = malloc((name_count + 1) * sizeof(char *));
        json_object_object_foreach(json_names, key, val) {
            response->names[json_object_object_length(json_names)] = strdup(json_object_get_string(val));
        }
        response->names[json_object_object_length(json_names)] = NULL;
    }

    if (json_relays) {
        response->relays = NULL;
        int relay_count = json_object_object_length(json_relays);
        response->relays = malloc((relay_count + 1) * sizeof(char *));
        json_object_object_foreach(json_relays, key, val) {
            response->relays[json_object_object_length(json_relays)] = strdup(json_object_get_string(val));
        }
        response->relays[json_object_object_length(json_relays)] = NULL;
    }

    json_object_put(json_response);
    curl_easy_cleanup(curl);

    return 0;
}

int query_identifier(const char *fullname, char **pubkey, char ***relays) {
    WellKnownResponse response;
    char *name;
    if (fetch_nip05(fullname, &response, &name) != 0) {
        return -1; // Failed to fetch NIP-05 data
    }

    *pubkey = NULL;
    for (int i = 0; response.names[i] != NULL; i++) {
        if (strcmp(response.names[i], name) == 0) {
            *pubkey = strdup(response.names[i]);
            break;
        }
    }

    if (*pubkey && response.relays) {
        *relays = response.relays;
    } else {
        *relays = NULL;
    }

    free(name);
    for (int i = 0; response.names[i] != NULL; i++) {
        free(response.names[i]);
    }
    free(response.names);

    return (*pubkey != NULL) ? 0 : -1;
}

char* normalize_identifier(const char *fullname) {
    if (strncmp(fullname, "_@", 2) == 0) {
        return strdup(fullname + 2);
    }
    return strdup(fullname);
}
