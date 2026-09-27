/*
 * test_nip29_events — moderation events of the NIP-29 plugin (nostrc-4gf4).
 *
 * docs/nips/29.md: kind:9007 create-group takes no tags besides `h`;
 * group-metadata is set with kind:9002 edit-metadata (at most one parent);
 * kind:9010 update-pin-list carries the full ordered e/a list.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "gn-nip29-events.h"

#include <json-glib/json-glib.h>

static JsonObject *
parse(const char *json, JsonParser **out_parser)
{
  JsonParser *p = json_parser_new();
  g_assert_true(json_parser_load_from_data(p, json, -1, NULL));
  *out_parser = p;
  return json_node_get_object(json_parser_get_root(p));
}

/* "name=value|flag|..." rendering of the tags, for compact assertions. */
static char *
tags_string(JsonObject *o)
{
  JsonArray *tags = json_object_get_array_member(o, "tags");
  GString *s = g_string_new(NULL);
  for (guint i = 0; i < json_array_get_length(tags); i++) {
    JsonArray *t = json_array_get_array_element(tags, i);
    if (i) g_string_append_c(s, '|');
    g_string_append(s, json_array_get_string_element(t, 0));
    for (guint j = 1; j < json_array_get_length(t); j++)
      g_string_append_printf(s, "=%s", json_array_get_string_element(t, j));
  }
  return g_string_free(s, FALSE);
}

static void
test_create_group(void)
{
  g_autofree char *json = gn_nip29_build_create_group_json("pizza", 1700000000);
  JsonParser *p = NULL;
  JsonObject *o = parse(json, &p);
  g_assert_cmpint(json_object_get_int_member(o, "kind"), ==, 9007);
  g_assert_cmpint(json_object_get_int_member(o, "created_at"), ==, 1700000000);
  g_autofree char *tags = tags_string(o);
  g_assert_cmpstr(tags, ==, "h=pizza");
  g_object_unref(p);
}

static void
test_edit_metadata(void)
{
  GnNip29Metadata md = { .name = "Pizza Lovers", .about = "we love pizza",
                         .picture = "https://p/x.png", .banner = "https://p/b.png",
                         .parent = "food", .is_private = TRUE, .is_closed = TRUE };
  g_assert_false(gn_nip29_metadata_is_empty(&md));
  g_autofree char *json = gn_nip29_build_edit_metadata_json("pizza", &md, 1700000001);
  JsonParser *p = NULL;
  JsonObject *o = parse(json, &p);
  g_assert_cmpint(json_object_get_int_member(o, "kind"), ==, 9002);
  g_autofree char *tags = tags_string(o);
  g_assert_cmpstr(tags, ==,
                  "h=pizza|name=Pizza Lovers|about=we love pizza|picture=https://p/x.png|"
                  "banner=https://p/b.png|private|closed|parent=food");
  g_object_unref(p);

  /* Empty strings are left out; nothing set means no 9002 is needed. */
  GnNip29Metadata none = { .name = "", .about = NULL };
  g_assert_true(gn_nip29_metadata_is_empty(&none));
  g_assert_true(gn_nip29_metadata_is_empty(NULL));
  GnNip29Metadata flags = { .is_restricted = TRUE, .is_hidden = TRUE };
  g_assert_false(gn_nip29_metadata_is_empty(&flags));
  g_autofree char *json2 = gn_nip29_build_edit_metadata_json("pizza", &flags, 1);
  o = parse(json2, &p);
  g_autofree char *tags2 = tags_string(o);
  g_assert_cmpstr(tags2, ==, "h=pizza|restricted|hidden");
  g_object_unref(p);
}

static void
test_update_pin_list(void)
{
  const char *refs[] = {
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
    "30023:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb:post",
    "not-a-ref",
    NULL,
  };
  g_autofree char *json = gn_nip29_build_update_pin_list_json("pizza", refs, 5);
  JsonParser *p = NULL;
  JsonObject *o = parse(json, &p);
  g_assert_cmpint(json_object_get_int_member(o, "kind"), ==, 9010);
  g_autofree char *tags = tags_string(o);
  g_assert_cmpstr(tags, ==,
                  "h=pizza|e=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|"
                  "a=30023:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb:post");
  g_object_unref(p);
  /* An empty list clears the pins. */
  g_autofree char *cleared = gn_nip29_build_update_pin_list_json("pizza", NULL, 6);
  o = parse(cleared, &p);
  g_autofree char *tags2 = tags_string(o);
  g_assert_cmpstr(tags2, ==, "h=pizza");
  g_object_unref(p);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/nip29/events/create-group", test_create_group);
  g_test_add_func("/nip29/events/edit-metadata", test_edit_metadata);
  g_test_add_func("/nip29/events/update-pin-list", test_update_pin_list);
  return g_test_run();
}
