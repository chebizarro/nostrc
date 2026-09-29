/* A local NIP-29 relay for Groundhog's group service tests (charter H2 for
 * groups): a libsoup WebSocket server on 127.0.0.1 with its own keypair.
 *
 *  - NIP-11: a plain GET of the relay URL (no Upgrade) returns
 *    {"self": <relay key>, ...}; nip11_no_key leaves the key out,
 *    nip11_pubkey_only gives only "pubkey" (an administrator's key, here a
 *    member's), nip11_redirect answers 302 to another host instead.
 *  - Groups: it signs 39000-39003 for each group with the relay key and
 *    replaces them (and pushes them to live subscriptions) on every change.
 *    partial_members makes the 39002 list the admins only (a subset).
 *  - REQ: every stored event matching the filters (each filter's limit
 *    honoured, newest first), then EOSE (none with hold_eose: a backfill
 *    cut off before its end); the subscription stays live.
 *  - EVENT: signed and verified; a known id is OK true "duplicate:". Kinds
 *    9-12 need a member author (OK false "restricted:") and every `previous`
 *    ref must name a stored event of the group ("invalid:"). 9021 follows
 *    the group's join policy (auto, pending: OK false "restricted: ... pending
 *    approval", deny: "blocked:") after the member ("duplicate:"), invite
 *    code and closed ("restricted: ... closed") checks; an admission emits a
 *    relay-signed 9000 and a new 39002. 9022 removes the author with a
 *    relay-signed 9001. 9000/9001/9002/9005/9009 need an admin whose role
 *    allows them ("admin": all, "moderator": 9005 only) and apply the change.
 *  - 9007 (allow_create): creates the group named by its h tag with the
 *    author as its "admin" (OK true); without allow_create "blocked:", for a
 *    group that exists "invalid:". 9002 sets name, about, private and
 *    closed as sent (the complete metadata, as NIP-29 edits replace it).
 *  - NIP-42 (require_auth): each connection gets a challenge; until it has
 *    authenticated, a REQ is CLOSED "auth-required:" and an EVENT is OK false
 *    "auth-required:". refuse_auth answers every AUTH with OK false.
 * Waits iterate the default main context; their deadlines are failure
 * bounds only. Header-only; include once per test executable. */
#ifndef GH_TEST_NIP29_RELAY_H
#define GH_TEST_NIP29_RELAY_H

#include <gio/gio.h>
#include <libsoup/soup.h>
#include <nostr-envelope.h>
#include <nostr-event.h>
#include <nostr-filter.h>
#include <nostr-keys.h>
#include <nostr-tag.h>
#include <stdlib.h>
#include <string.h>

typedef enum { NIP29_JOIN_AUTO, NIP29_JOIN_PENDING, NIP29_JOIN_DENY } Nip29JoinPolicy;

typedef struct {
  gchar *id;
  gchar *name;
  gchar *about;
  gboolean closed;
  gboolean private_;
  gboolean restricted;
  gboolean partial_members;
  Nip29JoinPolicy join_policy;
  GHashTable *members;  /* pubkey -> 1 */
  GHashTable *admins;   /* pubkey -> role */
  GHashTable *pending;  /* pubkey -> 1 */
  GPtrArray *invites;   /* codes */
  GPtrArray *extra_tags;/* GStrv tags added to every 39000 */
} Nip29TestGroup;

typedef struct {
  SoupWebsocketConnection *connection;
  gchar *sub_id;
  NostrFilters *filters;
} Nip29Sub;

typedef struct {
  SoupServer *server;
  gchar *url;
  gchar *sk;
  gchar *pk;
  gint64 clock;             /* created_at of the relay's own events */
  gboolean require_auth;
  gboolean refuse_auth;
  gboolean hold_eose;       /* answer a REQ's stored events but never its EOSE */
  gboolean nip11_no_key;
  gboolean nip11_pubkey_only; /* "pubkey" (an admin's key) but no "self" */
  const gchar *nip11_pubkey_only_key; /* that "pubkey" */
  gboolean nip11_redirect;
  gboolean nip11_huge;
  gboolean allow_create;    /* 9007 creates groups (G20b) */
  GPtrArray *connections;
  GPtrArray *events;        /* NostrEvent, stored */
  GHashTable *groups;       /* id -> Nip29TestGroup */
  GPtrArray *subs;          /* Nip29Sub */
  GPtrArray *received;      /* JSON of every EVENT frame received, in order */
  GPtrArray *ok_messages;   /* "<kind> <accepted> <message>" per answered EVENT */
  GPtrArray *auth_pubkeys;
  GPtrArray *req_frames;    /* every REQ frame text */
  guint reqs;
  guint closed_reqs;
  guint nip11_gets;         /* Groundhog's NIP-11 fetches (no user agent) */
  guint auth_ok;
  guint events_seen;
} Nip29Relay;

/* ---- groups ------------------------------------------------------------- */


/* An event's canonical id as an interned string: nostr_event_get_id()
 * returns a fresh allocation, and the fixture compares ids inline, so the
 * copy is interned (reachable for LSan, never freed) and the allocation
 * released. */
static inline const char *
nip29_event_id(NostrEvent *event)
{
  char *id = nostr_event_get_id(event);
  const char *interned = id ? g_intern_string(id) : NULL;
  free(id);
  return interned;
}

static G_GNUC_UNUSED void
nip29_group_free(gpointer data)
{
  Nip29TestGroup *group = data;
  g_free(group->id);
  g_free(group->name);
  g_free(group->about);
  g_hash_table_unref(group->members);
  g_hash_table_unref(group->admins);
  g_hash_table_unref(group->pending);
  g_ptr_array_unref(group->invites);
  g_ptr_array_unref(group->extra_tags);
  g_free(group);
}

static G_GNUC_UNUSED void
nip29_sub_free(gpointer data)
{
  Nip29Sub *sub = data;
  g_free(sub->sub_id);
  nostr_filters_free(sub->filters);
  g_free(sub);
}

/* A frame to a client that may be closing its connection (the service
 * rebuilds a relay's REQ when its groups change): dropped unless open. */
static G_GNUC_UNUSED void
nip29_send(SoupWebsocketConnection *connection, const gchar *frame)
{
  if (soup_websocket_connection_get_state(connection) == SOUP_WEBSOCKET_STATE_OPEN)
    soup_websocket_connection_send_text(connection, frame);
}

static G_GNUC_UNUSED gchar *
nip29_event_json(NostrEvent *event)
{
  char *raw = nostr_event_serialize_compact(event);
  gchar *json = g_strdup(raw);
  free(raw);
  return json;
}

static G_GNUC_UNUSED const gchar *
nip29_tag_value(NostrEvent *event, const gchar *key)
{
  NostrTags *tags = nostr_event_get_tags(event);
  for (size_t i = 0; tags && i < nostr_tags_size(tags); i++) {
    NostrTag *tag = nostr_tags_get(tags, i);
    if (nostr_tag_size(tag) >= 2 && g_strcmp0(nostr_tag_get(tag, 0), key) == 0)
      return nostr_tag_get(tag, 1);
  }
  return NULL;
}

static G_GNUC_UNUSED NostrTag *
nip29_tag_find(NostrEvent *event, const gchar *key)
{
  NostrTags *tags = nostr_event_get_tags(event);
  for (size_t i = 0; tags && i < nostr_tags_size(tags); i++) {
    NostrTag *tag = nostr_tags_get(tags, i);
    if (nostr_tag_size(tag) >= 1 && g_strcmp0(nostr_tag_get(tag, 0), key) == 0)
      return tag;
  }
  return NULL;
}

/* Sends every live subscription that matches event its EVENT frame. */
static G_GNUC_UNUSED void
nip29_broadcast(Nip29Relay *relay, NostrEvent *event)
{
  g_autofree gchar *json = nip29_event_json(event);
  for (guint i = 0; i < relay->subs->len; i++) {
    Nip29Sub *sub = g_ptr_array_index(relay->subs, i);
    if (!nostr_filters_match(sub->filters, event))
      continue;
    g_autofree gchar *frame = g_strdup_printf("[\"EVENT\",\"%s\",%s]", sub->sub_id, json);
    if (soup_websocket_connection_get_state(sub->connection) == SOUP_WEBSOCKET_STATE_OPEN)
      soup_websocket_connection_send_text(sub->connection, frame);
  }
}

static G_GNUC_UNUSED gboolean
nip29_stored(Nip29Relay *relay, const gchar *id)
{
  for (guint i = 0; i < relay->events->len; i++)
    if (g_strcmp0(nip29_event_id(g_ptr_array_index(relay->events, i)), id) == 0)
      return TRUE;
  return FALSE;
}

/* Stores (taking ownership) and broadcasts; an addressable 3900x replaces the
 * previous one of its kind and d. */
static G_GNUC_UNUSED void
nip29_store(Nip29Relay *relay, NostrEvent *event)
{
  gint kind = nostr_event_get_kind(event);
  if (kind >= 39000 && kind < 40000) {
    const gchar *d = nip29_tag_value(event, "d");
    for (guint i = relay->events->len; i > 0; i--) {
      NostrEvent *old = g_ptr_array_index(relay->events, i - 1);
      if (nostr_event_get_kind(old) == kind && g_strcmp0(nip29_tag_value(old, "d"), d) == 0)
        g_ptr_array_remove_index(relay->events, i - 1);
    }
  }
  g_ptr_array_add(relay->events, event);
  nip29_broadcast(relay, event);
}

/* The relay's clock for what it signs now: real time, strictly increasing.
 * History posted before a test's join uses nip29_past() instead. */
static G_GNUC_UNUSED gint64
nip29_now(Nip29Relay *relay)
{
  relay->clock = MAX(relay->clock + 1, g_get_real_time() / G_USEC_PER_SEC);
  return relay->clock;
}

static G_GNUC_UNUSED gint64
nip29_past(Nip29Relay *relay)
{
  static gint64 offset;
  (void)relay;
  return g_get_real_time() / G_USEC_PER_SEC - 3600 + ++offset;
}

/* A relay-signed event of kind with tags (taken). */
static G_GNUC_UNUSED NostrEvent *
nip29_relay_sign(Nip29Relay *relay, gint kind, NostrTags *tags, const gchar *content)
{
  NostrEvent *event = nostr_event_new();
  nostr_event_set_kind(event, kind);
  nostr_event_set_created_at(event, nip29_now(relay));
  nostr_event_set_content(event, content ? content : "");
  nostr_event_set_tags(event, tags);
  g_assert_cmpint(nostr_event_sign(event, relay->sk), ==, 0);
  return event;
}

static G_GNUC_UNUSED void
nip29_publish_state(Nip29Relay *relay, Nip29TestGroup *group)
{
  NostrTags *meta = nostr_tags_new(0);
  nostr_tags_append(meta, nostr_tag_new("d", group->id, NULL));
  if (group->name)
    nostr_tags_append(meta, nostr_tag_new("name", group->name, NULL));
  if (group->about)
    nostr_tags_append(meta, nostr_tag_new("about", group->about, NULL));
  if (group->private_)
    nostr_tags_append(meta, nostr_tag_new("private", NULL));
  if (group->restricted)
    nostr_tags_append(meta, nostr_tag_new("restricted", NULL));
  if (group->closed)
    nostr_tags_append(meta, nostr_tag_new("closed", NULL));
  for (guint i = 0; i < group->extra_tags->len; i++) {
    const gchar *const *extra = g_ptr_array_index(group->extra_tags, i);
    NostrTag *tag = nostr_tag_new(extra[0], NULL);
    for (guint j = 1; extra[j]; j++)
      nostr_tag_append(tag, extra[j]);
    nostr_tags_append(meta, tag);
  }
  nip29_store(relay, nip29_relay_sign(relay, 39000, meta, NULL));

  NostrTags *admins = nostr_tags_new(0);
  nostr_tags_append(admins, nostr_tag_new("d", group->id, NULL));
  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, group->admins);
  while (g_hash_table_iter_next(&iter, &key, &value))
    nostr_tags_append(admins, nostr_tag_new("p", key, value, NULL));
  nip29_store(relay, nip29_relay_sign(relay, 39001, admins, NULL));

  NostrTags *members = nostr_tags_new(0);
  nostr_tags_append(members, nostr_tag_new("d", group->id, NULL));
  g_hash_table_iter_init(&iter, group->members);
  while (g_hash_table_iter_next(&iter, &key, NULL))
    if (!group->partial_members || g_hash_table_contains(group->admins, key))
      nostr_tags_append(members, nostr_tag_new("p", key, NULL));
  nip29_store(relay, nip29_relay_sign(relay, 39002, members, NULL));

  NostrTags *roles = nostr_tags_new(0);
  nostr_tags_append(roles, nostr_tag_new("d", group->id, NULL));
  nostr_tags_append(roles, nostr_tag_new("role", "admin", "everything", NULL));
  nostr_tags_append(roles, nostr_tag_new("role", "moderator", "deletes messages", NULL));
  nip29_store(relay, nip29_relay_sign(relay, 39003, roles, NULL));
}

static G_GNUC_UNUSED Nip29TestGroup *
nip29_add_group(Nip29Relay *relay, const gchar *id, const gchar *name)
{
  Nip29TestGroup *group = g_new0(Nip29TestGroup, 1);
  group->id = g_strdup(id);
  group->name = g_strdup(name);
  group->members = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  group->admins = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  group->pending = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  group->invites = g_ptr_array_new_with_free_func(g_free);
  group->extra_tags = g_ptr_array_new_with_free_func((GDestroyNotify)g_strfreev);
  g_hash_table_replace(relay->groups, group->id, group);
  nip29_publish_state(relay, group);
  return group;
}

static G_GNUC_UNUSED void
nip29_set_member(Nip29Relay *relay, Nip29TestGroup *group, const gchar *pubkey, const gchar *role)
{
  g_hash_table_add(group->members, g_strdup(pubkey));
  g_hash_table_remove(group->pending, pubkey);
  if (role)
    g_hash_table_replace(group->admins, g_strdup(pubkey), g_strdup(role));
  nip29_publish_state(relay, group);
}

/* A member signs something into the group (an event from someone else). */
static G_GNUC_UNUSED NostrEvent *
nip29_member_event(const gchar *sk, gint kind, gint64 created_at, const gchar *group_id,
                   const gchar *content)
{
  NostrEvent *event = nostr_event_new();
  nostr_event_set_kind(event, kind);
  nostr_event_set_created_at(event, created_at);
  nostr_event_set_content(event, content);
  NostrTags *tags = nostr_tags_new(0);
  nostr_tags_append(tags, nostr_tag_new("h", group_id, NULL));
  nostr_event_set_tags(event, tags);
  g_assert_cmpint(nostr_event_sign(event, sk), ==, 0);
  return event;
}

/* An admin's decision on a pending join: a relay-signed 9000 and 39002. */
static G_GNUC_UNUSED void
nip29_admit(Nip29Relay *relay, Nip29TestGroup *group, const gchar *pubkey)
{
  g_hash_table_add(group->members, g_strdup(pubkey));
  g_hash_table_remove(group->pending, pubkey);
  NostrTags *tags = nostr_tags_new(0);
  nostr_tags_append(tags, nostr_tag_new("h", group->id, NULL));
  nostr_tags_append(tags, nostr_tag_new("p", pubkey, NULL));
  nip29_store(relay, nip29_relay_sign(relay, 9000, tags, NULL));
  nip29_publish_state(relay, group);
}

static G_GNUC_UNUSED void
nip29_remove(Nip29Relay *relay, Nip29TestGroup *group, const gchar *pubkey, const gchar *why)
{
  g_hash_table_remove(group->members, pubkey);
  g_hash_table_remove(group->admins, pubkey);
  NostrTags *tags = nostr_tags_new(0);
  nostr_tags_append(tags, nostr_tag_new("h", group->id, NULL));
  nostr_tags_append(tags, nostr_tag_new("p", pubkey, NULL));
  nip29_store(relay, nip29_relay_sign(relay, 9001, tags, why));
  nip29_publish_state(relay, group);
}

/* ---- wire ---------------------------------------------------------------- */

static G_GNUC_UNUSED void
nip29_ok(Nip29Relay *relay, SoupWebsocketConnection *connection, NostrEvent *event,
         gboolean accepted, const gchar *message)
{
  g_ptr_array_add(relay->ok_messages, g_strdup_printf("%d %s %s", nostr_event_get_kind(event),
                                                      accepted ? "true" : "false", message));
  g_autofree gchar *frame = g_strdup_printf("[\"OK\",\"%s\",%s,\"%s\"]",
                                            nip29_event_id(event),
                                            accepted ? "true" : "false", message);
  nip29_send(connection, frame);
}

static G_GNUC_UNUSED gboolean
nip29_authed(SoupWebsocketConnection *connection)
{
  return g_object_get_data(G_OBJECT(connection), "authed") != NULL;
}

static G_GNUC_UNUSED gboolean
nip29_previous_known(Nip29Relay *relay, NostrEvent *event)
{
  NostrTag *previous = nip29_tag_find(event, "previous");
  for (size_t i = 1; previous && i < nostr_tag_size(previous); i++) {
    const gchar *ref = nostr_tag_get(previous, i);
    gboolean found = FALSE;
    for (guint j = 0; j < relay->events->len && !found; j++)
      found = g_str_has_prefix(nip29_event_id(g_ptr_array_index(relay->events, j)), ref);
    if (!found || strlen(ref) != 8)
      return FALSE;
  }
  return TRUE;
}

/* role may perform kind? */
static G_GNUC_UNUSED gboolean
nip29_role_allows(const gchar *role, gint kind)
{
  if (g_strcmp0(role, "admin") == 0)
    return TRUE;
  return g_strcmp0(role, "moderator") == 0 && kind == 9005;
}

static G_GNUC_UNUSED void
nip29_on_event(Nip29Relay *relay, SoupWebsocketConnection *connection, NostrEvent *event)
{
  gint kind = nostr_event_get_kind(event);
  const gchar *author = nostr_event_get_pubkey(event);
  Nip29TestGroup *group = g_hash_table_lookup(relay->groups, nip29_tag_value(event, "h")
                                                               ? nip29_tag_value(event, "h") : "");
  if (nip29_stored(relay, nip29_event_id(event))) {
    nip29_ok(relay, connection, event, TRUE, "duplicate: already have this event");
    nostr_event_free(event);
    return;
  }
  if (kind == 9007) {
    const gchar *id = nip29_tag_value(event, "h");
    if (!relay->allow_create) {
      nip29_ok(relay, connection, event, FALSE, "blocked: group creation is not allowed here");
    } else if (group) {
      nip29_ok(relay, connection, event, FALSE, "invalid: group already exists");
    } else {
      nip29_store(relay, event);
      nip29_ok(relay, connection, event, TRUE, "");
      Nip29TestGroup *created = nip29_add_group(relay, id, NULL);
      nip29_set_member(relay, created, author, "admin");
      return;
    }
    nostr_event_free(event);
    return;
  }
  if (!group) {
    nip29_ok(relay, connection, event, FALSE, "invalid: no such group");
    nostr_event_free(event);
    return;
  }
  gboolean member = g_hash_table_contains(group->members, author);
  if (!nip29_previous_known(relay, event)) {
    nip29_ok(relay, connection, event, FALSE, "invalid: unknown previous reference");
    nostr_event_free(event);
    return;
  }
  if (kind == 9021) {
    const gchar *code = nip29_tag_value(event, "code");
    gboolean invited = FALSE;
    for (guint i = 0; code && i < group->invites->len; i++)
      invited = g_str_equal(g_ptr_array_index(group->invites, i), code);
    if (member) {
      nip29_ok(relay, connection, event, FALSE, "duplicate: already a member");
    } else if (invited || (!group->closed && group->join_policy == NIP29_JOIN_AUTO)) {
      nip29_store(relay, event);
      nip29_ok(relay, connection, event, TRUE, "");
      nip29_admit(relay, group, author);
      return;
    } else if (group->closed) {
      nip29_ok(relay, connection, event, FALSE, "restricted: this group is closed");
    } else if (group->join_policy == NIP29_JOIN_PENDING) {
      g_hash_table_add(group->pending, g_strdup(author));
      nip29_ok(relay, connection, event, FALSE,
               "restricted: your join request is pending approval");
    } else {
      nip29_ok(relay, connection, event, FALSE, "blocked: you may not join this group");
    }
    nostr_event_free(event);
    return;
  }
  if (kind == 9022) {
    if (!member) {
      nip29_ok(relay, connection, event, FALSE, "invalid: not a member");
      nostr_event_free(event);
      return;
    }
    nip29_store(relay, event);
    nip29_ok(relay, connection, event, TRUE, "");
    nip29_remove(relay, group, author, "left");
    return;
  }
  if (kind >= 9000 && kind <= 9020) {
    const gchar *role = g_hash_table_lookup(group->admins, author);
    if (!nip29_role_allows(role, kind)) {
      nip29_ok(relay, connection, event, FALSE, "restricted: you are not allowed to do that");
      nostr_event_free(event);
      return;
    }
    const gchar *p = nip29_tag_value(event, "p");
    if (kind == 9000 && p) {
      g_hash_table_add(group->members, g_strdup(p));
      NostrTag *tag = nip29_tag_find(event, "p");
      if (nostr_tag_size(tag) >= 3)
        g_hash_table_replace(group->admins, g_strdup(p), g_strdup(nostr_tag_get(tag, 2)));
    } else if (kind == 9001 && p) {
      g_hash_table_remove(group->members, p);
      g_hash_table_remove(group->admins, p);
    } else if (kind == 9002) {
      g_free(group->name);
      group->name = g_strdup(nip29_tag_value(event, "name"));
      g_free(group->about);
      group->about = g_strdup(nip29_tag_value(event, "about"));
      group->closed = nip29_tag_find(event, "closed") != NULL;
      group->private_ = nip29_tag_find(event, "private") != NULL;
      g_ptr_array_set_size(group->extra_tags, 0);
      NostrTags *tags = nostr_event_get_tags(event);
      static const gchar *const known[] = { "h", "previous", "name", "picture", "banner",
                                            "about", "private", "restricted", "hidden",
                                            "closed", "livekit", "supported_kinds", "parent",
                                            "child", NULL };
      for (size_t i = 0; i < nostr_tags_size(tags); i++) {
        NostrTag *tag = nostr_tags_get(tags, i);
        if (g_strv_contains(known, nostr_tag_get(tag, 0)))
          continue;
        GStrv copy = g_new0(gchar *, nostr_tag_size(tag) + 1);
        for (size_t j = 0; j < nostr_tag_size(tag); j++)
          copy[j] = g_strdup(nostr_tag_get(tag, j));
        g_ptr_array_add(group->extra_tags, copy);
      }
    } else if (kind == 9005) {
      const gchar *e = nip29_tag_value(event, "e");
      for (guint i = relay->events->len; e && i > 0; i--)
        if (g_strcmp0(nip29_event_id(g_ptr_array_index(relay->events, i - 1)), e) == 0)
          g_ptr_array_remove_index(relay->events, i - 1);
    } else if (kind == 9009) {
      g_ptr_array_add(group->invites, g_strdup(nip29_tag_value(event, "code")));
    }
    nip29_store(relay, event);
    nip29_ok(relay, connection, event, TRUE, "");
    nip29_publish_state(relay, group);
    return;
  }
  if (kind >= 9 && kind <= 12) {
    if (!member) {
      nip29_ok(relay, connection, event, FALSE, "restricted: only members can write");
      nostr_event_free(event);
      return;
    }
    nip29_store(relay, event);
    nip29_ok(relay, connection, event, TRUE, "");
    return;
  }
  nip29_ok(relay, connection, event, FALSE, "blocked: kind not supported");
  nostr_event_free(event);
}

static G_GNUC_UNUSED void
nip29_on_req(Nip29Relay *relay, SoupWebsocketConnection *connection, const gchar *text)
{
  relay->reqs++;
  g_ptr_array_add(relay->req_frames, g_strdup(text));
  NostrEnvelope *envelope = nostr_envelope_parse(text);
  g_assert_nonnull(envelope);
  g_assert_cmpint(nostr_envelope_get_type(envelope), ==, NOSTR_ENVELOPE_REQ);
  NostrReqEnvelope *req = (NostrReqEnvelope *)envelope;
  const gchar *sub_id = nostr_req_envelope_get_subscription_id(req);
  if (relay->require_auth && !nip29_authed(connection)) {
    relay->closed_reqs++;
    g_autofree gchar *frame = g_strdup_printf(
      "[\"CLOSED\",\"%s\",\"auth-required: this group is private\"]", sub_id);
    nip29_send(connection, frame);
    nostr_envelope_free(envelope);
    return;
  }
  for (guint i = relay->subs->len; i > 0; i--) {
    Nip29Sub *old = g_ptr_array_index(relay->subs, i - 1);
    if (old->connection == connection && g_str_equal(old->sub_id, sub_id))
      g_ptr_array_remove_index(relay->subs, i - 1);
  }
  Nip29Sub *sub = g_new0(Nip29Sub, 1);
  sub->connection = connection;
  sub->sub_id = g_strdup(sub_id);
  sub->filters = req->filters;
  req->filters = NULL;
  g_ptr_array_add(relay->subs, sub);
  /* Stored matches, each filter's limit taken from the newest. */
  g_autoptr(GHashTable) sent = g_hash_table_new(g_str_hash, g_str_equal);
  for (size_t f = 0; f < sub->filters->count; f++) {
    NostrFilter *filter = &sub->filters->filters[f];
    int limit = nostr_filter_get_limit(filter);
    int taken = 0;
    for (guint i = relay->events->len; i > 0; i--) {
      NostrEvent *event = g_ptr_array_index(relay->events, i - 1);
      if (!nostr_filter_matches(filter, event))
        continue;
      if (limit > 0 && taken >= limit)
        break;
      taken++;
      if (!g_hash_table_add(sent, (gpointer)nip29_event_id(event)))
        continue;
      g_autofree gchar *json = nip29_event_json(event);
      g_autofree gchar *frame = g_strdup_printf("[\"EVENT\",\"%s\",%s]", sub_id, json);
      nip29_send(connection, frame);
    }
  }
  if (!relay->hold_eose) {
    g_autofree gchar *eose = g_strdup_printf("[\"EOSE\",\"%s\"]", sub_id);
    nip29_send(connection, eose);
  }
  nostr_envelope_free(envelope);
}

static G_GNUC_UNUSED gchar *
nip29_frame_json(const gchar *text, const gchar *prefix)
{
  gsize length = strlen(text);
  gsize skip = strlen(prefix);
  if (!g_str_has_prefix(text, prefix) || length <= skip || text[length - 1] != ']')
    return NULL;
  return g_strndup(text + skip, length - skip - 1);
}

static G_GNUC_UNUSED void
nip29_on_message(SoupWebsocketConnection *connection, SoupWebsocketDataType type, GBytes *message,
                 gpointer data)
{
  Nip29Relay *relay = data;
  if (type != SOUP_WEBSOCKET_DATA_TEXT)
    return;
  gsize length;
  const gchar *bytes = g_bytes_get_data(message, &length);
  g_autofree gchar *text = g_strndup(bytes, length);
  if (g_str_has_prefix(text, "[\"REQ\"")) {
    nip29_on_req(relay, connection, text);
  } else if (g_str_has_prefix(text, "[\"CLOSE\"")) {
    for (guint i = relay->subs->len; i > 0; i--) {
      Nip29Sub *sub = g_ptr_array_index(relay->subs, i - 1);
      if (sub->connection == connection && strstr(text, sub->sub_id))
        g_ptr_array_remove_index(relay->subs, i - 1);
    }
  } else if (g_str_has_prefix(text, "[\"EVENT\"")) {
    g_autofree gchar *json = nip29_frame_json(text, "[\"EVENT\",");
    NostrEvent *event = nostr_event_new();
    gchar id[65];
    g_assert_nonnull(json);
    g_assert_cmpint(nostr_event_deserialize_signed(event, json, NULL), ==,
                    NOSTR_EVENT_VALIDATION_OK);
    g_assert_cmpint(nostr_event_validate(event, id), ==, NOSTR_EVENT_VALIDATION_OK);
    relay->events_seen++;
    g_ptr_array_add(relay->received, g_strdup(json));
    if (relay->require_auth && !nip29_authed(connection)) {
      nip29_ok(relay, connection, event, FALSE, "auth-required: sign in to write");
      nostr_event_free(event);
      return;
    }
    nip29_on_event(relay, connection, event);
  } else if (g_str_has_prefix(text, "[\"AUTH\"")) {
    g_autofree gchar *json = nip29_frame_json(text, "[\"AUTH\",");
    NostrEvent *event = nostr_event_new();
    gchar id[65] = { 0 };
    const gchar *challenge = g_object_get_data(G_OBJECT(connection), "challenge");
    gboolean valid = json &&
      nostr_event_deserialize_signed(event, json, NULL) == NOSTR_EVENT_VALIDATION_OK &&
      nostr_event_validate(event, id) == NOSTR_EVENT_VALIDATION_OK &&
      nostr_event_get_kind(event) == 22242 &&
      g_strcmp0(nip29_tag_value(event, "challenge"), challenge) == 0 &&
      g_strcmp0(nip29_tag_value(event, "relay"), relay->url) == 0;
    g_assert_true(valid); /* Groundhog never sends an unverified AUTH */
    g_ptr_array_add(relay->auth_pubkeys, g_strdup(nostr_event_get_pubkey(event)));
    g_autofree gchar *frame = NULL;
    if (relay->refuse_auth) {
      frame = g_strdup_printf("[\"OK\",\"%s\",false,\"restricted: not today\"]", id);
    } else {
      relay->auth_ok++;
      g_object_set_data(G_OBJECT(connection), "authed", GINT_TO_POINTER(1));
      frame = g_strdup_printf("[\"OK\",\"%s\",true,\"\"]", id);
    }
    nip29_send(connection, frame);
    nostr_event_free(event);
  }
}

static G_GNUC_UNUSED void
nip29_on_closed(SoupWebsocketConnection *connection, gpointer data)
{
  Nip29Relay *relay = data;
  for (guint i = relay->subs->len; i > 0; i--) {
    Nip29Sub *sub = g_ptr_array_index(relay->subs, i - 1);
    if (sub->connection == connection)
      g_ptr_array_remove_index(relay->subs, i - 1);
  }
}

static G_GNUC_UNUSED void
nip29_on_websocket(SoupServer *server, SoupServerMessage *message, const char *path,
                   SoupWebsocketConnection *connection, gpointer data)
{
  (void)server;
  (void)message;
  (void)path;
  Nip29Relay *relay = data;
  g_ptr_array_add(relay->connections, g_object_ref(connection));
  g_signal_connect(connection, "message", G_CALLBACK(nip29_on_message), relay);
  g_signal_connect(connection, "closed", G_CALLBACK(nip29_on_closed), relay);
  if (relay->require_auth) {
    static guint counter;
    gchar *challenge = g_strdup_printf("nip29-challenge-%u", ++counter);
    g_object_set_data_full(G_OBJECT(connection), "challenge", challenge, g_free);
    g_autofree gchar *frame = g_strdup_printf("[\"AUTH\",\"%s\"]", challenge);
    nip29_send(connection, frame);
  }
}

/* NIP-11: answered before the WebSocket handler sees a plain GET. */
static G_GNUC_UNUSED void
nip29_on_early(SoupServer *server, SoupServerMessage *message, const char *path,
               GHashTable *query, gpointer data)
{
  (void)server;
  (void)path;
  (void)query;
  Nip29Relay *relay = data;
  SoupMessageHeaders *headers = soup_server_message_get_request_headers(message);
  if (soup_message_headers_get_one(headers, "Upgrade"))
    return;
  /* libnostr's relay transport probes NIP-11 too (with its user agent);
   * Groundhog's own fetch sends no user agent, cookie or referrer. */
  if (!soup_message_headers_get_one(headers, "User-Agent")) {
    relay->nip11_gets++;
    g_assert_null(soup_message_headers_get_one(headers, "Cookie"));
    g_assert_null(soup_message_headers_get_one(headers, "Referer"));
    g_assert_cmpstr(soup_message_headers_get_one(headers, "Accept"), ==, "application/nostr+json");
  }
  if (relay->nip11_redirect) {
    soup_server_message_set_redirect(message, SOUP_STATUS_FOUND, "http://127.0.0.2:9/relay");
    return;
  }
  if (relay->nip11_huge) {
    g_autofree gchar *filler = g_strnfill(70000, 'x');
    g_autofree gchar *huge = g_strdup_printf("{\"self\":\"%s\",\"description\":\"%s\"}",
                                             relay->pk, filler);
    soup_server_message_set_status(message, SOUP_STATUS_OK, NULL);
    soup_server_message_set_response(message, "application/nostr+json", SOUP_MEMORY_COPY, huge,
                                     strlen(huge));
    return;
  }
  g_autofree gchar *body = relay->nip11_no_key
    ? g_strdup("{\"name\":\"nip29 test relay\",\"supported_nips\":[1,11,29,42]}")
    : relay->nip11_pubkey_only
    ? g_strdup_printf("{\"name\":\"nip29 test relay\",\"pubkey\":\"%s\","
                      "\"supported_nips\":[1,11,29,42]}", relay->nip11_pubkey_only_key)
    : g_strdup_printf("{\"name\":\"nip29 test relay\",\"self\":\"%s\",\"pubkey\":\"%s\","
                      "\"supported_nips\":[1,11,29,42]}", relay->pk, relay->pk);
  soup_server_message_set_status(message, SOUP_STATUS_OK, NULL);
  soup_server_message_set_response(message, "application/nostr+json", SOUP_MEMORY_COPY, body,
                                   strlen(body));
}

/* The local server may accept a connection the code under test has already
 * reset (it closes a publish connection once answered). libsoup then cannot
 * read the peer address (macOS getpeername() says EINVAL) and warns from
 * soup-server-connection.c. That is the harness's accept race, not the code
 * under test, so exactly that warning is not fatal. GTest clears the fatal
 * handler before each case: nip29_relay_init() arms it for the case. */
static gboolean
nip29_is_accept_race(const gchar *domain, const gchar *message)
{
  return g_strcmp0(domain, "libsoup") == 0 && message &&
         strstr(message, "could not get remote address") != NULL;
}

static GLogFunc nip29_previous_default_handler;

/* A forgiven warning goes out as an ordinary one, not "Bail out!". */
static void
nip29_log_forgiven_as_warning(const gchar *domain, GLogLevelFlags level, const gchar *message,
                              gpointer data)
{
  if (nip29_is_accept_race(domain, message))
    level &= ~G_LOG_FLAG_FATAL;
  nip29_previous_default_handler(domain, level, message, data);
}

static gboolean
nip29_fatal_unless_accept_race(const gchar *domain, GLogLevelFlags level, const gchar *message,
                               gpointer data)
{
  (void)level;
  (void)data;
  return !nip29_is_accept_race(domain, message);
}

static void
nip29_tolerate_accept_race(void)
{
  static gsize installed;
  if (g_once_init_enter(&installed)) {
    nip29_previous_default_handler =
      g_log_set_default_handler(nip29_log_forgiven_as_warning, NULL);
    g_once_init_leave(&installed, 1);
  }
  g_test_log_set_fatal_handler(nip29_fatal_unless_accept_race, NULL);
}

static G_GNUC_UNUSED void
nip29_relay_init(Nip29Relay *relay)
{
  memset(relay, 0, sizeof *relay);
  char *sk = nostr_key_generate_private();
  char *pk = nostr_key_get_public(sk);
  relay->sk = g_strdup(sk);
  relay->pk = g_strdup(pk);
  free(sk);
  free(pk);
  relay->clock = g_get_real_time() / G_USEC_PER_SEC;
  relay->connections = g_ptr_array_new_with_free_func(g_object_unref);
  relay->events = g_ptr_array_new_with_free_func((GDestroyNotify)nostr_event_free);
  relay->groups = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, nip29_group_free);
  relay->subs = g_ptr_array_new_with_free_func(nip29_sub_free);
  relay->received = g_ptr_array_new_with_free_func(g_free);
  relay->ok_messages = g_ptr_array_new_with_free_func(g_free);
  relay->auth_pubkeys = g_ptr_array_new_with_free_func(g_free);
  relay->req_frames = g_ptr_array_new_with_free_func(g_free);
  nip29_tolerate_accept_race();
  relay->server = soup_server_new(NULL, NULL);
  soup_server_add_early_handler(relay->server, "/relay", nip29_on_early, relay, NULL);
  soup_server_add_websocket_handler(relay->server, "/relay", NULL, NULL, nip29_on_websocket,
                                    relay, NULL);
  g_autoptr(GError) error = NULL;
  g_assert_true(soup_server_listen_local(relay->server, 0, SOUP_SERVER_LISTEN_IPV4_ONLY, &error));
  GSList *uris = soup_server_get_uris(relay->server);
  relay->url = g_strdup_printf("ws://127.0.0.1:%d/relay", g_uri_get_port(uris->data));
  g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
}

static G_GNUC_UNUSED void
nip29_relay_clear(Nip29Relay *relay)
{
  for (guint i = 0; i < relay->connections->len; i++) {
    SoupWebsocketConnection *connection = g_ptr_array_index(relay->connections, i);
    g_signal_handlers_disconnect_by_data(connection, relay);
    if (soup_websocket_connection_get_state(connection) == SOUP_WEBSOCKET_STATE_OPEN)
      soup_websocket_connection_close(connection, SOUP_WEBSOCKET_CLOSE_NORMAL, NULL);
  }
  g_ptr_array_unref(relay->subs);
  g_ptr_array_unref(relay->connections);
  g_ptr_array_unref(relay->events);
  g_hash_table_unref(relay->groups);
  g_ptr_array_unref(relay->received);
  g_ptr_array_unref(relay->ok_messages);
  g_ptr_array_unref(relay->auth_pubkeys);
  g_ptr_array_unref(relay->req_frames);
  soup_server_disconnect(relay->server);
  g_object_unref(relay->server);
  g_free(relay->url);
  g_free(relay->sk);
  g_free(relay->pk);
}

/* The last EVENT frame received of kind (JSON), or NULL. */
static G_GNUC_UNUSED NostrEvent *
nip29_last_received(Nip29Relay *relay, gint kind)
{
  for (guint i = relay->received->len; i > 0; i--) {
    NostrEvent *event = nostr_event_new();
    g_assert_cmpint(nostr_event_deserialize_signed(event, g_ptr_array_index(relay->received, i - 1),
                                                   NULL), ==, NOSTR_EVENT_VALIDATION_OK);
    if (nostr_event_get_kind(event) == kind)
      return event;
    nostr_event_free(event);
  }
  return NULL;
}

#endif
