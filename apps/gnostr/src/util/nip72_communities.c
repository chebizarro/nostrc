/**
 * NIP-72 Moderated Communities Implementation
 */

#include "nip72_communities.h"
#include <string.h>
#include "json.h"

/* ===== GnostrCommunityModerator functions ===== */

GnostrCommunityModerator *
gnostr_community_moderator_new(void)
{
    return g_new0(GnostrCommunityModerator, 1);
}

void
gnostr_community_moderator_free(GnostrCommunityModerator *mod)
{
    if (!mod) return;

    g_free(mod->pubkey);
    g_free(mod->relay_hint);
    g_free(mod->petname);
    g_free(mod);
}

GnostrCommunityModerator *
gnostr_community_moderator_copy(const GnostrCommunityModerator *mod)
{
    if (!mod) return NULL;

    GnostrCommunityModerator *copy = gnostr_community_moderator_new();
    copy->pubkey = g_strdup(mod->pubkey);
    copy->relay_hint = g_strdup(mod->relay_hint);
    copy->petname = g_strdup(mod->petname);

    return copy;
}

/* ===== GnostrCommunity functions ===== */

GnostrCommunity *
gnostr_community_new(void)
{
    GnostrCommunity *community = g_new0(GnostrCommunity, 1);
    community->moderators = g_ptr_array_new_with_free_func(
        (GDestroyNotify)gnostr_community_moderator_free);
    return community;
}

void
gnostr_community_free(GnostrCommunity *community)
{
    if (!community) return;

    g_free(community->event_id);
    g_free(community->creator_pubkey);
    g_free(community->d_tag);
    g_free(community->name);
    g_free(community->description);
    g_free(community->image);
    g_free(community->rules);
    g_ptr_array_unref(community->moderators);
    g_free(community);
}

GnostrCommunity *
gnostr_community_copy(const GnostrCommunity *community)
{
    if (!community) return NULL;

    GnostrCommunity *copy = gnostr_community_new();
    copy->event_id = g_strdup(community->event_id);
    copy->creator_pubkey = g_strdup(community->creator_pubkey);
    copy->d_tag = g_strdup(community->d_tag);
    copy->name = g_strdup(community->name);
    copy->description = g_strdup(community->description);
    copy->image = g_strdup(community->image);
    copy->rules = g_strdup(community->rules);
    copy->created_at = community->created_at;
    copy->post_count = community->post_count;
    copy->member_count = community->member_count;

    /* Deep copy moderators */
    for (guint i = 0; i < community->moderators->len; i++) {
        GnostrCommunityModerator *mod = g_ptr_array_index(community->moderators, i);
        g_ptr_array_add(copy->moderators, gnostr_community_moderator_copy(mod));
    }

    return copy;
}

/* Callback context for parsing community tags */
typedef struct {
    GnostrCommunity *community;
} ParseCommunityTagsCtx;

static bool parse_community_tag_cb(size_t idx, const char *element_json, void *user_data) {
    (void)idx;
    ParseCommunityTagsCtx *ctx = (ParseCommunityTagsCtx *)user_data;
    GnostrCommunity *community = ctx->community;

    if (!nostr_json_is_array_str(element_json)) return true;

    size_t tag_len = 0;
    if (nostr_json_get_array_length(element_json, NULL, &tag_len) != 0 || tag_len < 2) {
        return true;
    }

    char *tag_name = NULL;
    char *tag_value = NULL;
    if (nostr_json_get_array_string(element_json, NULL, 0, &tag_name) != 0 || !tag_name) {
        return true;
    }
    if (nostr_json_get_array_string(element_json, NULL, 1, &tag_value) != 0 || !tag_value) {
        free(tag_name);
        return true;
    }

    if (strcmp(tag_name, "d") == 0) {
        g_free(community->d_tag);
        community->d_tag = g_strdup(tag_value);
        if (!community->name) {
            community->name = g_strdup(tag_value);
        }
    } else if (strcmp(tag_name, "name") == 0) {
        g_free(community->name);
        community->name = g_strdup(tag_value);
    } else if (strcmp(tag_name, "description") == 0) {
        g_free(community->description);
        community->description = g_strdup(tag_value);
    } else if (strcmp(tag_name, "image") == 0) {
        g_free(community->image);
        community->image = g_strdup(tag_value);
    } else if (strcmp(tag_name, "rules") == 0) {
        g_free(community->rules);
        community->rules = g_strdup(tag_value);
    } else if (strcmp(tag_name, "p") == 0) {
        char *relay_hint = NULL;
        char *role = NULL;
        char *petname = NULL;

        if (tag_len >= 3)
            nostr_json_get_array_string(element_json, NULL, 2, &relay_hint);
        if (tag_len >= 4)
            nostr_json_get_array_string(element_json, NULL, 3, &role);
        if (tag_len >= 5)
            nostr_json_get_array_string(element_json, NULL, 4, &petname);

        if (role && strcmp(role, "moderator") == 0) {
            GnostrCommunityModerator *mod = gnostr_community_moderator_new();
            mod->pubkey = g_strdup(tag_value);
            mod->relay_hint = relay_hint ? g_strdup(relay_hint) : NULL;
            mod->petname = petname ? g_strdup(petname) : NULL;
            g_ptr_array_add(community->moderators, mod);
        }

        free(relay_hint);
        free(role);
        free(petname);
    }

    free(tag_name);
    free(tag_value);
    return true;
}

gboolean
gnostr_community_parse_tags(const char *tags_json, GnostrCommunity *community)
{
    if (!tags_json || !community) return FALSE;

    if (!nostr_json_is_array_str(tags_json)) {
        return FALSE;
    }

    ParseCommunityTagsCtx ctx = { .community = community };
    nostr_json_array_foreach_root(tags_json, parse_community_tag_cb, &ctx);

    return (community->d_tag != NULL);
}

char *
gnostr_community_create_tags(const GnostrCommunity *community)
{
    if (!community || !community->d_tag) return NULL;

    NostrJsonBuilder *builder = nostr_json_builder_new();
    nostr_json_builder_begin_array(builder);

    /* d tag (required) */
    nostr_json_builder_begin_array(builder);
    nostr_json_builder_add_string(builder, "d");
    nostr_json_builder_add_string(builder, community->d_tag);
    nostr_json_builder_end_array(builder);

    /* name tag (if different from d tag) */
    if (community->name && strcmp(community->name, community->d_tag) != 0) {
        nostr_json_builder_begin_array(builder);
        nostr_json_builder_add_string(builder, "name");
        nostr_json_builder_add_string(builder, community->name);
        nostr_json_builder_end_array(builder);
    }

    /* description tag */
    if (community->description) {
        nostr_json_builder_begin_array(builder);
        nostr_json_builder_add_string(builder, "description");
        nostr_json_builder_add_string(builder, community->description);
        nostr_json_builder_end_array(builder);
    }

    /* image tag */
    if (community->image) {
        nostr_json_builder_begin_array(builder);
        nostr_json_builder_add_string(builder, "image");
        nostr_json_builder_add_string(builder, community->image);
        nostr_json_builder_end_array(builder);
    }

    /* rules tag */
    if (community->rules) {
        nostr_json_builder_begin_array(builder);
        nostr_json_builder_add_string(builder, "rules");
        nostr_json_builder_add_string(builder, community->rules);
        nostr_json_builder_end_array(builder);
    }

    /* moderator p tags */
    for (guint i = 0; i < community->moderators->len; i++) {
        GnostrCommunityModerator *mod = g_ptr_array_index(community->moderators, i);
        if (!mod->pubkey) continue;

        nostr_json_builder_begin_array(builder);
        nostr_json_builder_add_string(builder, "p");
        nostr_json_builder_add_string(builder, mod->pubkey);
        nostr_json_builder_add_string(builder, mod->relay_hint ? mod->relay_hint : "");
        nostr_json_builder_add_string(builder, "moderator");
        if (mod->petname) {
            nostr_json_builder_add_string(builder, mod->petname);
        }
        nostr_json_builder_end_array(builder);
    }

    nostr_json_builder_end_array(builder);
    char *result = nostr_json_builder_finish(builder);
    nostr_json_builder_free(builder);

    return result;
}

char *
gnostr_community_get_a_tag(const GnostrCommunity *community)
{
    if (!community || !community->creator_pubkey || !community->d_tag)
        return NULL;

    return g_strdup_printf("%d:%s:%s",
                           NIP72_KIND_COMMUNITY_DEFINITION,
                           community->creator_pubkey,
                           community->d_tag);
}

gboolean
gnostr_community_is_moderator(const GnostrCommunity *community,
                               const char *pubkey)
{
    if (!community || !pubkey) return FALSE;

    /* Creator is always a moderator */
    if (community->creator_pubkey &&
        strcmp(community->creator_pubkey, pubkey) == 0) {
        return TRUE;
    }

    /* Check moderators list */
    for (guint i = 0; i < community->moderators->len; i++) {
        GnostrCommunityModerator *mod = g_ptr_array_index(community->moderators, i);
        if (mod->pubkey && strcmp(mod->pubkey, pubkey) == 0) {
            return TRUE;
        }
    }

    return FALSE;
}

/* ===== GnostrApprovedPost functions ===== */

GnostrApprovedPost *
gnostr_approved_post_new(void)
{
    return g_new0(GnostrApprovedPost, 1);
}

void
gnostr_approved_post_free(GnostrApprovedPost *post)
{
    if (!post) return;

    g_free(post->approval_id);
    g_free(post->moderator_pubkey);
    g_free(post->post_event_id);
    g_free(post->post_author);
    g_free(post->community_a_tag);
    g_free(post);
}

GnostrApprovedPost *
gnostr_approved_post_copy(const GnostrApprovedPost *post)
{
    if (!post) return NULL;

    GnostrApprovedPost *copy = gnostr_approved_post_new();
    copy->approval_id = g_strdup(post->approval_id);
    copy->moderator_pubkey = g_strdup(post->moderator_pubkey);
    copy->post_event_id = g_strdup(post->post_event_id);
    copy->post_author = g_strdup(post->post_author);
    copy->community_a_tag = g_strdup(post->community_a_tag);
    copy->approved_at = post->approved_at;

    return copy;
}

/* Callback context for parsing approved post tags */
typedef struct {
    GnostrApprovedPost *approval;
} ParseApprovedPostTagsCtx;

static bool parse_approved_post_tag_cb(size_t idx, const char *element_json, void *user_data) {
    (void)idx;
    ParseApprovedPostTagsCtx *ctx = (ParseApprovedPostTagsCtx *)user_data;
    GnostrApprovedPost *approval = ctx->approval;

    if (!nostr_json_is_array_str(element_json)) return true;

    size_t tag_len = 0;
    if (nostr_json_get_array_length(element_json, NULL, &tag_len) != 0 || tag_len < 2) {
        return true;
    }

    char *tag_name = NULL;
    char *tag_value = NULL;
    if (nostr_json_get_array_string(element_json, NULL, 0, &tag_name) != 0 || !tag_name) {
        return true;
    }
    if (nostr_json_get_array_string(element_json, NULL, 1, &tag_value) != 0 || !tag_value) {
        free(tag_name);
        return true;
    }

    if (strcmp(tag_name, "a") == 0) {
        g_free(approval->community_a_tag);
        approval->community_a_tag = g_strdup(tag_value);
    } else if (strcmp(tag_name, "e") == 0) {
        g_free(approval->post_event_id);
        approval->post_event_id = g_strdup(tag_value);
    } else if (strcmp(tag_name, "p") == 0) {
        g_free(approval->post_author);
        approval->post_author = g_strdup(tag_value);
    }

    free(tag_name);
    free(tag_value);
    return true;
}

gboolean
gnostr_approved_post_parse_tags(const char *tags_json, GnostrApprovedPost *approval)
{
    if (!tags_json || !approval) return FALSE;

    if (!nostr_json_is_array_str(tags_json)) {
        return FALSE;
    }

    ParseApprovedPostTagsCtx ctx = { .approval = approval };
    nostr_json_array_foreach_root(tags_json, parse_approved_post_tag_cb, &ctx);

    return (approval->post_event_id != NULL && approval->community_a_tag != NULL);
}

char *
gnostr_approved_post_create_tags(const char *community_a_tag,
                                  const char *post_event_id,
                                  const char *post_author,
                                  const char *recommended_relay)
{
    if (!community_a_tag || !post_event_id || !post_author) return NULL;

    NostrJsonBuilder *builder = nostr_json_builder_new();
    nostr_json_builder_begin_array(builder);

    /* "a" tag for community reference */
    nostr_json_builder_begin_array(builder);
    nostr_json_builder_add_string(builder, "a");
    nostr_json_builder_add_string(builder, community_a_tag);
    if (recommended_relay)
        nostr_json_builder_add_string(builder, recommended_relay);
    nostr_json_builder_end_array(builder);

    /* "e" tag for approved post */
    nostr_json_builder_begin_array(builder);
    nostr_json_builder_add_string(builder, "e");
    nostr_json_builder_add_string(builder, post_event_id);
    if (recommended_relay)
        nostr_json_builder_add_string(builder, recommended_relay);
    nostr_json_builder_end_array(builder);

    /* "p" tag for post author */
    nostr_json_builder_begin_array(builder);
    nostr_json_builder_add_string(builder, "p");
    nostr_json_builder_add_string(builder, post_author);
    if (recommended_relay)
        nostr_json_builder_add_string(builder, recommended_relay);
    nostr_json_builder_end_array(builder);

    /* "k" tag for the kind of the approved post (kind 1 for notes) */
    nostr_json_builder_begin_array(builder);
    nostr_json_builder_add_string(builder, "k");
    nostr_json_builder_add_string(builder, "1");
    nostr_json_builder_end_array(builder);

    nostr_json_builder_end_array(builder);
    char *result = nostr_json_builder_finish(builder);
    nostr_json_builder_free(builder);

    return result;
}

/* ===== GnostrCommunityPost functions ===== */

GnostrCommunityPost *
gnostr_community_post_new(void)
{
    return g_new0(GnostrCommunityPost, 1);
}

void
gnostr_community_post_free(GnostrCommunityPost *post)
{
    if (!post) return;

    g_free(post->event_id);
    g_free(post->author_pubkey);
    g_free(post->content);
    g_free(post->community_a_tag);
    g_free(post->approval_id);
    g_free(post);
}

GnostrCommunityPost *
gnostr_community_post_copy(const GnostrCommunityPost *post)
{
    if (!post) return NULL;

    GnostrCommunityPost *copy = gnostr_community_post_new();
    copy->event_id = g_strdup(post->event_id);
    copy->author_pubkey = g_strdup(post->author_pubkey);
    copy->content = g_strdup(post->content);
    copy->community_a_tag = g_strdup(post->community_a_tag);
    copy->created_at = post->created_at;
    copy->is_approved = post->is_approved;
    copy->approval_id = g_strdup(post->approval_id);

    return copy;
}

/* Callback context for extracting a tag */
typedef struct {
    char *a_tag_value;
} ExtractATagCtx;

static bool extract_a_tag_cb(size_t idx, const char *element_json, void *user_data) {
    (void)idx;
    ExtractATagCtx *ctx = (ExtractATagCtx *)user_data;

    if (ctx->a_tag_value) return false;  /* Already found */

    if (!nostr_json_is_array_str(element_json)) return true;

    size_t tag_len = 0;
    if (nostr_json_get_array_length(element_json, NULL, &tag_len) != 0 || tag_len < 2) {
        return true;
    }

    char *tag_name = NULL;
    if (nostr_json_get_array_string(element_json, NULL, 0, &tag_name) != 0 || !tag_name) {
        return true;
    }

    if (strcmp(tag_name, "a") != 0) {
        free(tag_name);
        return true;
    }
    free(tag_name);

    char *tag_value = NULL;
    if (nostr_json_get_array_string(element_json, NULL, 1, &tag_value) != 0 || !tag_value) {
        return true;
    }

    /* Check if this is a community reference (kind 34550) */
    if (strncmp(tag_value, "34550:", 6) == 0) {
        ctx->a_tag_value = g_strdup(tag_value);
    }

    free(tag_value);
    return (ctx->a_tag_value == NULL);  /* Continue if not found */
}

char *
gnostr_community_post_extract_a_tag(const char *tags_json)
{
    if (!tags_json) return NULL;

    if (!nostr_json_is_array_str(tags_json)) {
        return NULL;
    }

    ExtractATagCtx ctx = { .a_tag_value = NULL };
    nostr_json_array_foreach_root(tags_json, extract_a_tag_cb, &ctx);

    return ctx.a_tag_value;
}

char *
gnostr_community_post_create_tags(const char *community_a_tag,
                                   const char *recommended_relay)
{
    if (!community_a_tag) return NULL;

    NostrJsonBuilder *builder = nostr_json_builder_new();
    nostr_json_builder_begin_array(builder);

    /* "a" tag for community reference */
    nostr_json_builder_begin_array(builder);
    nostr_json_builder_add_string(builder, "a");
    nostr_json_builder_add_string(builder, community_a_tag);
    if (recommended_relay)
        nostr_json_builder_add_string(builder, recommended_relay);
    nostr_json_builder_end_array(builder);

    nostr_json_builder_end_array(builder);
    char *result = nostr_json_builder_finish(builder);
    nostr_json_builder_free(builder);

    return result;
}

gboolean
gnostr_parse_a_tag(const char *a_tag,
                    guint *out_kind,
                    char **out_pubkey,
                    char **out_d_tag)
{
    if (!a_tag || !out_kind || !out_pubkey || !out_d_tag) return FALSE;

    *out_kind = 0;
    *out_pubkey = NULL;
    *out_d_tag = NULL;

    /* Parse format: kind:pubkey:d-tag */
    gchar **parts = g_strsplit(a_tag, ":", 3);
    if (!parts) return FALSE;

    guint len = g_strv_length(parts);
    if (len < 3) {
        g_strfreev(parts);
        return FALSE;
    }

    /* Parse kind */
    char *endptr;
    gulong kind = strtoul(parts[0], &endptr, 10);
    if (*endptr != '\0' || kind > G_MAXUINT) {
        g_strfreev(parts);
        return FALSE;
    }

    *out_kind = (guint)kind;
    *out_pubkey = g_strdup(parts[1]);
    *out_d_tag = g_strdup(parts[2]);

    g_strfreev(parts);
    return TRUE;
}
