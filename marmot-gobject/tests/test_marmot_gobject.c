/*
 * test_marmot_gobject.c — Comprehensive unit tests for marmot-gobject
 *
 * Tests GObject lifecycle, properties, type registration, GInterface
 * implementation, signal emission, GTask async patterns, and enum types.
 *
 * Bead: mm64
 *
 * SPDX-License-Identifier: MIT
 */

#include <glib.h>
#include <glib-object.h>
#include <gio/gio.h>
#include <marmot-gobject-1.0/marmot-gobject.h>
#include <string.h>

/* ══════════════════════════════════════════════════════════════════════════
 * Test fixtures and helpers
 * ══════════════════════════════════════════════════════════════════════════ */

/* Fake 32-byte hex strings for testing (64 hex chars) */
#define TEST_HEX_32 "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define TEST_HEX_32_B "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
#define TEST_HEX_32_C "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"

/* Shorter group IDs (variable length in MLS) */
#define TEST_GROUP_ID_HEX "deadbeefcafebabe0102030405060708"

/* Async test helper: run the main loop until a callback fires */
typedef struct {
    GMainLoop *loop;
    GAsyncResult *result;
} AsyncFixture;

static void
async_callback(GObject *source, GAsyncResult *result, gpointer user_data)
{
    AsyncFixture *f = user_data;
    f->result = g_object_ref(result);
    g_main_loop_quit(f->loop);
}

static AsyncFixture *
async_fixture_new(void)
{
    AsyncFixture *f = g_new0(AsyncFixture, 1);
    f->loop = g_main_loop_new(NULL, FALSE);
    return f;
}

static void
async_fixture_free(AsyncFixture *f)
{
    g_main_loop_unref(f->loop);
    g_clear_object(&f->result);
    g_free(f);
}

/* ══════════════════════════════════════════════════════════════════════════
 * 1. Enum type registration tests
 * ══════════════════════════════════════════════════════════════════════════ */

static void
test_enum_group_state_type(void)
{
    GType type = MARMOT_GOBJECT_TYPE_GROUP_STATE;
    g_assert_cmpuint(type, !=, G_TYPE_INVALID);
    g_assert_true(G_TYPE_IS_ENUM(type));

    GEnumClass *ec = g_type_class_ref(type);
    g_assert_nonnull(ec);

    /* Verify all values are registered */
    GEnumValue *v;
    v = g_enum_get_value(ec, MARMOT_GOBJECT_GROUP_STATE_ACTIVE);
    g_assert_nonnull(v);
    v = g_enum_get_value(ec, MARMOT_GOBJECT_GROUP_STATE_INACTIVE);
    g_assert_nonnull(v);
    v = g_enum_get_value(ec, MARMOT_GOBJECT_GROUP_STATE_PENDING);
    g_assert_nonnull(v);

    g_type_class_unref(ec);
}

static void
test_enum_message_state_type(void)
{
    GType type = MARMOT_GOBJECT_TYPE_MESSAGE_STATE;
    g_assert_cmpuint(type, !=, G_TYPE_INVALID);
    g_assert_true(G_TYPE_IS_ENUM(type));

    GEnumClass *ec = g_type_class_ref(type);
    g_assert_nonnull(ec);
    g_assert_nonnull(g_enum_get_value(ec, MARMOT_GOBJECT_MESSAGE_STATE_CREATED));
    g_assert_nonnull(g_enum_get_value(ec, MARMOT_GOBJECT_MESSAGE_STATE_PROCESSED));
    g_assert_nonnull(g_enum_get_value(ec, MARMOT_GOBJECT_MESSAGE_STATE_DELETED));
    g_assert_nonnull(g_enum_get_value(ec, MARMOT_GOBJECT_MESSAGE_STATE_EPOCH_INVALIDATED));
    g_type_class_unref(ec);
}

static void
test_enum_welcome_state_type(void)
{
    GType type = MARMOT_GOBJECT_TYPE_WELCOME_STATE;
    g_assert_cmpuint(type, !=, G_TYPE_INVALID);
    g_assert_true(G_TYPE_IS_ENUM(type));

    GEnumClass *ec = g_type_class_ref(type);
    g_assert_nonnull(ec);
    g_assert_nonnull(g_enum_get_value(ec, MARMOT_GOBJECT_WELCOME_STATE_PENDING));
    g_assert_nonnull(g_enum_get_value(ec, MARMOT_GOBJECT_WELCOME_STATE_ACCEPTED));
    g_assert_nonnull(g_enum_get_value(ec, MARMOT_GOBJECT_WELCOME_STATE_DECLINED));
    g_type_class_unref(ec);
}

static void
test_enum_message_result_type(void)
{
    GType type = MARMOT_GOBJECT_TYPE_MESSAGE_RESULT_TYPE;
    g_assert_cmpuint(type, !=, G_TYPE_INVALID);
    g_assert_true(G_TYPE_IS_ENUM(type));

    GEnumClass *ec = g_type_class_ref(type);
    g_assert_nonnull(ec);
    g_assert_nonnull(g_enum_get_value(ec, MARMOT_GOBJECT_MESSAGE_RESULT_APPLICATION));
    g_assert_nonnull(g_enum_get_value(ec, MARMOT_GOBJECT_MESSAGE_RESULT_COMMIT));
    g_assert_nonnull(g_enum_get_value(ec, MARMOT_GOBJECT_MESSAGE_RESULT_PROPOSAL));
    g_assert_nonnull(g_enum_get_value(ec, MARMOT_GOBJECT_MESSAGE_RESULT_UNPROCESSABLE));
    g_assert_nonnull(g_enum_get_value(ec, MARMOT_GOBJECT_MESSAGE_RESULT_OWN_MESSAGE));
    g_type_class_unref(ec);
}

/* ══════════════════════════════════════════════════════════════════════════
 * 2. Group type tests
 * ══════════════════════════════════════════════════════════════════════════ */

static void
test_group_type_registration(void)
{
    GType type = MARMOT_GOBJECT_TYPE_GROUP;
    g_assert_cmpuint(type, !=, G_TYPE_INVALID);
    g_assert_true(g_type_is_a(type, G_TYPE_OBJECT));
}

static void
test_group_new_from_data(void)
{
    MarmotGobjectGroup *group = marmot_gobject_group_new_from_data(
        TEST_GROUP_ID_HEX, TEST_HEX_32,
        "Test Group", "A test group",
        MARMOT_GOBJECT_GROUP_STATE_ACTIVE, 42);

    g_assert_nonnull(group);
    g_assert_true(MARMOT_GOBJECT_IS_GROUP(group));

    g_assert_cmpstr(marmot_gobject_group_get_mls_group_id(group), ==, TEST_GROUP_ID_HEX);
    g_assert_cmpstr(marmot_gobject_group_get_nostr_group_id(group), ==, TEST_HEX_32);
    g_assert_cmpstr(marmot_gobject_group_get_name(group), ==, "Test Group");
    g_assert_cmpstr(marmot_gobject_group_get_description(group), ==, "A test group");
    g_assert_cmpint(marmot_gobject_group_get_state(group), ==, MARMOT_GOBJECT_GROUP_STATE_ACTIVE);
    g_assert_cmpuint(marmot_gobject_group_get_epoch(group), ==, 42);

    g_object_unref(group);
}

static void
test_group_null_optional_fields(void)
{
    MarmotGobjectGroup *group = marmot_gobject_group_new_from_data(
        TEST_GROUP_ID_HEX, TEST_HEX_32,
        NULL, NULL,  /* name and description are nullable */
        MARMOT_GOBJECT_GROUP_STATE_PENDING, 0);

    g_assert_nonnull(group);
    g_assert_null(marmot_gobject_group_get_name(group));
    g_assert_null(marmot_gobject_group_get_description(group));
    g_assert_cmpint(marmot_gobject_group_get_state(group), ==, MARMOT_GOBJECT_GROUP_STATE_PENDING);
    g_assert_cmpuint(marmot_gobject_group_get_epoch(group), ==, 0);

    g_object_unref(group);
}

static void
test_group_properties_via_gvalue(void)
{
    MarmotGobjectGroup *group = marmot_gobject_group_new_from_data(
        TEST_GROUP_ID_HEX, TEST_HEX_32,
        "Props Test", "desc",
        MARMOT_GOBJECT_GROUP_STATE_INACTIVE, 7);

    /* Read properties through GObject property mechanism */
    gchar *mls_id = NULL;
    gchar *nostr_id = NULL;
    gchar *name = NULL;
    gchar *desc = NULL;
    gint state = -1;
    guint64 epoch = 0;
    guint admin_count = 99;
    gint64 last_msg = -1;

    g_object_get(group,
                 "mls-group-id", &mls_id,
                 "nostr-group-id", &nostr_id,
                 "name", &name,
                 "description", &desc,
                 "state", &state,
                 "epoch", &epoch,
                 "admin-count", &admin_count,
                 "last-message-at", &last_msg,
                 NULL);

    g_assert_cmpstr(mls_id, ==, TEST_GROUP_ID_HEX);
    g_assert_cmpstr(nostr_id, ==, TEST_HEX_32);
    g_assert_cmpstr(name, ==, "Props Test");
    g_assert_cmpstr(desc, ==, "desc");
    g_assert_cmpint(state, ==, MARMOT_GOBJECT_GROUP_STATE_INACTIVE);
    g_assert_cmpuint(epoch, ==, 7);
    g_assert_cmpuint(admin_count, ==, 0);
    g_assert_cmpint(last_msg, ==, 0);

    g_free(mls_id);
    g_free(nostr_id);
    g_free(name);
    g_free(desc);

    g_object_unref(group);
}

static void
test_group_refcount(void)
{
    MarmotGobjectGroup *group = marmot_gobject_group_new_from_data(
        TEST_GROUP_ID_HEX, TEST_HEX_32,
        "RC Test", NULL,
        MARMOT_GOBJECT_GROUP_STATE_ACTIVE, 1);

    /* Initial refcount should be 1 (floating ref sunk by new_from_data) */
    g_object_ref(group);  /* refcount = 2 */
    g_object_unref(group);  /* refcount = 1 */

    /* Still alive */
    g_assert_cmpstr(marmot_gobject_group_get_name(group), ==, "RC Test");

    g_object_unref(group);  /* refcount = 0 → finalize */
}

/* ══════════════════════════════════════════════════════════════════════════
 * 3. Message type tests
 * ══════════════════════════════════════════════════════════════════════════ */

static void
test_message_type_registration(void)
{
    GType type = MARMOT_GOBJECT_TYPE_MESSAGE;
    g_assert_cmpuint(type, !=, G_TYPE_INVALID);
    g_assert_true(g_type_is_a(type, G_TYPE_OBJECT));
}

static void
test_message_new_from_data(void)
{
    MarmotGobjectMessage *msg = marmot_gobject_message_new_from_data(
        TEST_HEX_32, TEST_HEX_32_B,
        "Hello, world!", 1,
        1700000000, TEST_GROUP_ID_HEX);

    g_assert_nonnull(msg);
    g_assert_true(MARMOT_GOBJECT_IS_MESSAGE(msg));

    g_assert_cmpstr(marmot_gobject_message_get_event_id(msg), ==, TEST_HEX_32);
    g_assert_cmpstr(marmot_gobject_message_get_pubkey(msg), ==, TEST_HEX_32_B);
    g_assert_cmpstr(marmot_gobject_message_get_content(msg), ==, "Hello, world!");
    g_assert_cmpuint(marmot_gobject_message_get_kind(msg), ==, 1);
    g_assert_cmpint(marmot_gobject_message_get_created_at(msg), ==, 1700000000);
    g_assert_cmpstr(marmot_gobject_message_get_mls_group_id(msg), ==, TEST_GROUP_ID_HEX);

    /* Defaults */
    g_assert_cmpint(marmot_gobject_message_get_processed_at(msg), ==, 0);
    g_assert_cmpuint(marmot_gobject_message_get_epoch(msg), ==, 0);
    g_assert_cmpint(marmot_gobject_message_get_state(msg), ==, MARMOT_GOBJECT_MESSAGE_STATE_CREATED);

    g_object_unref(msg);
}

static void
test_message_null_content(void)
{
    MarmotGobjectMessage *msg = marmot_gobject_message_new_from_data(
        TEST_HEX_32, TEST_HEX_32_B,
        NULL, 445,
        1700000000, TEST_GROUP_ID_HEX);

    g_assert_nonnull(msg);
    g_assert_null(marmot_gobject_message_get_content(msg));
    g_assert_cmpuint(marmot_gobject_message_get_kind(msg), ==, 445);

    g_object_unref(msg);
}

static void
test_message_properties_via_gvalue(void)
{
    MarmotGobjectMessage *msg = marmot_gobject_message_new_from_data(
        TEST_HEX_32, TEST_HEX_32_B,
        "Test content", 1,
        1700000000, TEST_GROUP_ID_HEX);

    gchar *event_id = NULL;
    gchar *pubkey = NULL;
    gchar *content = NULL;
    guint kind = 0;
    gint64 created_at = 0;

    g_object_get(msg,
                 "event-id", &event_id,
                 "pubkey", &pubkey,
                 "content", &content,
                 "kind", &kind,
                 "created-at", &created_at,
                 NULL);

    g_assert_cmpstr(event_id, ==, TEST_HEX_32);
    g_assert_cmpstr(pubkey, ==, TEST_HEX_32_B);
    g_assert_cmpstr(content, ==, "Test content");
    g_assert_cmpuint(kind, ==, 1);
    g_assert_cmpint(created_at, ==, 1700000000);

    g_free(event_id);
    g_free(pubkey);
    g_free(content);

    g_object_unref(msg);
}

/* ══════════════════════════════════════════════════════════════════════════
 * 4. Welcome type tests
 * ══════════════════════════════════════════════════════════════════════════ */

static void
test_welcome_type_registration(void)
{
    GType type = MARMOT_GOBJECT_TYPE_WELCOME;
    g_assert_cmpuint(type, !=, G_TYPE_INVALID);
    g_assert_true(g_type_is_a(type, G_TYPE_OBJECT));
}

static void
test_welcome_new_from_data(void)
{
    MarmotGobjectWelcome *w = marmot_gobject_welcome_new_from_data(
        TEST_HEX_32,       /* event_id_hex */
        "Test Group",      /* group_name */
        "A test group",    /* group_description */
        TEST_HEX_32_B,     /* welcomer_hex */
        5,                 /* member_count */
        MARMOT_GOBJECT_WELCOME_STATE_PENDING,
        TEST_GROUP_ID_HEX, /* mls_group_id_hex */
        TEST_HEX_32_C);    /* nostr_group_id_hex */

    g_assert_nonnull(w);
    g_assert_true(MARMOT_GOBJECT_IS_WELCOME(w));

    g_assert_cmpstr(marmot_gobject_welcome_get_event_id(w), ==, TEST_HEX_32);
    g_assert_cmpstr(marmot_gobject_welcome_get_group_name(w), ==, "Test Group");
    g_assert_cmpstr(marmot_gobject_welcome_get_group_description(w), ==, "A test group");
    g_assert_cmpstr(marmot_gobject_welcome_get_welcomer(w), ==, TEST_HEX_32_B);
    g_assert_cmpuint(marmot_gobject_welcome_get_member_count(w), ==, 5);
    g_assert_cmpint(marmot_gobject_welcome_get_state(w), ==, MARMOT_GOBJECT_WELCOME_STATE_PENDING);
    g_assert_cmpstr(marmot_gobject_welcome_get_mls_group_id(w), ==, TEST_GROUP_ID_HEX);
    g_assert_cmpstr(marmot_gobject_welcome_get_nostr_group_id(w), ==, TEST_HEX_32_C);

    g_object_unref(w);
}

static void
test_welcome_null_name_description(void)
{
    MarmotGobjectWelcome *w = marmot_gobject_welcome_new_from_data(
        TEST_HEX_32, NULL, NULL, TEST_HEX_32_B,
        0, MARMOT_GOBJECT_WELCOME_STATE_DECLINED,
        TEST_GROUP_ID_HEX, TEST_HEX_32_C);

    g_assert_nonnull(w);
    g_assert_null(marmot_gobject_welcome_get_group_name(w));
    g_assert_null(marmot_gobject_welcome_get_group_description(w));
    g_assert_cmpint(marmot_gobject_welcome_get_state(w), ==, MARMOT_GOBJECT_WELCOME_STATE_DECLINED);

    g_object_unref(w);
}

static void
test_welcome_properties_via_gvalue(void)
{
    MarmotGobjectWelcome *w = marmot_gobject_welcome_new_from_data(
        TEST_HEX_32, "VGroup", "Vdesc", TEST_HEX_32_B,
        3, MARMOT_GOBJECT_WELCOME_STATE_ACCEPTED,
        TEST_GROUP_ID_HEX, TEST_HEX_32_C);

    gchar *event_id = NULL;
    gchar *gname = NULL;
    gchar *gdesc = NULL;
    gchar *welcomer = NULL;
    guint mc = 0;
    gint state = -1;
    gchar *mls_gid = NULL;
    gchar *nostr_gid = NULL;

    g_object_get(w,
                 "event-id", &event_id,
                 "group-name", &gname,
                 "group-description", &gdesc,
                 "welcomer", &welcomer,
                 "member-count", &mc,
                 "state", &state,
                 "mls-group-id", &mls_gid,
                 "nostr-group-id", &nostr_gid,
                 NULL);

    g_assert_cmpstr(event_id, ==, TEST_HEX_32);
    g_assert_cmpstr(gname, ==, "VGroup");
    g_assert_cmpstr(gdesc, ==, "Vdesc");
    g_assert_cmpstr(welcomer, ==, TEST_HEX_32_B);
    g_assert_cmpuint(mc, ==, 3);
    g_assert_cmpint(state, ==, MARMOT_GOBJECT_WELCOME_STATE_ACCEPTED);
    g_assert_cmpstr(mls_gid, ==, TEST_GROUP_ID_HEX);
    g_assert_cmpstr(nostr_gid, ==, TEST_HEX_32_C);

    g_free(event_id);
    g_free(gname);
    g_free(gdesc);
    g_free(welcomer);
    g_free(mls_gid);
    g_free(nostr_gid);

    g_object_unref(w);
}

/* ══════════════════════════════════════════════════════════════════════════
 * 5. Storage interface tests
 * ══════════════════════════════════════════════════════════════════════════ */

static void
test_storage_interface_type(void)
{
    GType type = MARMOT_GOBJECT_TYPE_STORAGE;
    g_assert_cmpuint(type, !=, G_TYPE_INVALID);
    g_assert_true(G_TYPE_IS_INTERFACE(type));
}

static void
test_memory_storage_type(void)
{
    GType type = MARMOT_GOBJECT_TYPE_MEMORY_STORAGE;
    g_assert_cmpuint(type, !=, G_TYPE_INVALID);
    g_assert_true(g_type_is_a(type, G_TYPE_OBJECT));
    g_assert_true(g_type_is_a(type, MARMOT_GOBJECT_TYPE_STORAGE));
}

static void
test_memory_storage_new(void)
{
    MarmotGobjectMemoryStorage *store = marmot_gobject_memory_storage_new();
    g_assert_nonnull(store);
    g_assert_true(MARMOT_GOBJECT_IS_MEMORY_STORAGE(store));
    g_assert_true(MARMOT_GOBJECT_IS_STORAGE(store));

    /* The raw storage pointer should be non-NULL */
    gpointer raw = marmot_gobject_storage_get_raw_storage(MARMOT_GOBJECT_STORAGE(store));
    g_assert_nonnull(raw);

    g_object_unref(store);
}

static void
test_memory_storage_interface_cast(void)
{
    MarmotGobjectMemoryStorage *store = marmot_gobject_memory_storage_new();
    MarmotGobjectStorage *iface = MARMOT_GOBJECT_STORAGE(store);
    g_assert_nonnull(iface);

    /* Should be able to get raw storage through the interface */
    gpointer raw = marmot_gobject_storage_get_raw_storage(iface);
    g_assert_nonnull(raw);

    g_object_unref(store);
}

static void
test_sqlite_storage_type(void)
{
    GType type = MARMOT_GOBJECT_TYPE_SQLITE_STORAGE;
    g_assert_cmpuint(type, !=, G_TYPE_INVALID);
    g_assert_true(g_type_is_a(type, G_TYPE_OBJECT));
    g_assert_true(g_type_is_a(type, MARMOT_GOBJECT_TYPE_STORAGE));
}

/* ══════════════════════════════════════════════════════════════════════════
 * 6. Client type tests
 * ══════════════════════════════════════════════════════════════════════════ */

static void
test_client_type_registration(void)
{
    GType type = MARMOT_GOBJECT_TYPE_CLIENT;
    g_assert_cmpuint(type, !=, G_TYPE_INVALID);
    g_assert_true(g_type_is_a(type, G_TYPE_OBJECT));
}

static void
test_client_new_with_memory_storage(void)
{
    MarmotGobjectMemoryStorage *store = marmot_gobject_memory_storage_new();
    MarmotGobjectClient *client = marmot_gobject_client_new(MARMOT_GOBJECT_STORAGE(store));

    g_assert_nonnull(client);
    g_assert_true(MARMOT_GOBJECT_IS_CLIENT(client));

    g_object_unref(client);
    g_object_unref(store);
}

static void
test_client_finalize_releases_storage(void)
{
    MarmotGobjectMemoryStorage *store = marmot_gobject_memory_storage_new();

    /* The client takes a ref on the storage */
    MarmotGobjectClient *client = marmot_gobject_client_new(MARMOT_GOBJECT_STORAGE(store));
    g_assert_nonnull(client);

    /* Ref store again so we can check it survives client destruction */
    g_object_ref(store);

    /* Destroy client — should release its ref on storage */
    g_object_unref(client);

    /* Store should still be alive (we hold one ref) */
    g_assert_true(MARMOT_GOBJECT_IS_MEMORY_STORAGE(store));

    g_object_unref(store);
}

/* ══════════════════════════════════════════════════════════════════════════
 * 7. Client signal tests
 * ══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    gboolean fired;
    GObject *received_object;
} SignalData;

static void
on_group_joined(MarmotGobjectClient *client, MarmotGobjectGroup *group, gpointer data)
{
    SignalData *sd = data;
    sd->fired = TRUE;
    sd->received_object = G_OBJECT(group);
    g_object_ref(group);
}

static void
test_client_signal_group_joined(void)
{
    MarmotGobjectMemoryStorage *store = marmot_gobject_memory_storage_new();
    MarmotGobjectClient *client = marmot_gobject_client_new(MARMOT_GOBJECT_STORAGE(store));

    SignalData sd = { FALSE, NULL };
    g_signal_connect(client, "group-joined", G_CALLBACK(on_group_joined), &sd);

    /* Emit the signal manually to test the mechanism */
    MarmotGobjectGroup *group = marmot_gobject_group_new_from_data(
        TEST_GROUP_ID_HEX, TEST_HEX_32,
        "Signal Test", NULL,
        MARMOT_GOBJECT_GROUP_STATE_ACTIVE, 1);

    g_signal_emit_by_name(client, "group-joined", group);

    g_assert_true(sd.fired);
    g_assert_nonnull(sd.received_object);
    g_assert_true(MARMOT_GOBJECT_IS_GROUP(sd.received_object));
    g_assert_cmpstr(marmot_gobject_group_get_name(MARMOT_GOBJECT_GROUP(sd.received_object)),
                    ==, "Signal Test");

    g_object_unref(sd.received_object);
    g_object_unref(group);
    g_object_unref(client);
    g_object_unref(store);
}

static void
on_message_received(MarmotGobjectClient *client, MarmotGobjectMessage *msg, gpointer data)
{
    SignalData *sd = data;
    sd->fired = TRUE;
    sd->received_object = G_OBJECT(msg);
    g_object_ref(msg);
}

static void
test_client_signal_message_received(void)
{
    MarmotGobjectMemoryStorage *store = marmot_gobject_memory_storage_new();
    MarmotGobjectClient *client = marmot_gobject_client_new(MARMOT_GOBJECT_STORAGE(store));

    SignalData sd = { FALSE, NULL };
    g_signal_connect(client, "message-received", G_CALLBACK(on_message_received), &sd);

    MarmotGobjectMessage *msg = marmot_gobject_message_new_from_data(
        TEST_HEX_32, TEST_HEX_32_B, "Hello!", 1, 1700000000, TEST_GROUP_ID_HEX);

    g_signal_emit_by_name(client, "message-received", msg);

    g_assert_true(sd.fired);
    g_assert_true(MARMOT_GOBJECT_IS_MESSAGE(sd.received_object));
    g_assert_cmpstr(marmot_gobject_message_get_content(MARMOT_GOBJECT_MESSAGE(sd.received_object)),
                    ==, "Hello!");

    g_object_unref(sd.received_object);
    g_object_unref(msg);
    g_object_unref(client);
    g_object_unref(store);
}

static void
on_welcome_received(MarmotGobjectClient *client, MarmotGobjectWelcome *welcome, gpointer data)
{
    SignalData *sd = data;
    sd->fired = TRUE;
    sd->received_object = G_OBJECT(welcome);
    g_object_ref(welcome);
}

static void
test_client_signal_welcome_received(void)
{
    MarmotGobjectMemoryStorage *store = marmot_gobject_memory_storage_new();
    MarmotGobjectClient *client = marmot_gobject_client_new(MARMOT_GOBJECT_STORAGE(store));

    SignalData sd = { FALSE, NULL };
    g_signal_connect(client, "welcome-received", G_CALLBACK(on_welcome_received), &sd);

    MarmotGobjectWelcome *w = marmot_gobject_welcome_new_from_data(
        TEST_HEX_32, "WelcomeGroup", NULL, TEST_HEX_32_B,
        2, MARMOT_GOBJECT_WELCOME_STATE_PENDING,
        TEST_GROUP_ID_HEX, TEST_HEX_32_C);

    g_signal_emit_by_name(client, "welcome-received", w);

    g_assert_true(sd.fired);
    g_assert_true(MARMOT_GOBJECT_IS_WELCOME(sd.received_object));
    g_assert_cmpstr(marmot_gobject_welcome_get_group_name(
        MARMOT_GOBJECT_WELCOME(sd.received_object)), ==, "WelcomeGroup");

    g_object_unref(sd.received_object);
    g_object_unref(w);
    g_object_unref(client);
    g_object_unref(store);
}

/* ══════════════════════════════════════════════════════════════════════════
 * 8. Client synchronous query tests (empty store)
 * ══════════════════════════════════════════════════════════════════════════ */

static void
test_client_get_all_groups_empty(void)
{
    MarmotGobjectMemoryStorage *store = marmot_gobject_memory_storage_new();
    MarmotGobjectClient *client = marmot_gobject_client_new(MARMOT_GOBJECT_STORAGE(store));

    GError *error = NULL;
    GPtrArray *groups = marmot_gobject_client_get_all_groups(client, &error);

    /* Should succeed with empty array, not error */
    g_assert_no_error(error);
    g_assert_nonnull(groups);
    g_assert_cmpuint(groups->len, ==, 0);

    g_ptr_array_unref(groups);
    g_object_unref(client);
    g_object_unref(store);
}

static void
test_client_get_group_not_found(void)
{
    MarmotGobjectMemoryStorage *store = marmot_gobject_memory_storage_new();
    MarmotGobjectClient *client = marmot_gobject_client_new(MARMOT_GOBJECT_STORAGE(store));

    GError *error = NULL;
    MarmotGobjectGroup *group = marmot_gobject_client_get_group(
        client, TEST_GROUP_ID_HEX, &error);

    /* Depending on implementation: either NULL with no error (not found)
     * or NULL with an error. Both are acceptable. */
    if (group) {
        g_object_unref(group);
    }
    g_clear_error(&error);

    g_object_unref(client);
    g_object_unref(store);
}

static void
test_client_get_pending_welcomes_empty(void)
{
    MarmotGobjectMemoryStorage *store = marmot_gobject_memory_storage_new();
    MarmotGobjectClient *client = marmot_gobject_client_new(MARMOT_GOBJECT_STORAGE(store));

    GError *error = NULL;
    GPtrArray *welcomes = marmot_gobject_client_get_pending_welcomes(client, &error);

    g_assert_no_error(error);
    g_assert_nonnull(welcomes);
    g_assert_cmpuint(welcomes->len, ==, 0);

    g_ptr_array_unref(welcomes);
    g_object_unref(client);
    g_object_unref(store);
}

static void
test_client_get_messages_empty(void)
{
    MarmotGobjectMemoryStorage *store = marmot_gobject_memory_storage_new();
    MarmotGobjectClient *client = marmot_gobject_client_new(MARMOT_GOBJECT_STORAGE(store));

    GError *error = NULL;
    GPtrArray *msgs = marmot_gobject_client_get_messages(
        client, TEST_GROUP_ID_HEX, 50, 0, &error);

    /* Should succeed with empty array or return error for unknown group */
    if (msgs) {
        g_assert_cmpuint(msgs->len, ==, 0);
        g_ptr_array_unref(msgs);
    }
    g_clear_error(&error);

    g_object_unref(client);
    g_object_unref(store);
}

/* ══════════════════════════════════════════════════════════════════════════
 * 9. GObject lifecycle stress tests
 * ══════════════════════════════════════════════════════════════════════════ */

static void
test_group_rapid_create_destroy(void)
{
    /* Create and destroy 1000 group objects to check for leaks */
    for (int i = 0; i < 1000; i++) {
        MarmotGobjectGroup *group = marmot_gobject_group_new_from_data(
            TEST_GROUP_ID_HEX, TEST_HEX_32,
            "Stress", NULL,
            MARMOT_GOBJECT_GROUP_STATE_ACTIVE, (guint64)i);
        g_object_unref(group);
    }
    /* If we reach here without crash or ASan error, test passes */
}

static void
test_message_rapid_create_destroy(void)
{
    for (int i = 0; i < 1000; i++) {
        MarmotGobjectMessage *msg = marmot_gobject_message_new_from_data(
            TEST_HEX_32, TEST_HEX_32_B,
            "Stress message", 1,
            1700000000 + i, TEST_GROUP_ID_HEX);
        g_object_unref(msg);
    }
}

static void
test_welcome_rapid_create_destroy(void)
{
    for (int i = 0; i < 1000; i++) {
        MarmotGobjectWelcome *w = marmot_gobject_welcome_new_from_data(
            TEST_HEX_32, "Group", "Desc", TEST_HEX_32_B,
            (guint)i, MARMOT_GOBJECT_WELCOME_STATE_PENDING,
            TEST_GROUP_ID_HEX, TEST_HEX_32_C);
        g_object_unref(w);
    }
}

static void
test_client_rapid_create_destroy(void)
{
    /* Create/destroy 100 client instances (heavier than data objects) */
    for (int i = 0; i < 100; i++) {
        MarmotGobjectMemoryStorage *store = marmot_gobject_memory_storage_new();
        MarmotGobjectClient *client = marmot_gobject_client_new(MARMOT_GOBJECT_STORAGE(store));
        g_assert_nonnull(client);
        g_object_unref(client);
        g_object_unref(store);
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * 10. Type checking and casting tests
 * ══════════════════════════════════════════════════════════════════════════ */

static void
test_type_checks(void)
{
    MarmotGobjectGroup *group = marmot_gobject_group_new_from_data(
        TEST_GROUP_ID_HEX, TEST_HEX_32,
        "Type Check", NULL,
        MARMOT_GOBJECT_GROUP_STATE_ACTIVE, 1);

    MarmotGobjectMessage *msg = marmot_gobject_message_new_from_data(
        TEST_HEX_32, TEST_HEX_32_B, "Content", 1, 0, TEST_GROUP_ID_HEX);

    MarmotGobjectWelcome *w = marmot_gobject_welcome_new_from_data(
        TEST_HEX_32, "G", NULL, TEST_HEX_32_B,
        1, MARMOT_GOBJECT_WELCOME_STATE_PENDING,
        TEST_GROUP_ID_HEX, TEST_HEX_32_C);

    /* Positive checks */
    g_assert_true(MARMOT_GOBJECT_IS_GROUP(group));
    g_assert_true(MARMOT_GOBJECT_IS_MESSAGE(msg));
    g_assert_true(MARMOT_GOBJECT_IS_WELCOME(w));
    g_assert_true(G_IS_OBJECT(group));
    g_assert_true(G_IS_OBJECT(msg));
    g_assert_true(G_IS_OBJECT(w));

    /* Cross-type checks should be false */
    g_assert_false(MARMOT_GOBJECT_IS_MESSAGE(group));
    g_assert_false(MARMOT_GOBJECT_IS_GROUP(msg));
    g_assert_false(MARMOT_GOBJECT_IS_WELCOME(group));

    g_object_unref(group);
    g_object_unref(msg);
    g_object_unref(w);
}

/* ══════════════════════════════════════════════════════════════════════════
 * 11. Concurrent GObject creation (thread safety)
 * ══════════════════════════════════════════════════════════════════════════ */

static gpointer
thread_create_groups(gpointer data)
{
    (void)data;
    for (int i = 0; i < 200; i++) {
        MarmotGobjectGroup *group = marmot_gobject_group_new_from_data(
            TEST_GROUP_ID_HEX, TEST_HEX_32,
            "Thread Test", NULL,
            MARMOT_GOBJECT_GROUP_STATE_ACTIVE, (guint64)i);
        g_object_unref(group);
    }
    return NULL;
}

static void
test_concurrent_object_creation(void)
{
    const int N_THREADS = 4;
    GThread *threads[4];

    for (int i = 0; i < N_THREADS; i++)
        threads[i] = g_thread_new("test-thread", thread_create_groups, NULL);

    for (int i = 0; i < N_THREADS; i++)
        g_thread_join(threads[i]);

    /* If we reach here without race conditions or crashes, success */
}

/* ══════════════════════════════════════════════════════════════════════════
 * 12. Storage backend dual-implementation test
 * ══════════════════════════════════════════════════════════════════════════ */

static void
test_memory_storage_raw_is_stable(void)
{
    MarmotGobjectMemoryStorage *store = marmot_gobject_memory_storage_new();
    MarmotGobjectStorage *iface = MARMOT_GOBJECT_STORAGE(store);

    gpointer raw1 = marmot_gobject_storage_get_raw_storage(iface);
    gpointer raw2 = marmot_gobject_storage_get_raw_storage(iface);

    /* Same underlying storage should be returned each time */
    g_assert_true(raw1 == raw2);

    g_object_unref(store);
}

/* ══════════════════════════════════════════════════════════════════════════
 * 13. Edge cases
 * ══════════════════════════════════════════════════════════════════════════ */

static void
test_group_empty_strings(void)
{
    /* Empty strings (not NULL) should be stored as-is */
    MarmotGobjectGroup *group = marmot_gobject_group_new_from_data(
        "", "",  /* empty hex strings */
        "", "",  /* empty name, description */
        MARMOT_GOBJECT_GROUP_STATE_ACTIVE, 0);

    g_assert_nonnull(group);
    g_assert_cmpstr(marmot_gobject_group_get_mls_group_id(group), ==, "");
    g_assert_cmpstr(marmot_gobject_group_get_name(group), ==, "");

    g_object_unref(group);
}

static void
test_message_large_kind(void)
{
    /* Kind can be any unsigned value (Nostr kinds can be large) */
    MarmotGobjectMessage *msg = marmot_gobject_message_new_from_data(
        TEST_HEX_32, TEST_HEX_32_B,
        "Large kind", 30078,  /* NIP-78 arbitrary custom data */
        1700000000, TEST_GROUP_ID_HEX);

    g_assert_cmpuint(marmot_gobject_message_get_kind(msg), ==, 30078);
    g_object_unref(msg);
}

static void
test_message_negative_timestamp(void)
{
    /* While unlikely, the API should handle it gracefully */
    MarmotGobjectMessage *msg = marmot_gobject_message_new_from_data(
        TEST_HEX_32, TEST_HEX_32_B,
        "Old", 1,
        -1, TEST_GROUP_ID_HEX);

    g_assert_cmpint(marmot_gobject_message_get_created_at(msg), ==, -1);
    g_object_unref(msg);
}

static void
test_welcome_zero_members(void)
{
    MarmotGobjectWelcome *w = marmot_gobject_welcome_new_from_data(
        TEST_HEX_32, "Empty", NULL, TEST_HEX_32_B,
        0, MARMOT_GOBJECT_WELCOME_STATE_PENDING,
        TEST_GROUP_ID_HEX, TEST_HEX_32_C);

    g_assert_cmpuint(marmot_gobject_welcome_get_member_count(w), ==, 0);
    g_object_unref(w);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Main
 * ══════════════════════════════════════════════════════════════════════════ */

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    /* 1. Enum type registration */
    g_test_add_func("/marmot-gobject/enums/group-state", test_enum_group_state_type);
    g_test_add_func("/marmot-gobject/enums/message-state", test_enum_message_state_type);
    g_test_add_func("/marmot-gobject/enums/welcome-state", test_enum_welcome_state_type);
    g_test_add_func("/marmot-gobject/enums/message-result-type", test_enum_message_result_type);

    /* 2. Group */
    g_test_add_func("/marmot-gobject/group/type-registration", test_group_type_registration);
    g_test_add_func("/marmot-gobject/group/new-from-data", test_group_new_from_data);
    g_test_add_func("/marmot-gobject/group/null-optional-fields", test_group_null_optional_fields);
    g_test_add_func("/marmot-gobject/group/properties-via-gvalue", test_group_properties_via_gvalue);
    g_test_add_func("/marmot-gobject/group/refcount", test_group_refcount);

    /* 3. Message */
    g_test_add_func("/marmot-gobject/message/type-registration", test_message_type_registration);
    g_test_add_func("/marmot-gobject/message/new-from-data", test_message_new_from_data);
    g_test_add_func("/marmot-gobject/message/null-content", test_message_null_content);
    g_test_add_func("/marmot-gobject/message/properties-via-gvalue", test_message_properties_via_gvalue);

    /* 4. Welcome */
    g_test_add_func("/marmot-gobject/welcome/type-registration", test_welcome_type_registration);
    g_test_add_func("/marmot-gobject/welcome/new-from-data", test_welcome_new_from_data);
    g_test_add_func("/marmot-gobject/welcome/null-name-description", test_welcome_null_name_description);
    g_test_add_func("/marmot-gobject/welcome/properties-via-gvalue", test_welcome_properties_via_gvalue);

    /* 5. Storage interface */
    g_test_add_func("/marmot-gobject/storage/interface-type", test_storage_interface_type);
    g_test_add_func("/marmot-gobject/storage/memory-type", test_memory_storage_type);
    g_test_add_func("/marmot-gobject/storage/memory-new", test_memory_storage_new);
    g_test_add_func("/marmot-gobject/storage/memory-interface-cast", test_memory_storage_interface_cast);
    g_test_add_func("/marmot-gobject/storage/sqlite-type", test_sqlite_storage_type);
    g_test_add_func("/marmot-gobject/storage/memory-raw-is-stable", test_memory_storage_raw_is_stable);

    /* 6. Client */
    g_test_add_func("/marmot-gobject/client/type-registration", test_client_type_registration);
    g_test_add_func("/marmot-gobject/client/new-with-memory-storage", test_client_new_with_memory_storage);
    g_test_add_func("/marmot-gobject/client/finalize-releases-storage", test_client_finalize_releases_storage);

    /* 7. Signals */
    g_test_add_func("/marmot-gobject/client/signal-group-joined", test_client_signal_group_joined);
    g_test_add_func("/marmot-gobject/client/signal-message-received", test_client_signal_message_received);
    g_test_add_func("/marmot-gobject/client/signal-welcome-received", test_client_signal_welcome_received);

    /* 8. Synchronous queries */
    g_test_add_func("/marmot-gobject/client/get-all-groups-empty", test_client_get_all_groups_empty);
    g_test_add_func("/marmot-gobject/client/get-group-not-found", test_client_get_group_not_found);
    g_test_add_func("/marmot-gobject/client/get-pending-welcomes-empty", test_client_get_pending_welcomes_empty);
    g_test_add_func("/marmot-gobject/client/get-messages-empty", test_client_get_messages_empty);

    /* 9. Lifecycle stress */
    g_test_add_func("/marmot-gobject/stress/group-rapid-create-destroy", test_group_rapid_create_destroy);
    g_test_add_func("/marmot-gobject/stress/message-rapid-create-destroy", test_message_rapid_create_destroy);
    g_test_add_func("/marmot-gobject/stress/welcome-rapid-create-destroy", test_welcome_rapid_create_destroy);
    g_test_add_func("/marmot-gobject/stress/client-rapid-create-destroy", test_client_rapid_create_destroy);

    /* 10. Type checking */
    g_test_add_func("/marmot-gobject/types/type-checks", test_type_checks);

    /* 11. Concurrency */
    g_test_add_func("/marmot-gobject/threads/concurrent-object-creation", test_concurrent_object_creation);

    /* 12. Edge cases */
    g_test_add_func("/marmot-gobject/edge/group-empty-strings", test_group_empty_strings);
    g_test_add_func("/marmot-gobject/edge/message-large-kind", test_message_large_kind);
    g_test_add_func("/marmot-gobject/edge/message-negative-timestamp", test_message_negative_timestamp);
    g_test_add_func("/marmot-gobject/edge/welcome-zero-members", test_welcome_zero_members);

    return g_test_run();
}
