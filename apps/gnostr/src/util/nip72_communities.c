/**
 * NIP-72 Moderated Communities Implementation
 */

#include "nip72_communities.h"
#include <jansson.h>
#include <string.h>

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

gboolean
gnostr_community_parse_tags(const char *tags_json, GnostrCommunity *community)
{
    if (!tags_json || !community) return FALSE;

    json_error_t error;
    json_t *tags = json_loads(tags_json, 0, &error);
    if (!tags || !json_is_array(tags)) {
        if (tags) json_decref(tags);
        return FALSE;
    }

    size_t index;
    json_t *tag;

    json_array_foreach(tags, index, tag) {
        if (!json_is_array(tag) || json_array_size(tag) < 2)
            continue;

        const char *tag_name = json_string_value(json_array_get(tag, 0));
        const char *tag_value = json_string_value(json_array_get(tag, 1));

        if (!tag_name || !tag_value)
            continue;

        if (strcmp(tag_name, "d") == 0) {
            /* Community identifier - also used as name if no explicit name */
            g_free(community->d_tag);
            community->d_tag = g_strdup(tag_value);
            if (!community->name) {
                community->name = g_strdup(tag_value);
            }
        } else if (strcmp(tag_name, "name") == 0) {
            /* Explicit community name */
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
            /* Check for moderator role */
            const char *relay_hint = NULL;
            const char *role = NULL;
            const char *petname = NULL;

            if (json_array_size(tag) >= 3)
                relay_hint = json_string_value(json_array_get(tag, 2));
            if (json_array_size(tag) >= 4)
                role = json_string_value(json_array_get(tag, 3));
            if (json_array_size(tag) >= 5)
                petname = json_string_value(json_array_get(tag, 4));

            /* Per NIP-72: p tag with "moderator" role indicates a moderator */
            if (role && strcmp(role, "moderator") == 0) {
                GnostrCommunityModerator *mod = gnostr_community_moderator_new();
                mod->pubkey = g_strdup(tag_value);
                mod->relay_hint = g_strdup(relay_hint);
                mod->petname = g_strdup(petname);
                g_ptr_array_add(community->moderators, mod);
            }
        }
    }

    json_decref(tags);
    return (community->d_tag != NULL);
}

char *
gnostr_community_create_tags(const GnostrCommunity *community)
{
    if (!community || !community->d_tag) return NULL;

    json_t *tags = json_array();

    /* d tag (required) */
    json_t *d_tag = json_array();
    json_array_append_new(d_tag, json_string("d"));
    json_array_append_new(d_tag, json_string(community->d_tag));
    json_array_append_new(tags, d_tag);

    /* name tag (if different from d tag) */
    if (community->name && strcmp(community->name, community->d_tag) != 0) {
        json_t *name_tag = json_array();
        json_array_append_new(name_tag, json_string("name"));
        json_array_append_new(name_tag, json_string(community->name));
        json_array_append_new(tags, name_tag);
    }

    /* description tag */
    if (community->description) {
        json_t *desc_tag = json_array();
        json_array_append_new(desc_tag, json_string("description"));
        json_array_append_new(desc_tag, json_string(community->description));
        json_array_append_new(tags, desc_tag);
    }

    /* image tag */
    if (community->image) {
        json_t *img_tag = json_array();
        json_array_append_new(img_tag, json_string("image"));
        json_array_append_new(img_tag, json_string(community->image));
        json_array_append_new(tags, img_tag);
    }

    /* rules tag */
    if (community->rules) {
        json_t *rules_tag = json_array();
        json_array_append_new(rules_tag, json_string("rules"));
        json_array_append_new(rules_tag, json_string(community->rules));
        json_array_append_new(tags, rules_tag);
    }

    /* moderator p tags */
    for (guint i = 0; i < community->moderators->len; i++) {
        GnostrCommunityModerator *mod = g_ptr_array_index(community->moderators, i);
        if (!mod->pubkey) continue;

        json_t *p_tag = json_array();
        json_array_append_new(p_tag, json_string("p"));
        json_array_append_new(p_tag, json_string(mod->pubkey));
        json_array_append_new(p_tag, json_string(mod->relay_hint ? mod->relay_hint : ""));
        json_array_append_new(p_tag, json_string("moderator"));
        if (mod->petname) {
            json_array_append_new(p_tag, json_string(mod->petname));
        }
        json_array_append_new(tags, p_tag);
    }

    char *result = json_dumps(tags, JSON_COMPACT);
    json_decref(tags);

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

gboolean
gnostr_approved_post_parse_tags(const char *tags_json, GnostrApprovedPost *approval)
{
    if (!tags_json || !approval) return FALSE;

    json_error_t error;
    json_t *tags = json_loads(tags_json, 0, &error);
    if (!tags || !json_is_array(tags)) {
        if (tags) json_decref(tags);
        return FALSE;
    }

    size_t index;
    json_t *tag;

    json_array_foreach(tags, index, tag) {
        if (!json_is_array(tag) || json_array_size(tag) < 2)
            continue;

        const char *tag_name = json_string_value(json_array_get(tag, 0));
        const char *tag_value = json_string_value(json_array_get(tag, 1));

        if (!tag_name || !tag_value)
            continue;

        if (strcmp(tag_name, "a") == 0) {
            /* Community reference */
            g_free(approval->community_a_tag);
            approval->community_a_tag = g_strdup(tag_value);
        } else if (strcmp(tag_name, "e") == 0) {
            /* Approved post reference */
            g_free(approval->post_event_id);
            approval->post_event_id = g_strdup(tag_value);
        } else if (strcmp(tag_name, "p") == 0) {
            /* Post author */
            g_free(approval->post_author);
            approval->post_author = g_strdup(tag_value);
        }
    }

    json_decref(tags);
    return (approval->post_event_id != NULL && approval->community_a_tag != NULL);
}

char *
gnostr_approved_post_create_tags(const char *community_a_tag,
                                  const char *post_event_id,
                                  const char *post_author,
                                  const char *recommended_relay)
{
    if (!community_a_tag || !post_event_id || !post_author) return NULL;

    json_t *tags = json_array();

    /* "a" tag for community reference */
    json_t *a_tag = json_array();
    json_array_append_new(a_tag, json_string("a"));
    json_array_append_new(a_tag, json_string(community_a_tag));
    if (recommended_relay)
        json_array_append_new(a_tag, json_string(recommended_relay));
    json_array_append_new(tags, a_tag);

    /* "e" tag for approved post */
    json_t *e_tag = json_array();
    json_array_append_new(e_tag, json_string("e"));
    json_array_append_new(e_tag, json_string(post_event_id));
    if (recommended_relay)
        json_array_append_new(e_tag, json_string(recommended_relay));
    json_array_append_new(tags, e_tag);

    /* "p" tag for post author */
    json_t *p_tag = json_array();
    json_array_append_new(p_tag, json_string("p"));
    json_array_append_new(p_tag, json_string(post_author));
    if (recommended_relay)
        json_array_append_new(p_tag, json_string(recommended_relay));
    json_array_append_new(tags, p_tag);

    /* "k" tag for the kind of the approved post (kind 1 for notes) */
    json_t *k_tag = json_array();
    json_array_append_new(k_tag, json_string("k"));
    json_array_append_new(k_tag, json_string("1"));
    json_array_append_new(tags, k_tag);

    char *result = json_dumps(tags, JSON_COMPACT);
    json_decref(tags);

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

char *
gnostr_community_post_extract_a_tag(const char *tags_json)
{
    if (!tags_json) return NULL;

    json_error_t error;
    json_t *tags = json_loads(tags_json, 0, &error);
    if (!tags || !json_is_array(tags)) {
        if (tags) json_decref(tags);
        return NULL;
    }

    char *a_tag_value = NULL;
    size_t index;
    json_t *tag;

    json_array_foreach(tags, index, tag) {
        if (!json_is_array(tag) || json_array_size(tag) < 2)
            continue;

        const char *tag_name = json_string_value(json_array_get(tag, 0));
        if (!tag_name || strcmp(tag_name, "a") != 0)
            continue;

        const char *tag_value = json_string_value(json_array_get(tag, 1));
        if (!tag_value)
            continue;

        /* Check if this is a community reference (kind 34550) */
        if (strncmp(tag_value, "34550:", 6) == 0) {
            a_tag_value = g_strdup(tag_value);
            break;
        }
    }

    json_decref(tags);
    return a_tag_value;
}

char *
gnostr_community_post_create_tags(const char *community_a_tag,
                                   const char *recommended_relay)
{
    if (!community_a_tag) return NULL;

    json_t *tags = json_array();

    /* "a" tag for community reference */
    json_t *a_tag = json_array();
    json_array_append_new(a_tag, json_string("a"));
    json_array_append_new(a_tag, json_string(community_a_tag));
    if (recommended_relay)
        json_array_append_new(a_tag, json_string(recommended_relay));
    json_array_append_new(tags, a_tag);

    char *result = json_dumps(tags, JSON_COMPACT);
    json_decref(tags);

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
