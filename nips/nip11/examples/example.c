#include "nip11.h"
#include <stdio.h>
#include <stdlib.h>

static void print_string_array(const char *label, char **arr, int n) {
    printf("%s: ", label);
    if (!arr || n <= 0) { printf("[]\n"); return; }
    printf("[");
    for (int i = 0; i < n; i++) {
        printf("%s\"%s\"", (i?", ":""), arr[i] ? arr[i] : "");
    }
    printf("]\n");
}

static void print_int_array(const char *label, int *arr, int n) {
    printf("%s: ", label);
    if (!arr || n <= 0) { printf("[]\n"); return; }
    printf("[");
    for (int i = 0; i < n; i++) {
        printf("%s%d", (i?", ":""), arr[i]);
    }
    printf("]\n");
}

int main(int argc, char **argv) {
    const char *url = argc > 1 ? argv[1] : "https://relay.example.com";
    RelayInformationDocument *info = nostr_nip11_fetch_info(url);
    if (!info) {
        fprintf(stderr, "Failed to fetch relay info from %s\n", url);
        return 1;
    }

    printf("URL: %s\n", info->url ? info->url : url);
    printf("Name: %s\n", info->name ? info->name : "(null)");
    printf("Description: %s\n", info->description ? info->description : "(null)");
    printf("Pubkey: %s\n", info->pubkey ? info->pubkey : "(null)");
    printf("Contact: %s\n", info->contact ? info->contact : "(null)");
    printf("Software: %s\n", info->software ? info->software : "(null)");
    printf("Version: %s\n", info->version ? info->version : "(null)");
    print_int_array("Supported NIPs", info->supported_nips, info->supported_nips_count);
    if (info->limitation) {
        printf("Limitation.max_message_length: %d\n", info->limitation->max_message_length);
        printf("Limitation.max_subscriptions: %d\n", info->limitation->max_subscriptions);
        printf("Limitation.auth_required: %s\n", info->limitation->auth_required?"true":"false");
        printf("Limitation.payment_required: %s\n", info->limitation->payment_required?"true":"false");
    }
    print_string_array("Relay Countries", info->relay_countries, info->relay_countries_count);
    print_string_array("Language Tags", info->language_tags, info->language_tags_count);
    print_string_array("Tags", info->tags, info->tags_count);
    printf("Posting Policy: %s\n", info->posting_policy ? info->posting_policy : "(null)");
    printf("Payments URL: %s\n", info->payments_url ? info->payments_url : "(null)");
    printf("Icon: %s\n", info->icon ? info->icon : "(null)");

    if (info->fees) {
        printf("Fees:\n");
        printf("  Admission: count=%d\n", info->fees->admission.count);
        for (int i = 0; i < info->fees->admission.count; i++) {
            printf("    - amount=%d unit=%s\n", info->fees->admission.items[i].amount,
                   info->fees->admission.items[i].unit ? info->fees->admission.items[i].unit : "");
        }
        printf("  Subscription: count=%d\n", info->fees->subscription.count);
        for (int i = 0; i < info->fees->subscription.count; i++) {
            printf("    - amount=%d unit=%s\n", info->fees->subscription.items[i].amount,
                   info->fees->subscription.items[i].unit ? info->fees->subscription.items[i].unit : "");
        }
        if (info->fees->publication.count > 0 || info->fees->publication.amount || info->fees->publication.unit) {
            printf("  Publication: kinds=");
            if (info->fees->publication.kinds && info->fees->publication.count > 0) {
                printf("[");
                for (int i = 0; i < info->fees->publication.count; i++) {
                    printf("%s%d", (i?", ":""), info->fees->publication.kinds[i]);
                }
                printf("] ");
            } else {
                printf("[] ");
            }
            printf("amount=%d unit=%s\n", info->fees->publication.amount,
                   info->fees->publication.unit ? info->fees->publication.unit : "");
        }
    }

    nostr_nip11_free_info(info);
    return 0;
}
