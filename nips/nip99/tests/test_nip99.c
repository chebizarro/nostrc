#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "nostr-event.h"
#include "nostr-tag.h"
#include "nostr/nip99/nip99.h"

static void test_parse_listing(void) {
    NostrEvent *ev = nostr_event_new();
    nostr_event_set_kind(ev, NOSTR_NIP99_KIND_LISTING);

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    nostr_tags_append(tags, nostr_tag_new("d", "vintage-camera-001", NULL));
    nostr_tags_append(tags, nostr_tag_new("title", "Vintage Camera", NULL));
    nostr_tags_append(tags, nostr_tag_new("summary", "A great vintage camera", NULL));
    nostr_tags_append(tags, nostr_tag_new("published_at", "1700000000", NULL));
    nostr_tags_append(tags, nostr_tag_new("location", "NYC", NULL));
    nostr_tags_append(tags, nostr_tag_new("price", "100", "USD", NULL));

    NostrNip99Listing listing;
    assert(nostr_nip99_parse(ev, &listing) == 0);
    assert(strcmp(listing.identifier, "vintage-camera-001") == 0);
    assert(strcmp(listing.title, "Vintage Camera") == 0);
    assert(strcmp(listing.summary, "A great vintage camera") == 0);
    assert(listing.published_at == 1700000000);
    assert(strcmp(listing.location, "NYC") == 0);
    assert(strcmp(listing.price.amount, "100") == 0);
    assert(strcmp(listing.price.currency, "USD") == 0);
    assert(listing.price.frequency == NULL);

    nostr_event_free(ev);
}

static void test_parse_price_with_frequency(void) {
    NostrEvent *ev = nostr_event_new();

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    nostr_tags_append(tags, nostr_tag_new("d", "apartment-rent", NULL));
    nostr_tags_append(tags, nostr_tag_new("price", "1500", "EUR", "month", NULL));

    NostrNip99Listing listing;
    assert(nostr_nip99_parse(ev, &listing) == 0);
    assert(strcmp(listing.price.amount, "1500") == 0);
    assert(strcmp(listing.price.currency, "EUR") == 0);
    assert(strcmp(listing.price.frequency, "month") == 0);

    nostr_event_free(ev);
}

static void test_validate(void) {
    NostrEvent *ev = nostr_event_new();
    nostr_event_set_kind(ev, NOSTR_NIP99_KIND_LISTING);

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    nostr_tags_append(tags, nostr_tag_new("d", "item", NULL));
    nostr_tags_append(tags, nostr_tag_new("title", "Item", NULL));
    nostr_tags_append(tags, nostr_tag_new("summary", "An item", NULL));
    nostr_tags_append(tags, nostr_tag_new("published_at", "1700000000", NULL));
    nostr_tags_append(tags, nostr_tag_new("location", "NYC", NULL));
    nostr_tags_append(tags, nostr_tag_new("price", "50", "USD", NULL));

    assert(nostr_nip99_validate(ev));

    /* Missing price = invalid */
    NostrEvent *ev2 = nostr_event_new();
    nostr_event_set_kind(ev2, NOSTR_NIP99_KIND_LISTING);
    NostrTags *tags2 = (NostrTags *)nostr_event_get_tags(ev2);
    nostr_tags_append(tags2, nostr_tag_new("d", "item", NULL));
    nostr_tags_append(tags2, nostr_tag_new("title", "Item", NULL));
    assert(!nostr_nip99_validate(ev2));

    nostr_event_free(ev);
    nostr_event_free(ev2);
}

static void test_is_listing(void) {
    NostrEvent *ev = nostr_event_new();

    nostr_event_set_kind(ev, NOSTR_NIP99_KIND_LISTING);
    assert(nostr_nip99_is_listing(ev));

    nostr_event_set_kind(ev, NOSTR_NIP99_KIND_DRAFT_LISTING);
    assert(nostr_nip99_is_listing(ev));

    nostr_event_set_kind(ev, 1);
    assert(!nostr_nip99_is_listing(ev));

    nostr_event_free(ev);
}

static void test_images(void) {
    NostrEvent *ev = nostr_event_new();

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    nostr_tags_append(tags, nostr_tag_new("d", "item", NULL));
    nostr_tags_append(tags, nostr_tag_new("image", "https://example.com/1.jpg",
                                           "800x600", NULL));
    nostr_tags_append(tags, nostr_tag_new("image", "https://example.com/2.jpg", NULL));

    assert(nostr_nip99_count_images(ev) == 2);

    NostrNip99Image images[4];
    size_t count = 0;
    assert(nostr_nip99_get_images(ev, images, 4, &count) == 0);
    assert(count == 2);
    assert(strcmp(images[0].url, "https://example.com/1.jpg") == 0);
    assert(strcmp(images[0].dimensions, "800x600") == 0);
    assert(strcmp(images[1].url, "https://example.com/2.jpg") == 0);
    assert(images[1].dimensions == NULL);

    nostr_event_free(ev);
}

static void test_categories(void) {
    NostrEvent *ev = nostr_event_new();

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    nostr_tags_append(tags, nostr_tag_new("d", "item", NULL));
    nostr_tags_append(tags, nostr_tag_new("t", "electronics", NULL));
    nostr_tags_append(tags, nostr_tag_new("t", "vintage", NULL));
    nostr_tags_append(tags, nostr_tag_new("p", "somepubkey", NULL));

    const char *cats[4];
    size_t count = 0;
    assert(nostr_nip99_get_categories(ev, cats, 4, &count) == 0);
    assert(count == 2);
    assert(strcmp(cats[0], "electronics") == 0);
    assert(strcmp(cats[1], "vintage") == 0);

    nostr_event_free(ev);
}

static void test_create_listing(void) {
    NostrEvent *ev = nostr_event_new();

    NostrNip99Listing listing = {
        .identifier = "my-listing",
        .title = "Widget",
        .summary = "A fine widget",
        .location = "London",
        .published_at = 1700000000,
        .price = {
            .amount = "25",
            .currency = "GBP",
            .frequency = NULL,
        },
    };

    assert(nostr_nip99_create_listing(ev, &listing) == 0);
    assert(nostr_event_get_kind(ev) == NOSTR_NIP99_KIND_LISTING);

    /* Add extras */
    assert(nostr_nip99_add_image(ev, "https://example.com/widget.jpg", "512x512") == 0);
    assert(nostr_nip99_add_category(ev, "gadgets") == 0);

    /* Round-trip parse */
    NostrNip99Listing parsed;
    assert(nostr_nip99_parse(ev, &parsed) == 0);
    assert(strcmp(parsed.identifier, "my-listing") == 0);
    assert(strcmp(parsed.title, "Widget") == 0);
    assert(strcmp(parsed.location, "London") == 0);
    assert(strcmp(parsed.price.amount, "25") == 0);
    assert(strcmp(parsed.price.currency, "GBP") == 0);

    assert(nostr_nip99_count_images(ev) == 1);

    const char *cats[2];
    size_t cat_count = 0;
    assert(nostr_nip99_get_categories(ev, cats, 2, &cat_count) == 0);
    assert(cat_count == 1);
    assert(strcmp(cats[0], "gadgets") == 0);

    nostr_event_free(ev);
}

static void test_invalid_inputs(void) {
    assert(!nostr_nip99_is_listing(NULL));
    assert(!nostr_nip99_validate(NULL));
    assert(nostr_nip99_count_images(NULL) == 0);

    NostrNip99Listing listing;
    assert(nostr_nip99_parse(NULL, &listing) == -EINVAL);

    NostrNip99Image images[2];
    size_t count;
    assert(nostr_nip99_get_images(NULL, images, 2, &count) == -EINVAL);

    const char *cats[2];
    assert(nostr_nip99_get_categories(NULL, cats, 2, &count) == -EINVAL);

    assert(nostr_nip99_create_listing(NULL, &listing) == -EINVAL);
    assert(nostr_nip99_add_image(NULL, "url", NULL) == -EINVAL);
    assert(nostr_nip99_add_category(NULL, "x") == -EINVAL);
}

int main(void) {
    test_parse_listing();
    test_parse_price_with_frequency();
    test_validate();
    test_is_listing();
    test_images();
    test_categories();
    test_create_listing();
    test_invalid_inputs();
    printf("nip99 ok\n");
    return 0;
}
