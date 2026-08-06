#include <glib.h>
#include <nostr/nip19/nip19.h>
#include <nostr-gtk-1.0/content_renderer.h>
#include <nostr-gobject-1.0/storage_ndb.h>
#include <stdlib.h>
#include <string.h>

static GnContentDescriptor *
descriptor_at(GnContentRenderResult *result,
              guint index,
              GnContentDescriptorType expected_type)
{
  g_assert_nonnull(result);
  g_assert_nonnull(result->descriptors);
  g_assert_cmpuint(index, <, result->descriptors->len);
  GnContentDescriptor *descriptor =
    g_ptr_array_index(result->descriptors, index);
  g_assert_nonnull(descriptor);
  g_assert_cmpint(descriptor->type, ==, expected_type);
  return descriptor;
}

static void
test_classification_and_elision(void)
{
  guint8 bytes[32];
  memset(bytes, 0x11, sizeof(bytes));

  char *note = NULL;
  char *npub = NULL;
  g_assert_cmpint(nostr_nip19_encode_note(bytes, &note), ==, 0);
  g_assert_cmpint(nostr_nip19_encode_npub(bytes, &npub), ==, 0);

  g_autofree gchar *content = g_strdup_printf(
    "before https://cdn.test/photo.jpg "
    "https://cdn.test/movie.mp4 "
    "https://example.test/page "
    "nostr:%s nostr:%s after",
    note, npub);

  GnContentRenderResult *result =
    gn_content_parse(content, -1, NULL, NULL);
  g_assert_nonnull(result);
  g_assert_false(result->used_block_fallback);
  g_assert_cmpuint(result->descriptors->len, ==, 5);

  GnContentDescriptor *image =
    descriptor_at(result, 0, GN_CONTENT_DESCRIPTOR_MEDIA_IMAGE);
  GnContentDescriptor *video =
    descriptor_at(result, 1, GN_CONTENT_DESCRIPTOR_MEDIA_VIDEO);
  GnContentDescriptor *link =
    descriptor_at(result, 2, GN_CONTENT_DESCRIPTOR_LINK_PREVIEW);
  GnContentDescriptor *event =
    descriptor_at(result, 3, GN_CONTENT_DESCRIPTOR_NOSTR_EVENT_REF);
  GnContentDescriptor *profile =
    descriptor_at(result, 4, GN_CONTENT_DESCRIPTOR_NOSTR_PROFILE_REF);

  g_assert_cmpstr(image->url, ==, "https://cdn.test/photo.jpg");
  g_assert_cmpstr(video->url, ==, "https://cdn.test/movie.mp4");
  g_assert_cmpstr(link->url, ==, "https://example.test/page");
  g_assert_cmpstr(event->id, ==,
                  "1111111111111111111111111111111111111111111111111111111111111111");
  g_assert_cmpstr(profile->pubkey, ==, event->id);

  g_assert_null(strstr(result->markup, image->url));
  g_assert_null(strstr(result->markup, video->url));
  g_assert_null(strstr(result->markup, note));
  g_assert_nonnull(strstr(result->markup, link->url));
  g_assert_nonnull(strstr(result->markup, "@npub1"));
  g_assert_null(strstr(result->plain_text, image->url));
  g_assert_null(strstr(result->plain_text, note));

  g_assert_cmpuint(result->media_urls->len, ==, 2);
  g_assert_cmpstr(result->first_og_url, ==, link->url);
  g_assert_cmpstr(result->first_nostr_ref, ==, event->original);

  gnostr_content_render_result_free(result);
  free(note);
  free(npub);
}

static gchar *
pointer_to_bech32(const NostrPointer *pointer)
{
  char *encoded = NULL;
  g_assert_cmpint(nostr_pointer_to_bech32(pointer, &encoded), ==, 0);
  return encoded;
}

static void
test_tlv_reference_classification_and_hints(void)
{
  const char *hex =
    "2222222222222222222222222222222222222222222222222222222222222222";
  const char *relays[] = { "wss://relay.example" };

  NostrNEventConfig event_config = {
    .id = hex,
    .author = hex,
    .kind = 1,
    .relays = relays,
    .relays_count = G_N_ELEMENTS(relays),
  };
  NostrNAddrConfig addr_config = {
    .identifier = "article",
    .public_key = hex,
    .kind = 30023,
    .relays = relays,
    .relays_count = G_N_ELEMENTS(relays),
  };
  NostrNProfileConfig profile_config = {
    .public_key = hex,
    .relays = relays,
    .relays_count = G_N_ELEMENTS(relays),
  };

  NostrPointer *event_pointer = NULL;
  NostrPointer *addr_pointer = NULL;
  NostrPointer *profile_pointer = NULL;
  g_assert_cmpint(nostr_pointer_from_nevent_config(&event_config,
                                                   &event_pointer), ==, 0);
  g_assert_cmpint(nostr_pointer_from_naddr_config(&addr_config,
                                                  &addr_pointer), ==, 0);
  g_assert_cmpint(nostr_pointer_from_nprofile_config(&profile_config,
                                                     &profile_pointer), ==, 0);

  g_autofree gchar *nevent = pointer_to_bech32(event_pointer);
  g_autofree gchar *naddr = pointer_to_bech32(addr_pointer);
  g_autofree gchar *nprofile = pointer_to_bech32(profile_pointer);
  g_autofree gchar *content =
    g_strdup_printf("nostr:%s nostr:%s nostr:%s",
                    nevent, naddr, nprofile);

  GnContentRenderResult *result =
    gn_content_parse(content, -1, NULL, NULL);
  g_assert_nonnull(result);
  g_assert_cmpuint(result->descriptors->len, ==, 3);

  GnContentDescriptor *event =
    descriptor_at(result, 0, GN_CONTENT_DESCRIPTOR_NOSTR_EVENT_REF);
  GnContentDescriptor *address =
    descriptor_at(result, 1, GN_CONTENT_DESCRIPTOR_NOSTR_EVENT_REF);
  GnContentDescriptor *profile =
    descriptor_at(result, 2, GN_CONTENT_DESCRIPTOR_NOSTR_PROFILE_REF);

  g_assert_cmpstr(event->id, ==, hex);
  g_assert_cmpstr(address->id, ==, "article");
  g_assert_cmpstr(address->pubkey, ==, hex);
  g_assert_cmpstr(profile->pubkey, ==, hex);
  g_assert_nonnull(event->relay_hints);
  g_assert_cmpstr(event->relay_hints[0], ==, relays[0]);
  g_assert_nonnull(profile->relay_hints);
  g_assert_cmpstr(profile->relay_hints[0], ==, relays[0]);

  g_assert_null(strstr(result->markup, nevent));
  g_assert_null(strstr(result->markup, naddr));
  g_assert_nonnull(strstr(result->markup, "@nprofile"));

  gnostr_content_render_result_free(result);
  nostr_pointer_free(event_pointer);
  nostr_pointer_free(addr_pointer);
  nostr_pointer_free(profile_pointer);
}

static void
test_imeta_enrichment_and_mime_classification(void)
{
  const char *url = "https://cdn.test/blob/abc";
  const char *tags_json =
    "[[\"imeta\",\"url https://cdn.test/blob/abc\","
    "\"m video/mp4\",\"dim 1920x1080\","
    "\"thumb https://cdn.test/poster.jpg\"]]";

  GnContentRenderResult *result =
    gn_content_parse(url, -1, tags_json, NULL);
  g_assert_nonnull(result);
  g_assert_cmpuint(result->descriptors->len, ==, 1);

  GnContentDescriptor *video =
    descriptor_at(result, 0, GN_CONTENT_DESCRIPTOR_MEDIA_VIDEO);
  g_assert_cmpstr(video->url, ==, url);
  g_assert_cmpint(video->width, ==, 1920);
  g_assert_cmpint(video->height, ==, 1080);
  g_assert_cmpstr(video->thumbnail_url, ==,
                  "https://cdn.test/poster.jpg");
  g_assert_cmpstr(result->markup, ==, "");
  g_assert_cmpstr(result->plain_text, ==, "");

  gnostr_content_render_result_free(result);
}

static void
assert_descriptors_equal(const GPtrArray *a,
                         const GPtrArray *b)
{
  g_assert_nonnull(a);
  g_assert_nonnull(b);
  g_assert_cmpuint(a->len, ==, b->len);
  for (guint i = 0; i < a->len; i++) {
    const GnContentDescriptor *left = g_ptr_array_index((GPtrArray *)a, i);
    const GnContentDescriptor *right = g_ptr_array_index((GPtrArray *)b, i);
    g_assert_cmpint(left->type, ==, right->type);
    g_assert_cmpstr(left->url, ==, right->url);
    g_assert_cmpstr(left->original, ==, right->original);
    g_assert_cmpstr(left->id, ==, right->id);
    g_assert_cmpstr(left->pubkey, ==, right->pubkey);
  }
}

static void
test_cached_blocks_match_fallback_with_utf8(void)
{
  static const char *note =
    "note1zyg3zyg3zyg3zyg3zyg3zyg3zyg3zyg3zyg3zyg3zyg3zyg3zygsglnzgl";
  g_autofree char *content = g_strdup_printf(
    "caf\xc3\xa9 \xce\xb1\xe2\x80\x8b\xce\xb2 https://example.test/page #nostr "
    "nostr:%s \xf0\x9f\x99\x82\xe7\xb5\x82",
    note);

  storage_ndb_blocks *blocks =
    storage_ndb_parse_content_blocks(content, (int)strlen(content));
  g_assert_nonnull(blocks);

  GnContentRenderResult *cached =
    gn_content_parse_with_blocks(content, -1, NULL, blocks, NULL);
  GnContentRenderResult *fallback =
    gn_content_parse(content, -1, NULL, NULL);
  g_assert_nonnull(cached);
  g_assert_nonnull(fallback);
  g_assert_cmpstr(cached->markup, ==, fallback->markup);
  g_assert_cmpstr(cached->plain_text, ==, fallback->plain_text);
  assert_descriptors_equal(cached->descriptors, fallback->descriptors);
  g_assert_true(g_utf8_validate(cached->markup, -1, NULL));
  g_assert_true(g_utf8_validate(cached->plain_text, -1, NULL));

  gnostr_content_render_result_free(cached);
  gnostr_content_render_result_free(fallback);
  storage_ndb_blocks_free(blocks);
}

static void
test_legacy_wrapper_uses_elided_parse(void)
{
  GnContentRenderResult *result =
    gnostr_render_content("caption https://cdn.test/photo.webp", -1, NULL);
  g_assert_nonnull(result);
  g_assert_cmpuint(result->media_urls->len, ==, 1);
  g_assert_cmpstr(g_ptr_array_index(result->media_urls, 0), ==,
                  "https://cdn.test/photo.webp");
  g_assert_null(strstr(result->markup, "photo.webp"));
  g_assert_nonnull(strstr(result->markup, "caption"));
  gnostr_content_render_result_free(result);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/nostr-gtk/content-parser/classification-elision",
                  test_classification_and_elision);
  g_test_add_func("/nostr-gtk/content-parser/tlv-refs",
                  test_tlv_reference_classification_and_hints);
  g_test_add_func("/nostr-gtk/content-parser/imeta",
                  test_imeta_enrichment_and_mime_classification);
  g_test_add_func("/nostr-gtk/content-parser/legacy-wrapper",
                  test_legacy_wrapper_uses_elided_parse);
  g_test_add_func("/nostr-gtk/content-parser/cached-blocks-utf8-equivalence",
                  test_cached_blocks_match_fallback_with_utf8);
  return g_test_run();
}
