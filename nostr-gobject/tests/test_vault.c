/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <nostr-gobject-1.0/gn-vault.h>
#include <vault/nip59.h>
#include <nostr-event.h>
#include <nostr-keys.h>
#include <json.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  GMainContext *context;
  GThread *thread;
  GAsyncResult *result;
} Await;
static void ready(GObject *source, GAsyncResult *result, gpointer data)
{
  Await *a = data;
  g_assert_true(GN_IS_VAULT(source));
  g_assert_true(g_main_context_is_owner(a->context));
  g_assert_true(g_thread_self() == a->thread);
  a->result = g_object_ref(result);
}
static void await_init(Await *a)
{
  memset(a, 0, sizeof(*a));
  a->context = g_main_context_new();
  a->thread = g_thread_self();
  g_main_context_push_thread_default(a->context);
}
static GAsyncResult *await_result(Await *a)
{
  while (!a->result) g_main_context_iteration(a->context, TRUE);
  return a->result;
}
static void await_clear(Await *a)
{
  g_clear_object(&a->result);
  g_main_context_pop_thread_default(a->context);
  g_main_context_unref(a->context);
}
static void await_next(Await *a) { g_clear_object(&a->result); }

typedef struct {
  guint8 sk[2][32], pub[2][32], key[32];
  gchar secret[2][65];
  vault_ring ring;
  porthome_vault_info info;
  porthome_dir_node node;
  porthome_node_entry entry;
  porthome_node_chunk chunk;
  guint8 *sealed;
  size_t sealed_len;
  guint reads, puts;
  gboolean fail_put;
  GThread *main_thread;
} Fixture;
static void fixture_init(Fixture *f)
{
  memset(f, 0, sizeof(*f));
  f->main_thread = g_thread_self();
  for (gsize i = 0; i < 2; ++i) {
    f->sk[i][31] = (guint8)i + 1;
    porthome_hex64(f->sk[i], f->secret[i]);
    char *pub = nostr_key_get_public(f->secret[i]);
    g_assert_nonnull(pub);
    g_assert_cmpint(porthome_from_hex64(pub, f->pub[i]), ==, 0);
    free(pub);
  }
  f->key[0] = 7;
  g_assert_cmpint(vault_ring_init(&f->ring, 1, f->key), ==, 0);
  memcpy(f->info.owner, f->pub[0], 32);
  f->info.vault_id[0] = 0x21;
  f->info.writers = f->pub;
  f->info.writers_len = 2;
  f->info.readers = &f->pub[1];
  f->info.readers_len = 1;
  f->info.grants_rev = 8;
  const guint8 plain[] = {'h', 'i', 0};
  g_assert_cmpint(porthome_encrypt_chunk_v2(f->key, plain, sizeof(plain),
      &f->sealed, &f->sealed_len, f->chunk.sha256), ==, 0);
  f->chunk.chunk_key_id = 1;
  f->chunk.size_sealed = f->sealed_len;
  f->chunk.size_plain = sizeof(plain);
  strcpy(f->entry.name_plain, "file");
  memset(f->entry.name_enc, 'a', 48);
  f->entry.kind = PORTHOME_KIND_FILE;
  f->entry.mode = 0600;
  f->entry.mtime_ns = 1;
  f->entry.size_plain = sizeof(plain);
  f->entry.chunks = &f->chunk;
  f->entry.chunks_len = 1;
  f->node.node_id[0] = 0x31;
  f->node.epoch = 1;
  f->node.vault = &f->info;
  f->node.entries = &f->entry;
  f->node.entries_len = 1;
}
static void fixture_clear(Fixture *f)
{
  vault_ring_dispose(&f->ring);
  free(f->sealed);
}
static vault_grant grant_for(Fixture *f)
{
  vault_grant grant = {.ring = f->ring, .cap = VAULT_CAP_READ, .issued_at = 2};
  memcpy(grant.vault_id, f->info.vault_id, 32);
  memcpy(grant.node_id, f->node.node_id, 32);
  memcpy(grant.owner, f->pub[0], 32);
  grant.root_manifest[0] = 3;
  return grant;
}
typedef struct {
  Fixture *fixture;
  NostrEvent *rumor;
  guint sends, receives;
  gboolean wrong_sender;
  GCancellable *cancel_after_send;
} Carrier;
static int send_grant(void *ctx, const guint8 recipient[32], const NostrEvent *rumor)
{
  Carrier *c = ctx;
  g_assert_true(g_thread_self() != c->fixture->main_thread);
  g_assert_cmpmem(recipient, 32, c->fixture->pub[1], 32);
  ++c->sends;
  vault_grant *grant = NULL;
  int rc = vault_grant_rumor_open(rumor, &grant);
  if (!rc) rc = vault_grant_rumor_new(grant, &c->rumor);
  vault_grant_free(grant);
  if (c->cancel_after_send) g_cancellable_cancel(c->cancel_after_send);
  return rc;
}
static int receive_grant(void *ctx, NostrEvent **rumor, guint8 sender[32])
{
  Carrier *c = ctx;
  g_assert_true(g_thread_self() != c->fixture->main_thread);
  ++c->receives;
  *rumor = c->rumor;
  c->rumor = NULL;
  memcpy(sender, c->fixture->pub[c->wrong_sender ? 1 : 0], 32);
  return VAULT_OK;
}
static void test_share_accept(void)
{
  Fixture f; fixture_init(&f);
  Carrier c = {.fixture = &f};
  vault_grant_carrier carrier = {&c, send_grant, receive_grant};
  vault_grant grant = grant_for(&f);
  g_autoptr(GBytes) recipient = g_bytes_new(f.pub[1], 32);
  g_autoptr(GnVault) vault = gn_vault_new();
  g_autoptr(GError) error = NULL;
  Await a; await_init(&a);
  gn_vault_share_async(vault, &carrier, recipient, &grant, NULL, ready, &a);
  g_assert_true(gn_vault_share_finish(vault, await_result(&a), &error));
  g_assert_no_error(error);
  await_next(&a);
  gn_vault_accept_async(vault, &carrier, NULL, ready, &a);
  g_autoptr(GnVaultGrant) accepted = gn_vault_accept_finish(vault, await_result(&a), &error);
  g_assert_no_error(error);
  g_assert_nonnull(accepted);
  GnVaultGrant *copy = g_boxed_copy(GN_TYPE_VAULT_GRANT, accepted);
  g_clear_pointer(&accepted, gn_vault_grant_unref);
  const vault_grant *native = gn_vault_grant_peek(copy);
  g_assert_cmpmem(native->owner, 32, f.pub[0], 32);
  g_assert_cmpmem(native->ring.entries[0].node_key, 32, f.key, 32);
  g_assert_cmpuint(native->ring.count, ==, 1);
  g_assert_cmpint(native->cap, ==, VAULT_CAP_READ);
  g_boxed_free(GN_TYPE_VAULT_GRANT, copy);
  g_assert_cmpuint(c.sends, ==, 1);
  g_assert_cmpuint(c.receives, ==, 1);
  await_next(&a);
  g_assert_cmpint(vault_grant_rumor_new(&grant, &c.rumor), ==, 0);
  c.wrong_sender = TRUE;
  gn_vault_accept_async(vault, &carrier, NULL, ready, &a);
  accepted = gn_vault_accept_finish(vault, await_result(&a), &error);
  g_assert_null(accepted);
  g_assert_error(error, GN_VAULT_ERROR, -VAULT_ERR_AUTH);
  await_clear(&a);
  fixture_clear(&f);
}
typedef struct {
  GThread *main_thread;
  char *json;
  guint publishes, receives;
} Nip59Wire;
static int publish_gift_wrap(void *ctx, const NostrEvent *event)
{
  Nip59Wire *wire = ctx;
  g_assert_true(g_thread_self() != wire->main_thread);
  g_assert_null(wire->json);
  g_assert_cmpint(event->kind, ==, 1059);
  g_assert_cmpint(nostr_event_validate(event, NULL), ==, NOSTR_EVENT_VALIDATION_OK);
  wire->json = nostr_event_serialize_compact(event);
  g_assert_nonnull(wire->json);
  g_assert_null(strstr(wire->json, "node_key"));
  ++wire->publishes;
  return VAULT_OK; /* In-memory transport acceptance, not a live relay ACK. */
}
static int receive_gift_wrap(void *ctx, NostrEvent **event)
{
  Nip59Wire *wire = ctx;
  g_assert_true(g_thread_self() != wire->main_thread);
  *event = NULL;
  if (!wire->json) return VAULT_ERR_AGAIN;
  *event = nostr_event_new();
  g_assert_nonnull(*event);
  g_assert_cmpint(nostr_event_deserialize_signed(*event, wire->json, NULL),
      ==, NOSTR_EVENT_VALIDATION_OK);
  free(wire->json);
  wire->json = NULL;
  ++wire->receives;
  return VAULT_OK;
}
static void test_nip59_share_accept(void)
{
  Fixture f; fixture_init(&f);
  Nip59Wire wire = {.main_thread = g_thread_self()};
  vault_nip59_transport transport = {&wire, publish_gift_wrap, receive_gift_wrap};
  vault_nip59_carrier *sender = NULL, *receiver = NULL;
  g_assert_cmpint(vault_nip59_carrier_new(f.sk[0], &transport, &sender), ==, VAULT_OK);
  g_assert_cmpint(vault_nip59_carrier_new(f.sk[1], &transport, &receiver), ==, VAULT_OK);
  vault_grant grant = grant_for(&f);
  char *relays[] = {"wss://nos.lol"}, *blossom[] = {"https://vault.example"};
  grant.relays = relays; grant.relays_len = 1;
  grant.blossom = blossom; grant.blossom_len = 1;
  g_autoptr(GnVault) vault = gn_vault_new();
  g_autoptr(GBytes) recipient = g_bytes_new(f.pub[1], 32);
  g_autoptr(GError) error = NULL;
  Await a; await_init(&a);
  gn_vault_share_async(vault, vault_nip59_carrier_interface(sender),
      recipient, &grant, NULL, ready, &a);
  g_assert_true(gn_vault_share_finish(vault, await_result(&a), &error));
  g_assert_no_error(error);
  g_assert_cmpuint(wire.publishes, ==, 1);
  g_assert_nonnull(wire.json);
  await_next(&a);
  gn_vault_accept_async(vault, vault_nip59_carrier_interface(receiver), NULL, ready, &a);
  g_autoptr(GnVaultGrant) accepted = gn_vault_accept_finish(vault, await_result(&a), &error);
  g_assert_no_error(error);
  g_assert_nonnull(accepted);
  const vault_grant *native = gn_vault_grant_peek(accepted);
  g_assert_cmpmem(native->owner, 32, grant.owner, 32);
  g_assert_cmpmem(native->vault_id, 32, grant.vault_id, 32);
  g_assert_cmpmem(native->node_id, 32, grant.node_id, 32);
  g_assert_cmpmem(native->root_manifest, 32, grant.root_manifest, 32);
  g_assert_cmpuint(native->ring.count, ==, grant.ring.count);
  g_assert_cmpuint(native->ring.entries[0].epoch, ==, grant.ring.entries[0].epoch);
  g_assert_cmpmem(native->ring.entries[0].node_key, 32, f.key, 32);
  g_assert_cmpint(native->cap, ==, grant.cap);
  g_assert_cmpuint(native->issued_at, ==, grant.issued_at);
  g_assert_cmpuint(native->relays_len, ==, 1);
  g_assert_cmpstr(native->relays[0], ==, relays[0]);
  g_assert_cmpuint(native->blossom_len, ==, 1);
  g_assert_cmpstr(native->blossom[0], ==, blossom[0]);
  g_assert_cmpuint(wire.receives, ==, 1);
  g_assert_null(wire.json);
  await_clear(&a);
  vault_nip59_carrier_free(sender);
  vault_nip59_carrier_free(receiver);
  fixture_clear(&f);
}
static void test_cancellation(void)
{
  Fixture f; fixture_init(&f);
  Carrier c = {.fixture = &f};
  vault_grant_carrier carrier = {&c, send_grant, receive_grant};
  vault_grant grant = grant_for(&f);
  g_autoptr(GnVault) vault = gn_vault_new();
  g_autoptr(GBytes) recipient = g_bytes_new(f.pub[1], 32);
  g_autoptr(GCancellable) cancel = g_cancellable_new();
  g_autoptr(GError) error = NULL;
  Await a; await_init(&a);
  g_cancellable_cancel(cancel);
  gn_vault_share_async(vault, &carrier, recipient, &grant, cancel, ready, &a);
  g_assert_false(gn_vault_share_finish(vault, await_result(&a), &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_assert_cmpuint(c.sends, ==, 0);
  g_clear_error(&error);
  await_next(&a);
  g_cancellable_reset(cancel);
  c.cancel_after_send = cancel;
  gn_vault_share_async(vault, &carrier, recipient, &grant, cancel, ready, &a);
  g_assert_true(gn_vault_share_finish(vault, await_result(&a), &error));
  g_assert_no_error(error);
  g_assert_cmpuint(c.sends, ==, 1);
  vault_grant_rumor_free(c.rumor);
  await_next(&a);
  g_autoptr(GBytes) short_id = g_bytes_new(f.pub[1], 31);
  gn_vault_share_async(vault, &carrier, short_id, &grant, NULL, ready, &a);
  g_assert_false(gn_vault_share_finish(vault, await_result(&a), &error));
  g_assert_error(error, GN_VAULT_ERROR, -VAULT_ERR_ARG);
  g_assert_cmpuint(c.sends, ==, 1);
  await_clear(&a);
  fixture_clear(&f);
}
static vault_record *record_for(Fixture *f, gsize author, gboolean changed,
    porthome_head_parent *parent)
{
  porthome_node_entry entry = f->entry;
  entry.mode = changed ? 0400 : 0600;
  porthome_dir_node node = f->node;
  node.entries = &entry;
  guint8 *sealed = NULL;
  size_t len = 0;
  porthome_head h = {.epoch = 1, .seq = 1, .grants_rev = 8, .written_at = 2,
      .parents = parent, .parents_len = parent ? 1 : 0};
  if (parent) h.seq = parent->seq + 1;
  memcpy(h.vault_id, f->info.vault_id, 32);
  g_assert_cmpint(porthome_dir_node_seal(&node, f->key, &sealed, &len, h.root_manifest), ==, 0);
  NostrEvent *event = NULL;
  g_assert_cmpint(porthome_head_event_new(&h, f->key, f->pub[author], &event), ==, 0);
  g_assert_cmpint(nostr_event_sign(event, f->secret[author]), ==, 0);
  porthome_ring_t ring = vault_ring_view(&f->ring);
  vault_manifest_object object = {sealed, len, ring, 1};
  vault_record *record = NULL;
  g_assert_cmpint(vault_record_open(event, f->info.vault_id, &ring, 1, &object, 1, &record), ==, 0);
  nostr_event_free(event);
  free(sealed);
  return record;
}
static void compare_bytes(GVariant *value, const guint8 expected[32])
{
  gsize len = 0;
  const guint8 *bytes = g_variant_get_fixed_array(value, &len, 1);
  g_assert_cmpmem(bytes, len, expected, 32);
}
static void compare_fold(GVariant *mapped, const vault_fold_result *native)
{
  guint64 view;
  g_assert_true(g_variant_lookup(mapped, "view", "t", &view));
  g_assert_cmpuint(view, ==, native->view == SIZE_MAX ? G_MAXUINT64 : native->view);
  g_autoptr(GVariant) frontier = g_variant_lookup_value(mapped, "frontier", G_VARIANT_TYPE("at"));
  g_autoptr(GVariant) dispositions = g_variant_lookup_value(mapped, "dispositions", G_VARIANT_TYPE("au"));
  g_autoptr(GVariant) conflicts = g_variant_lookup_value(mapped, "conflicts", G_VARIANT_TYPE("aa{sv}"));
  g_autoptr(GVariant) roots = g_variant_lookup_value(mapped, "retained-roots", G_VARIANT_TYPE("aay"));
  g_assert_cmpuint(g_variant_n_children(frontier), ==, native->frontier_len);
  g_assert_cmpuint(g_variant_n_children(dispositions), ==, native->records_len);
  g_assert_cmpuint(g_variant_n_children(conflicts), ==, native->conflicts_len);
  g_assert_cmpuint(g_variant_n_children(roots), ==, native->retained_roots_len);
  for (gsize i = 0; i < native->frontier_len; ++i) {
    guint64 index;
    g_variant_get_child(frontier, i, "t", &index);
    g_assert_cmpuint(index, ==, native->frontier[i]);
  }
  for (gsize i = 0; i < native->records_len; ++i) {
    guint32 disposition;
    g_variant_get_child(dispositions, i, "u", &disposition);
    g_assert_cmpuint(disposition, ==, native->dispositions[i]);
  }
  for (gsize i = 0; i < native->retained_roots_len; ++i) {
    g_autoptr(GVariant) root = g_variant_get_child_value(roots, i);
    compare_bytes(root, native->retained_roots[i]);
  }
  for (gsize i = 0; i < native->conflicts_len; ++i) {
    g_autoptr(GVariant) item = g_variant_get_child_value(conflicts, i);
    const gchar *path;
    guint64 other, seq;
    gboolean in_owner, in_other;
    g_assert_true(g_variant_lookup(item, "path-enc", "&s", &path));
    g_assert_cmpstr(path, ==, native->conflicts[i].path_enc);
    g_assert_true(g_variant_lookup(item, "other-record", "t", &other));
    g_assert_cmpuint(other, ==, native->conflicts[i].other_record);
    g_assert_true(g_variant_lookup(item, "seq", "t", &seq));
    g_assert_cmpuint(seq, ==, native->conflicts[i].seq);
    g_assert_true(g_variant_lookup(item, "in-owner", "b", &in_owner));
    g_assert_true(g_variant_lookup(item, "in-other", "b", &in_other));
    g_assert_cmpint(in_owner, ==, native->conflicts[i].in_owner);
    g_assert_cmpint(in_other, ==, native->conflicts[i].in_other);
    g_autoptr(GVariant) writer = g_variant_lookup_value(item, "writer", G_VARIANT_TYPE("ay"));
    g_autoptr(GVariant) root = g_variant_lookup_value(item, "root-manifest", G_VARIANT_TYPE("ay"));
    compare_bytes(writer, native->conflicts[i].writer);
    compare_bytes(root, native->conflicts[i].root_manifest);
  }
}
static void test_fold(void)
{
  Fixture f; fixture_init(&f);
  vault_record *owner = record_for(&f, 0, FALSE, NULL);
  vault_record *writer = record_for(&f, 1, TRUE, NULL);
  const vault_record *records[] = {owner, writer};
  g_autoptr(GnVault) vault = gn_vault_new();
  Await a; await_init(&a);
  for (gsize count = 1; count <= 2; ++count) {
    vault_fold_result *native = NULL;
    int rc = vault_fold(&f.info, records, count, &native);
    g_assert_cmpint(rc, ==, 0);
    if (count == 2) g_assert_cmpuint(native->conflicts_len, >, 0);
    gn_vault_fold_async(vault, &f.info, records, count, NULL, ready, &a);
    g_autoptr(GError) error = NULL;
    g_autoptr(GVariant) mapped = NULL;
    g_assert_true(gn_vault_fold_finish(vault, await_result(&a), &mapped, &error));
    g_assert_no_error(error);
    compare_fold(mapped, native);
    vault_fold_result_free(native);
    await_next(&a);
  }
  /* Writer-only forks cannot fabricate an owner view. */
  vault_record *fork = record_for(&f, 1, FALSE, NULL);
  const vault_record *forks[] = {writer, fork};
  vault_fold_result *native = NULL;
  int rc = vault_fold(&f.info, forks, 2, &native);
  g_assert_cmpint(rc, ==, VAULT_ERR_CONFLICT);
  gn_vault_fold_async(vault, &f.info, forks, 2, NULL, ready, &a);
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) mapped = NULL;
  g_assert_false(gn_vault_fold_finish(vault, await_result(&a), &mapped, &error));
  g_assert_error(error, GN_VAULT_ERROR, -VAULT_ERR_CONFLICT);
  compare_fold(mapped, native);
  vault_fold_result_free(native);
  vault_record_free(fork);
  await_next(&a);
  g_clear_error(&error);
  g_clear_pointer(&mapped, g_variant_unref);
  porthome_head_parent missing = {.seq = 1};
  memcpy(missing.writer, f.pub[0], 32);
  missing.root_manifest[0] = 42;
  vault_record *incomplete = record_for(&f, 0, FALSE, &missing);
  const vault_record *one[] = {incomplete};
  rc = vault_fold(&f.info, one, 1, &native);
  g_assert_cmpint(rc, ==, VAULT_ERR_INCOMPLETE);
  gn_vault_fold_async(vault, &f.info, one, 1, NULL, ready, &a);
  g_assert_false(gn_vault_fold_finish(vault, await_result(&a), &mapped, &error));
  g_assert_error(error, GN_VAULT_ERROR, -VAULT_ERR_INCOMPLETE);
  compare_fold(mapped, native);
  vault_fold_result_free(native);
  vault_record_free(incomplete);
  await_clear(&a);
  vault_record_free(owner);
  vault_record_free(writer);
  fixture_clear(&f);
}
static int read_blob(void *ctx, const guint8 address[32], guint8 **bytes, size_t *len)
{
  Fixture *f = ctx;
  g_assert_true(g_thread_self() != f->main_thread);
  ++f->reads;
  g_assert_cmpmem(address, 32, f->chunk.sha256, 32);
  *bytes = malloc(f->sealed_len);
  g_assert_nonnull(*bytes);
  memcpy(*bytes, f->sealed, f->sealed_len);
  *len = f->sealed_len;
  return 0;
}
static int put_blob(void *ctx, const guint8 *bytes, size_t len, const guint8 address[32])
{
  Fixture *f = ctx;
  g_assert_true(g_thread_self() != f->main_thread);
  ++f->puts;
  guint8 hash[32];
  g_assert_cmpint(porthome_sha256(bytes, len, hash), ==, 0);
  g_assert_cmpmem(hash, 32, address, 32);
  return f->fail_put ? VAULT_ERR_IO : 0;
}
static void test_revoke_reseal(void)
{
  Fixture f; fixture_init(&f);
  porthome_node_entry parent = {.kind = PORTHOME_KIND_DIR, .name_plain = "subtree", .child_epoch = 1};
  memset(parent.name_enc, 'b', 48);
  memcpy(parent.node_id, f.node.node_id, 32);
  guint8 *sealed = NULL;
  size_t len = 0;
  g_assert_cmpint(porthome_dir_node_seal(&f.node, f.key, &sealed, &len, parent.manifest_sha256), ==, 0);
  free(sealed);
  g_autoptr(GBytes) actor = g_bytes_new(f.pub[0], 32);
  g_autoptr(GBytes) member = g_bytes_new(f.pub[1], 32);
  g_autoptr(GnVault) vault = gn_vault_new();
  g_autoptr(GError) error = NULL;
  Await a; await_init(&a);
  gn_vault_revoke_async(vault, member, actor, &f.node, &f.ring, &parent, NULL, 3, NULL, ready, &a);
  g_autoptr(GnVaultRevocation) denied = gn_vault_revoke_finish(vault, await_result(&a), &error);
  g_assert_null(denied);
  g_assert_error(error, GN_VAULT_ERROR, -VAULT_ERR_AUTH);
  g_clear_error(&error);
  await_next(&a);
  gn_vault_revoke_async(vault, actor, member, &f.node, &f.ring, &parent, NULL, 3, NULL, ready, &a);
  g_autoptr(GnVaultRevocation) result = gn_vault_revoke_finish(vault, await_result(&a), &error);
  g_assert_no_error(error);
  g_assert_nonnull(result);
  GnVaultRevocation *copy = g_boxed_copy(GN_TYPE_VAULT_REVOCATION, result);
  g_clear_pointer(&result, gn_vault_revocation_unref);
  const vault_revoke_result *r = gn_vault_revocation_peek(copy);
  g_assert_cmpuint(r->ring.count, ==, 2);
  g_assert_cmpuint(r->node->epoch, ==, 2);
  g_assert_cmpuint(r->node->vault->readers_len, ==, 0);
  g_assert_cmpuint(f.node.epoch, ==, 1);
  g_assert_cmpuint(f.ring.count, ==, 1);
  vault_reseal_item item = {r->node, &r->ring, 0, 0};
  vault_blob_io io = {&f, read_blob, put_blob};
  vault_reseal_task *task = NULL;
  g_assert_cmpint(vault_reseal_task_new(&item, 1, 0, &task), ==, 0);
  await_next(&a);
  gn_vault_reseal_async(vault, task, 0, &io, NULL, ready, &a);
  g_autoptr(GVariant) progress = NULL;
  g_assert_false(gn_vault_reseal_finish(vault, await_result(&a), &progress, &error));
  g_assert_error(error, GN_VAULT_ERROR, -VAULT_ERR_BUDGET);
  guint64 completed, reserved;
  g_assert_true(g_variant_lookup(progress, "completed", "t", &completed));
  g_assert_cmpuint(completed, ==, 0);
  g_assert_cmpuint(f.reads, ==, 0);
  g_assert_cmpuint(f.puts, ==, 0);
  g_clear_error(&error);
  g_clear_pointer(&progress, g_variant_unref);
  await_next(&a);
  gn_vault_reseal_async(vault, task, G_MAXUINT64, &io, NULL, ready, &a);
  g_assert_true(gn_vault_reseal_finish(vault, await_result(&a), &progress, &error));
  g_assert_no_error(error);
  g_assert_true(g_variant_lookup(progress, "completed", "t", &completed));
  g_assert_true(g_variant_lookup(progress, "reserved", "t", &reserved));
  g_assert_cmpuint(completed, ==, 1);
  g_assert_cmpuint(reserved, ==, vault_reseal_task_reserved(task));
  g_assert_cmpuint(r->node->entries[0].chunks[0].chunk_key_id, ==, 2);
  g_assert_cmpuint(f.puts, ==, 1);
  vault_reseal_task_free(task);
  g_boxed_free(GN_TYPE_VAULT_REVOCATION, copy);

  /* Exercise root-head forwarding and retain uncertain-write progress on IO. */
  await_next(&a);
  vault_record *current = record_for(&f, 0, FALSE, NULL);
  gn_vault_revoke_async(vault, actor, member, &f.node, &f.ring, NULL,
      vault_record_head(current), 3, NULL, ready, &a);
  result = gn_vault_revoke_finish(vault, await_result(&a), &error);
  g_assert_no_error(error);
  g_assert_nonnull(result);
  r = gn_vault_revocation_peek(result);
  g_assert_nonnull(r->head);
  g_assert_nonnull(r->head_payload);
  g_assert_false(r->has_parent);
  vault_record_free(current);
  item = (vault_reseal_item){r->node, &r->ring, 0, 0};
  g_assert_cmpint(vault_reseal_task_new(&item, 1, 0, &task), ==, 0);
  f.fail_put = TRUE;
  f.reads = f.puts = 0;
  for (guint i = 0; i < 2; ++i) {
    await_next(&a);
    g_clear_pointer(&progress, g_variant_unref);
    gn_vault_reseal_async(vault, task, G_MAXUINT64, &io, NULL, ready, &a);
    g_assert_false(gn_vault_reseal_finish(vault, await_result(&a), &progress, &error));
    g_assert_error(error, GN_VAULT_ERROR, -VAULT_ERR_IO);
    g_assert_true(g_variant_lookup(progress, "completed", "t", &completed));
    g_assert_true(g_variant_lookup(progress, "reserved", "t", &reserved));
    g_assert_cmpuint(completed, ==, 0);
    g_assert_cmpuint(reserved, ==, vault_reseal_task_reserved(task));
    g_autoptr(GVariant) pending = g_variant_lookup_value(progress, "pending-address", G_VARIANT_TYPE("ay"));
    compare_bytes(pending, vault_reseal_task_pending_address(task));
    g_assert_cmpuint(f.reads, ==, 1);
    g_assert_cmpuint(f.puts, ==, 1);
    g_clear_error(&error);
  }
  vault_reseal_task_free(task);
  await_clear(&a);
  fixture_clear(&f);
}
int main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  nostr_json_init();
  g_test_add_func("/gn-vault/share-accept", test_share_accept);
  g_test_add_func("/gn-vault/nip59-share-accept", test_nip59_share_accept);
  g_test_add_func("/gn-vault/cancellation", test_cancellation);
  g_test_add_func("/gn-vault/fold", test_fold);
  g_test_add_func("/gn-vault/revoke-reseal", test_revoke_reseal);
  int status = g_test_run();
  nostr_json_cleanup();
  return status;
}
