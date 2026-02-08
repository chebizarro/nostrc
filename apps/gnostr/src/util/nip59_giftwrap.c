/**
 * gnostr NIP-59 Gift Wrap Implementation
 *
 * Provides async gift wrap creation and unwrapping using the D-Bus signer
 * interface for NIP-44 encryption operations.
 */

#include "nip59_giftwrap.h"
#include "../ipc/signer_ipc.h"
#include "../ipc/gnostr-signer-service.h"
#include "nostr-event.h"
#include "nostr-tag.h"
#include "nostr-kinds.h"

#include <string.h>
#include <stdlib.h>
#include <time.h>

/* Forward declarations for async callbacks */
static void on_rumor_encrypted(GObject *source, GAsyncResult *res, gpointer user_data);
static void on_seal_signed(GObject *source, GAsyncResult *res, gpointer user_data);
static void on_gift_wrap_seal_encrypted(GObject *source, GAsyncResult *res, gpointer user_data);
static void on_gift_wrap_signed(GObject *source, GAsyncResult *res, gpointer user_data);
static void on_seal_decrypted(GObject *source, GAsyncResult *res, gpointer user_data);
static void on_rumor_decrypted(GObject *source, GAsyncResult *res, gpointer user_data);

/* ============== Helper Functions ============== */

void gnostr_gift_wrap_result_free(GnostrGiftWrapResult *result) {
    if (!result) return;
    g_free(result->gift_wrap_json);
    g_free(result->error_message);
    g_free(result);
}

void gnostr_unwrap_result_free(GnostrUnwrapResult *result) {
    if (!result) return;
    if (result->rumor) nostr_event_free(result->rumor);
    g_free(result->sender_pubkey);
    g_free(result->error_message);
    g_free(result);
}

gint64 gnostr_nip59_get_randomized_timestamp(void) {
    gint64 now = (gint64)time(NULL);

    /* Use GLib's random functions for randomization */
    guint32 rand_val = g_random_int();
    gint64 offset = rand_val % NIP59_TIMESTAMP_WINDOW;

    return now - offset;
}

NostrEvent *gnostr_nip59_create_rumor(int kind,
                                       const char *sender_pubkey_hex,
                                       const char *content,
                                       NostrTags *tags) {
    if (!sender_pubkey_hex || !content) return NULL;

    NostrEvent *rumor = nostr_event_new();
    if (!rumor) return NULL;

    nostr_event_set_kind(rumor, kind);
    nostr_event_set_pubkey(rumor, sender_pubkey_hex);
    nostr_event_set_content(rumor, content);
    nostr_event_set_created_at(rumor, (gint64)time(NULL));

    /* Copy tags if provided */
    if (tags) {
        /* Note: nostr_event_set_tags takes ownership, so we need a copy */
        size_t tag_count = nostr_tags_size(tags);
        NostrTags *tags_copy = nostr_tags_new(0);

        for (size_t i = 0; i < tag_count; i++) {
            NostrTag *src_tag = nostr_tags_get(tags, i);
            if (src_tag) {
                /* Copy tag by recreating it with all elements */
                size_t src_size = nostr_tag_size(src_tag);
                if (src_size > 0) {
                    const char *key = nostr_tag_get(src_tag, 0);
                    NostrTag *tag_copy = nostr_tag_new(key, NULL);
                    if (tag_copy) {
                        for (size_t j = 1; j < src_size; j++) {
                            const char *val = nostr_tag_get(src_tag, j);
                            if (val) {
                                nostr_tag_append(tag_copy, val);
                            }
                        }
                        nostr_tags_append(tags_copy, tag_copy);
                    }
                }
            }
        }

        nostr_event_set_tags(rumor, tags_copy);
    }

    /* Rumor is NOT signed - sig remains NULL */
    return rumor;
}

NostrEvent *gnostr_nip59_create_dm_rumor(const char *sender_pubkey_hex,
                                          const char *recipient_pubkey_hex,
                                          const char *content) {
    if (!sender_pubkey_hex || !recipient_pubkey_hex || !content) return NULL;

    /* Create p-tag for recipient */
    NostrTag *ptag = nostr_tag_new("p", recipient_pubkey_hex, NULL);
    if (!ptag) return NULL;

    NostrTags *tags = nostr_tags_new(1, ptag);
    if (!tags) {
        nostr_tag_free(ptag);
        return NULL;
    }

    NostrEvent *rumor = nostr_event_new();
    if (!rumor) {
        nostr_tags_free(tags);
        return NULL;
    }

    nostr_event_set_kind(rumor, NOSTR_KIND_DIRECT_MESSAGE);
    nostr_event_set_pubkey(rumor, sender_pubkey_hex);
    nostr_event_set_content(rumor, content);
    nostr_event_set_created_at(rumor, (gint64)time(NULL));
    nostr_event_set_tags(rumor, tags);

    return rumor;
}

gboolean gnostr_nip59_validate_gift_wrap(NostrEvent *gift_wrap) {
    if (!gift_wrap) return FALSE;

    /* Check kind */
    if (nostr_event_get_kind(gift_wrap) != NOSTR_KIND_GIFT_WRAP) {
        return FALSE;
    }

    /* Check signature */
    if (!nostr_event_check_signature(gift_wrap)) {
        return FALSE;
    }

    /* Check for content */
    const char *content = nostr_event_get_content(gift_wrap);
    if (!content || !*content) {
        return FALSE;
    }

    /* Check for p-tag */
    NostrTags *tags = nostr_event_get_tags(gift_wrap);
    if (!tags || nostr_tags_size(tags) == 0) {
        return FALSE;
    }

    NostrTag *prefix = nostr_tag_new("p", NULL);
    NostrTag *ptag = nostr_tags_get_first(tags, prefix);
    nostr_tag_free(prefix);

    if (!ptag || nostr_tag_size(ptag) < 2) {
        return FALSE;
    }

    return TRUE;
}

char *gnostr_nip59_get_recipient_from_gift_wrap(NostrEvent *gift_wrap) {
    if (!gift_wrap) return NULL;

    NostrTags *tags = nostr_event_get_tags(gift_wrap);
    if (!tags) return NULL;

    NostrTag *prefix = nostr_tag_new("p", NULL);
    NostrTag *ptag = nostr_tags_get_first(tags, prefix);
    nostr_tag_free(prefix);

    if (!ptag || nostr_tag_size(ptag) < 2) {
        return NULL;
    }

    const char *recipient = nostr_tag_get(ptag, 1);
    return recipient ? g_strdup(recipient) : NULL;
}

/* ============== Async Gift Wrap Creation ============== */

/**
 * Context for multi-step async gift wrap creation
 */
typedef struct {
    /* Input parameters */
    char *recipient_pubkey_hex;
    char *sender_pubkey_hex;
    char *rumor_json;
    GCancellable *cancellable;
    GnostrGiftWrapCallback callback;
    gpointer user_data;

    /* Intermediate state */
    char *encrypted_rumor;      /* NIP-44 encrypted rumor JSON */
    char *seal_json;            /* Unsigned seal event JSON */
    char *signed_seal_json;     /* Signed seal event JSON */
    char *encrypted_seal;       /* NIP-44 encrypted seal JSON */
} GiftWrapCreateCtx;

static void gift_wrap_create_ctx_free(GiftWrapCreateCtx *ctx) {
    if (!ctx) return;
    g_free(ctx->recipient_pubkey_hex);
    g_free(ctx->sender_pubkey_hex);
    g_free(ctx->rumor_json);
    if (ctx->cancellable) g_object_unref(ctx->cancellable);
    g_free(ctx->encrypted_rumor);
    g_free(ctx->seal_json);
    g_free(ctx->signed_seal_json);
    g_free(ctx->encrypted_seal);
    g_free(ctx);
}

static void finish_gift_wrap_with_error(GiftWrapCreateCtx *ctx, const char *msg) {
    GnostrGiftWrapResult *result = g_new0(GnostrGiftWrapResult, 1);
    result->success = FALSE;
    result->error_message = g_strdup(msg);

    if (ctx->callback) {
        ctx->callback(result, ctx->user_data);
    } else {
        gnostr_gift_wrap_result_free(result);
    }

    gift_wrap_create_ctx_free(ctx);
}

/* Step 4: Gift wrap creation complete - finalize and return */
static void on_gift_wrap_seal_encrypted(GObject *source, GAsyncResult *res, gpointer user_data);

/* Step 3: Seal signed - now encrypt it with ephemeral key for gift wrap */
static void on_seal_signed(GObject *source, GAsyncResult *res, gpointer user_data) {
    GiftWrapCreateCtx *ctx = (GiftWrapCreateCtx *)user_data;
    GError *error = NULL;

    (void)source;

    char *signed_seal = NULL;
    gboolean ok = gnostr_sign_event_finish(res, &signed_seal, &error);

    if (!ok || !signed_seal) {
        g_warning("[NIP59] Failed to sign seal: %s", error ? error->message : "unknown");
        finish_gift_wrap_with_error(ctx, error ? error->message : "Failed to sign seal");
        g_clear_error(&error);
        return;
    }

    ctx->signed_seal_json = signed_seal;
    g_debug("[NIP59] Seal signed, encrypting for gift wrap");

    /* Get signer proxy for NIP-44 encryption with ephemeral key */
    NostrSignerProxy *proxy = gnostr_signer_proxy_get(&error);
    if (!proxy) {
        g_warning("[NIP59] Failed to get signer proxy: %s", error ? error->message : "unknown");
        finish_gift_wrap_with_error(ctx, "Signer not available");
        g_clear_error(&error);
        return;
    }

    /* Encrypt seal JSON using sender's key (will be decrypted by recipient) */
    /* Note: For a true ephemeral key implementation, we would need the signer
     * to support generating and using ephemeral keys. For now, we use the
     * sender's key and rely on the randomized timestamp for metadata protection. */
    nostr_org_nostr_signer_call_nip44_encrypt(
        proxy,
        ctx->signed_seal_json,
        ctx->recipient_pubkey_hex,
        ctx->sender_pubkey_hex,
        NULL, /* GCancellable */
        on_gift_wrap_seal_encrypted,
        ctx);
}

/* Step 4: Seal encrypted - create and sign gift wrap event */
static void on_gift_wrap_seal_encrypted(GObject *source, GAsyncResult *res, gpointer user_data) {
    GiftWrapCreateCtx *ctx = (GiftWrapCreateCtx *)user_data;
    GError *error = NULL;

    NostrSignerProxy *proxy = NOSTR_ORG_NOSTR_SIGNER(source);

    char *encrypted_seal = NULL;
    gboolean ok = nostr_org_nostr_signer_call_nip44_encrypt_finish(
        proxy, &encrypted_seal, res, &error);

    if (!ok || !encrypted_seal) {
        g_warning("[NIP59] Failed to encrypt seal: %s", error ? error->message : "unknown");
        finish_gift_wrap_with_error(ctx, error ? error->message : "Failed to encrypt seal");
        g_clear_error(&error);
        return;
    }

    ctx->encrypted_seal = encrypted_seal;
    g_debug("[NIP59] Seal encrypted, creating gift wrap");

    /* Create gift wrap event */
    NostrEvent *gift_wrap = nostr_event_new();
    if (!gift_wrap) {
        finish_gift_wrap_with_error(ctx, "Failed to create gift wrap event");
        return;
    }

    nostr_event_set_kind(gift_wrap, NOSTR_KIND_GIFT_WRAP);
    nostr_event_set_pubkey(gift_wrap, ctx->sender_pubkey_hex);
    nostr_event_set_content(gift_wrap, ctx->encrypted_seal);
    nostr_event_set_created_at(gift_wrap, gnostr_nip59_get_randomized_timestamp());

    /* Add p-tag for recipient */
    NostrTag *ptag = nostr_tag_new("p", ctx->recipient_pubkey_hex, NULL);
    NostrTags *tags = nostr_tags_new(1, ptag);
    nostr_event_set_tags(gift_wrap, tags);

    /* Serialize and sign via signer service */
    char *gift_wrap_json = nostr_event_serialize_compact(gift_wrap);
    nostr_event_free(gift_wrap);

    if (!gift_wrap_json) {
        finish_gift_wrap_with_error(ctx, "Failed to serialize gift wrap");
        return;
    }

    /* Sign the gift wrap event */
    gnostr_sign_event_async(
        gift_wrap_json,
        ctx->sender_pubkey_hex,
        "gnostr",
        ctx->cancellable,
        (GAsyncReadyCallback)on_gift_wrap_signed,
        ctx);

    g_free(gift_wrap_json);
}

/* Step 5: Gift wrap signed - complete */
static void on_gift_wrap_signed(GObject *source, GAsyncResult *res, gpointer user_data) {
    GiftWrapCreateCtx *ctx = (GiftWrapCreateCtx *)user_data;
    GError *error = NULL;

    (void)source;

    char *signed_gift_wrap = NULL;
    gboolean ok = gnostr_sign_event_finish(res, &signed_gift_wrap, &error);

    if (!ok || !signed_gift_wrap) {
        g_warning("[NIP59] Failed to sign gift wrap: %s", error ? error->message : "unknown");
        finish_gift_wrap_with_error(ctx, error ? error->message : "Failed to sign gift wrap");
        g_clear_error(&error);
        return;
    }

    g_message("[NIP59] Gift wrap created successfully");

    /* Success! */
    GnostrGiftWrapResult *result = g_new0(GnostrGiftWrapResult, 1);
    result->success = TRUE;
    result->gift_wrap_json = signed_gift_wrap;

    if (ctx->callback) {
        ctx->callback(result, ctx->user_data);
    } else {
        gnostr_gift_wrap_result_free(result);
    }

    gift_wrap_create_ctx_free(ctx);
}

/* Step 2: Rumor encrypted - create and sign seal */
static void on_rumor_encrypted(GObject *source, GAsyncResult *res, gpointer user_data) {
    GiftWrapCreateCtx *ctx = (GiftWrapCreateCtx *)user_data;
    GError *error = NULL;

    NostrSignerProxy *proxy = NOSTR_ORG_NOSTR_SIGNER(source);

    char *encrypted_rumor = NULL;
    gboolean ok = nostr_org_nostr_signer_call_nip44_encrypt_finish(
        proxy, &encrypted_rumor, res, &error);

    if (!ok || !encrypted_rumor) {
        g_warning("[NIP59] Failed to encrypt rumor: %s", error ? error->message : "unknown");
        finish_gift_wrap_with_error(ctx, error ? error->message : "Failed to encrypt rumor");
        g_clear_error(&error);
        return;
    }

    ctx->encrypted_rumor = encrypted_rumor;
    g_debug("[NIP59] Rumor encrypted, creating seal");

    /* Create seal event (kind 13) */
    NostrEvent *seal = nostr_event_new();
    if (!seal) {
        finish_gift_wrap_with_error(ctx, "Failed to create seal event");
        return;
    }

    nostr_event_set_kind(seal, NIP59_KIND_SEAL);
    nostr_event_set_pubkey(seal, ctx->sender_pubkey_hex);
    nostr_event_set_content(seal, ctx->encrypted_rumor);
    nostr_event_set_created_at(seal, (gint64)time(NULL));

    /* Seal has no tags */
    NostrTags *empty_tags = nostr_tags_new(0);
    nostr_event_set_tags(seal, empty_tags);

    /* Serialize seal */
    char *seal_json = nostr_event_serialize_compact(seal);
    nostr_event_free(seal);

    if (!seal_json) {
        finish_gift_wrap_with_error(ctx, "Failed to serialize seal");
        return;
    }

    ctx->seal_json = seal_json;

    /* Sign the seal via signer service */
    gnostr_sign_event_async(
        ctx->seal_json,
        ctx->sender_pubkey_hex,
        "gnostr",
        ctx->cancellable,
        (GAsyncReadyCallback)on_seal_signed,
        ctx);
}

void gnostr_nip59_create_gift_wrap_async(NostrEvent *rumor,
                                          const char *recipient_pubkey_hex,
                                          const char *sender_pubkey_hex,
                                          GCancellable *cancellable,
                                          GnostrGiftWrapCallback callback,
                                          gpointer user_data) {
    if (!rumor || !recipient_pubkey_hex || !sender_pubkey_hex) {
        GnostrGiftWrapResult *result = g_new0(GnostrGiftWrapResult, 1);
        result->success = FALSE;
        result->error_message = g_strdup("Invalid parameters");
        if (callback) callback(result, user_data);
        else gnostr_gift_wrap_result_free(result);
        return;
    }

    /* Serialize rumor to JSON */
    char *rumor_json = nostr_event_serialize_compact(rumor);
    if (!rumor_json) {
        GnostrGiftWrapResult *result = g_new0(GnostrGiftWrapResult, 1);
        result->success = FALSE;
        result->error_message = g_strdup("Failed to serialize rumor");
        if (callback) callback(result, user_data);
        else gnostr_gift_wrap_result_free(result);
        return;
    }

    /* Get signer proxy */
    GError *error = NULL;
    NostrSignerProxy *proxy = gnostr_signer_proxy_get(&error);
    if (!proxy) {
        g_warning("[NIP59] Failed to get signer proxy: %s", error ? error->message : "unknown");
        GnostrGiftWrapResult *result = g_new0(GnostrGiftWrapResult, 1);
        result->success = FALSE;
        result->error_message = g_strdup("Signer not available");
        if (callback) callback(result, user_data);
        else gnostr_gift_wrap_result_free(result);
        g_free(rumor_json);
        g_clear_error(&error);
        return;
    }

    /* Create context for async operation */
    GiftWrapCreateCtx *ctx = g_new0(GiftWrapCreateCtx, 1);
    ctx->recipient_pubkey_hex = g_strdup(recipient_pubkey_hex);
    ctx->sender_pubkey_hex = g_strdup(sender_pubkey_hex);
    ctx->rumor_json = rumor_json;
    ctx->cancellable = cancellable ? g_object_ref(cancellable) : NULL;
    ctx->callback = callback;
    ctx->user_data = user_data;

    g_debug("[NIP59] Starting gift wrap creation for recipient %.8s", recipient_pubkey_hex);

    /* Step 1: Encrypt rumor JSON using NIP-44 */
    nostr_org_nostr_signer_call_nip44_encrypt(
        proxy,
        ctx->rumor_json,
        ctx->recipient_pubkey_hex,
        ctx->sender_pubkey_hex,
        NULL, /* GCancellable */
        on_rumor_encrypted,
        ctx);
}

/* ============== Async Unwrap ============== */

/**
 * Context for multi-step async unwrap
 */
typedef struct {
    /* Input parameters */
    char *user_pubkey_hex;
    char *ephemeral_pubkey;     /* Gift wrap sender (ephemeral key) */
    char *encrypted_seal;       /* Gift wrap content */
    GCancellable *cancellable;
    GnostrUnwrapCallback callback;
    gpointer user_data;

    /* Intermediate state */
    char *seal_pubkey;          /* Real sender pubkey from seal */
    char *encrypted_rumor;      /* Seal content */
} UnwrapCtx;

static void unwrap_ctx_free(UnwrapCtx *ctx) {
    if (!ctx) return;
    g_free(ctx->user_pubkey_hex);
    g_free(ctx->ephemeral_pubkey);
    g_free(ctx->encrypted_seal);
    if (ctx->cancellable) g_object_unref(ctx->cancellable);
    g_free(ctx->seal_pubkey);
    g_free(ctx->encrypted_rumor);
    g_free(ctx);
}

static void finish_unwrap_with_error(UnwrapCtx *ctx, const char *msg) {
    GnostrUnwrapResult *result = g_new0(GnostrUnwrapResult, 1);
    result->success = FALSE;
    result->error_message = g_strdup(msg);

    if (ctx->callback) {
        ctx->callback(result, ctx->user_data);
    } else {
        gnostr_unwrap_result_free(result);
    }

    unwrap_ctx_free(ctx);
}

/* Step 3: Rumor decrypted - validate and complete */
static void on_rumor_decrypted(GObject *source, GAsyncResult *res, gpointer user_data) {
    UnwrapCtx *ctx = (UnwrapCtx *)user_data;
    GError *error = NULL;

    NostrSignerProxy *proxy = NOSTR_ORG_NOSTR_SIGNER(source);

    char *rumor_json = NULL;
    gboolean ok = nostr_org_nostr_signer_call_nip44_decrypt_finish(
        proxy, &rumor_json, res, &error);

    if (!ok || !rumor_json) {
        g_warning("[NIP59] Failed to decrypt rumor: %s", error ? error->message : "unknown");
        finish_unwrap_with_error(ctx, error ? error->message : "Failed to decrypt rumor");
        g_clear_error(&error);
        return;
    }

    g_debug("[NIP59] Rumor decrypted: %.100s...", rumor_json);

    /* Parse rumor event */
    NostrEvent *rumor = nostr_event_new();
    if (!nostr_event_deserialize_compact(rumor, rumor_json, NULL)) {
        g_warning("[NIP59] Failed to parse rumor JSON");
        nostr_event_free(rumor);
        g_free(rumor_json);
        finish_unwrap_with_error(ctx, "Failed to parse rumor");
        return;
    }
    g_free(rumor_json);

    /* Validate: seal pubkey must match rumor pubkey (anti-spoofing) */
    const char *rumor_pubkey = nostr_event_get_pubkey(rumor);
    if (!rumor_pubkey || !ctx->seal_pubkey ||
        strcmp(rumor_pubkey, ctx->seal_pubkey) != 0) {
        g_warning("[NIP59] Pubkey mismatch: seal=%s rumor=%s",
                  ctx->seal_pubkey ? ctx->seal_pubkey : "(null)",
                  rumor_pubkey ? rumor_pubkey : "(null)");
        nostr_event_free(rumor);
        finish_unwrap_with_error(ctx, "Sender pubkey mismatch (spoofing attempt?)");
        return;
    }

    g_message("[NIP59] Gift wrap unwrapped successfully from sender %.8s", ctx->seal_pubkey);

    /* Success! */
    GnostrUnwrapResult *result = g_new0(GnostrUnwrapResult, 1);
    result->success = TRUE;
    result->rumor = rumor;
    result->sender_pubkey = g_strdup(ctx->seal_pubkey);

    if (ctx->callback) {
        ctx->callback(result, ctx->user_data);
    } else {
        gnostr_unwrap_result_free(result);
    }

    unwrap_ctx_free(ctx);
}

/* Step 2: Seal decrypted - parse, validate, decrypt rumor */
static void on_seal_decrypted(GObject *source, GAsyncResult *res, gpointer user_data) {
    UnwrapCtx *ctx = (UnwrapCtx *)user_data;
    GError *error = NULL;

    NostrSignerProxy *proxy = NOSTR_ORG_NOSTR_SIGNER(source);

    char *seal_json = NULL;
    gboolean ok = nostr_org_nostr_signer_call_nip44_decrypt_finish(
        proxy, &seal_json, res, &error);

    if (!ok || !seal_json) {
        g_warning("[NIP59] Failed to decrypt seal: %s", error ? error->message : "unknown");
        finish_unwrap_with_error(ctx, error ? error->message : "Failed to decrypt seal");
        g_clear_error(&error);
        return;
    }

    g_debug("[NIP59] Seal decrypted: %.100s...", seal_json);

    /* Parse seal event */
    NostrEvent *seal = nostr_event_new();
    if (!nostr_event_deserialize_compact(seal, seal_json, NULL)) {
        g_warning("[NIP59] Failed to parse seal JSON");
        nostr_event_free(seal);
        g_free(seal_json);
        finish_unwrap_with_error(ctx, "Failed to parse seal");
        return;
    }
    g_free(seal_json);

    /* Validate seal kind */
    if (nostr_event_get_kind(seal) != NIP59_KIND_SEAL) {
        g_warning("[NIP59] Invalid seal kind: %d", nostr_event_get_kind(seal));
        nostr_event_free(seal);
        finish_unwrap_with_error(ctx, "Invalid seal kind");
        return;
    }

    /* Validate seal signature */
    if (!nostr_event_check_signature(seal)) {
        g_warning("[NIP59] Invalid seal signature");
        nostr_event_free(seal);
        finish_unwrap_with_error(ctx, "Invalid seal signature");
        return;
    }

    /* Store seal pubkey (real sender) and encrypted rumor */
    ctx->seal_pubkey = g_strdup(nostr_event_get_pubkey(seal));
    ctx->encrypted_rumor = g_strdup(nostr_event_get_content(seal));

    nostr_event_free(seal);

    if (!ctx->seal_pubkey || !ctx->encrypted_rumor) {
        finish_unwrap_with_error(ctx, "Missing seal pubkey or content");
        return;
    }

    g_debug("[NIP59] Seal validated, decrypting rumor from sender %.8s", ctx->seal_pubkey);

    /* Decrypt rumor using seal sender's pubkey */
    nostr_org_nostr_signer_call_nip44_decrypt(
        proxy,
        ctx->encrypted_rumor,
        ctx->seal_pubkey,
        ctx->user_pubkey_hex,
        NULL, /* GCancellable */
        on_rumor_decrypted,
        ctx);
}

void gnostr_nip59_unwrap_async(NostrEvent *gift_wrap,
                                const char *user_pubkey_hex,
                                GCancellable *cancellable,
                                GnostrUnwrapCallback callback,
                                gpointer user_data) {
    if (!gift_wrap || !user_pubkey_hex) {
        GnostrUnwrapResult *result = g_new0(GnostrUnwrapResult, 1);
        result->success = FALSE;
        result->error_message = g_strdup("Invalid parameters");
        if (callback) callback(result, user_data);
        else gnostr_unwrap_result_free(result);
        return;
    }

    /* Validate gift wrap */
    if (!gnostr_nip59_validate_gift_wrap(gift_wrap)) {
        GnostrUnwrapResult *result = g_new0(GnostrUnwrapResult, 1);
        result->success = FALSE;
        result->error_message = g_strdup("Invalid gift wrap event");
        if (callback) callback(result, user_data);
        else gnostr_unwrap_result_free(result);
        return;
    }

    const char *ephemeral_pk = nostr_event_get_pubkey(gift_wrap);
    const char *encrypted_content = nostr_event_get_content(gift_wrap);

    if (!ephemeral_pk || !encrypted_content) {
        GnostrUnwrapResult *result = g_new0(GnostrUnwrapResult, 1);
        result->success = FALSE;
        result->error_message = g_strdup("Missing gift wrap pubkey or content");
        if (callback) callback(result, user_data);
        else gnostr_unwrap_result_free(result);
        return;
    }

    /* Get signer proxy */
    GError *error = NULL;
    NostrSignerProxy *proxy = gnostr_signer_proxy_get(&error);
    if (!proxy) {
        g_warning("[NIP59] Failed to get signer proxy: %s", error ? error->message : "unknown");
        GnostrUnwrapResult *result = g_new0(GnostrUnwrapResult, 1);
        result->success = FALSE;
        result->error_message = g_strdup("Signer not available");
        if (callback) callback(result, user_data);
        else gnostr_unwrap_result_free(result);
        g_clear_error(&error);
        return;
    }

    /* Create context */
    UnwrapCtx *ctx = g_new0(UnwrapCtx, 1);
    ctx->user_pubkey_hex = g_strdup(user_pubkey_hex);
    ctx->ephemeral_pubkey = g_strdup(ephemeral_pk);
    ctx->encrypted_seal = g_strdup(encrypted_content);
    ctx->cancellable = cancellable ? g_object_ref(cancellable) : NULL;
    ctx->callback = callback;
    ctx->user_data = user_data;

    char *gift_wrap_id = nostr_event_get_id(gift_wrap);
    g_debug("[NIP59] Unwrapping gift wrap %.8s from ephemeral key %.8s",
            gift_wrap_id ? gift_wrap_id : "(null)", ephemeral_pk);
    g_free(gift_wrap_id);

    /* Step 1: Decrypt gift wrap content to get seal */
    nostr_org_nostr_signer_call_nip44_decrypt(
        proxy,
        ctx->encrypted_seal,
        ctx->ephemeral_pubkey,
        ctx->user_pubkey_hex,
        NULL, /* GCancellable */
        on_seal_decrypted,
        ctx);
}
