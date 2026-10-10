/* Open Graph metadata and the shared <head> parser (nostrc-8xfib.3).
 * Moved from Groundhog's gh_web_result_parse_html (libxml2, head only,
 * NONET, ranked fields) plus gnostr's twitter:image fallback and relative
 * image resolution. Replaces gnostr's strstr parser. */
#include <nostr-gtk-1.0/gn-og-preview.h>
#include <libxml/HTMLparser.h>
#include <string.h>

#define GN_OG_FIELD_MAX_CHARS 512

struct _GnOgMetadata {
  gatomicrefcount ref_count;
  char *source_url, *title, *description, *site_name, *image_url;
};

G_DEFINE_BOXED_TYPE(GnOgMetadata, gn_og_metadata, gn_og_metadata_ref, gn_og_metadata_unref)

GnOgMetadata *
gn_og_metadata_new(const char *source_url, const char *title, const char *description,
                   const char *site_name, const char *image_url)
{
  GnOgMetadata *m = g_new0(GnOgMetadata, 1);
  g_atomic_ref_count_init(&m->ref_count);
  m->source_url = g_strdup(source_url);
  m->title = g_strdup(title);
  m->description = g_strdup(description);
  m->site_name = g_strdup(site_name);
  m->image_url = g_strdup(image_url);
  return m;
}

GnOgMetadata *
gn_og_metadata_ref(GnOgMetadata *m)
{
  g_return_val_if_fail(m != NULL, NULL);
  g_atomic_ref_count_inc(&m->ref_count);
  return m;
}

void
gn_og_metadata_unref(GnOgMetadata *m)
{
  if (!m || !g_atomic_ref_count_dec(&m->ref_count)) return;
  g_free(m->source_url);
  g_free(m->title);
  g_free(m->description);
  g_free(m->site_name);
  g_free(m->image_url);
  g_free(m);
}

const char *gn_og_metadata_get_source_url(const GnOgMetadata *m) { return m ? m->source_url : NULL; }
const char *gn_og_metadata_get_title(const GnOgMetadata *m) { return m ? m->title : NULL; }
const char *gn_og_metadata_get_description(const GnOgMetadata *m) { return m ? m->description : NULL; }
const char *gn_og_metadata_get_site_name(const GnOgMetadata *m) { return m ? m->site_name : NULL; }
const char *gn_og_metadata_get_image_url(const GnOgMetadata *m) { return m ? m->image_url : NULL; }

static gchar *
bounded_text(const xmlChar *text)
{
  if (!text) return NULL;
  g_autofree gchar *valid = g_utf8_make_valid((const gchar *)text, -1);
  return g_utf8_substring(valid, 0, MIN(g_utf8_strlen(valid, -1), GN_OG_FIELD_MAX_CHARS));
}

/* Precedence among the head's names for one field: og: wins over twitter:,
 * which wins over the plain HTML one, whatever the document order. */
static void
take_field(gchar **dest, guint *rank, guint new_rank, const xmlChar *value)
{
  if (!value || (*dest && new_rank <= *rank)) return;
  g_autofree gchar *text = bounded_text(value);
  g_strstrip(text);
  if (!*text) return;
  g_free(*dest);
  *dest = g_steal_pointer(&text);
  *rank = new_rank;
}

static gboolean
default_image_policy(const char *url)
{
  return g_ascii_strncasecmp(url, "https://", 8) == 0 ||
         g_ascii_strncasecmp(url, "http://", 7) == 0;
}

/* og:image (rank 2) over twitter:image (rank 1); a relative address resolves
 * against the page, then the policy decides. */
static void
take_image(gchar **dest, guint *rank, guint new_rank, const xmlChar *value,
           const char *source_url, GnOgImagePolicy accept, gpointer data)
{
  if (!value || (*dest && new_rank <= *rank)) return;
  g_autofree gchar *candidate = bounded_text(value);
  if (!candidate) return;
  g_strstrip(candidate);
  if (!*candidate) return;
  if (source_url && *source_url) {
    gchar *absolute = g_uri_resolve_relative(source_url, candidate, G_URI_FLAGS_PARSE_RELAXED, NULL);
    if (absolute) { g_free(candidate); candidate = absolute; }
  }
  if (!(accept ? accept(candidate, data) : default_image_policy(candidate))) return;
  g_free(*dest);
  *dest = g_steal_pointer(&candidate);
  *rank = new_rank;
}

typedef struct {
  gchar *title, *description, *site, *image;
  guint title_rank, description_rank, site_rank, image_rank;
  const char *source_url;
  GnOgImagePolicy accept;
  gpointer data;
} ParseState;

static void
take_meta(ParseState *s, xmlNode *node)
{
  xmlChar *property = xmlGetProp(node, BAD_CAST "property");
  if (!property) property = xmlGetProp(node, BAD_CAST "name");
  xmlChar *value = xmlGetProp(node, BAD_CAST "content");
  if (property && value) {
    const xmlChar *p = property;
    if (!xmlStrcasecmp(p, BAD_CAST "og:image"))
      take_image(&s->image, &s->image_rank, 2, value, s->source_url, s->accept, s->data);
    else if (!xmlStrcasecmp(p, BAD_CAST "twitter:image"))
      take_image(&s->image, &s->image_rank, 1, value, s->source_url, s->accept, s->data);
    else if (!xmlStrcasecmp(p, BAD_CAST "og:title"))
      take_field(&s->title, &s->title_rank, 3, value);
    else if (!xmlStrcasecmp(p, BAD_CAST "twitter:title"))
      take_field(&s->title, &s->title_rank, 2, value);
    else if (!xmlStrcasecmp(p, BAD_CAST "og:description"))
      take_field(&s->description, &s->description_rank, 3, value);
    else if (!xmlStrcasecmp(p, BAD_CAST "twitter:description"))
      take_field(&s->description, &s->description_rank, 2, value);
    else if (!xmlStrcasecmp(p, BAD_CAST "description"))
      take_field(&s->description, &s->description_rank, 1, value);
    else if (!xmlStrcasecmp(p, BAD_CAST "og:site_name"))
      take_field(&s->site, &s->site_rank, 3, value);
  }
  xmlFree(property);
  xmlFree(value);
}

GnOgMetadata *
gn_og_metadata_parse_html(GBytes *bytes, const char *source_url, GnOgImagePolicy accept_image,
                          gpointer user_data, GError **error)
{
  g_return_val_if_fail(bytes != NULL, NULL);
  ParseState s = { .source_url = source_url, .accept = accept_image, .data = user_data };
  gsize size = 0;
  const gchar *body = g_bytes_get_data(bytes, &size);
  /* HTML parsing never runs scripts, resolves entities, or loads any linked
   * resource. Only head text is retained; the image address is never
   * fetched here. The page may be cut short: the parser recovers, and the
   * head is what matters. */
  htmlDocPtr doc = size ? htmlReadMemory(body, (int)MIN(size, (gsize)G_MAXINT), NULL, NULL,
                                         HTML_PARSE_NONET | HTML_PARSE_NOERROR |
                                         HTML_PARSE_NOWARNING | HTML_PARSE_RECOVER)
                        : NULL;
  if (doc) {
    xmlNode *html = xmlDocGetRootElement(doc);
    for (xmlNode *head = html ? html->children : NULL; head; head = head->next) {
      if (head->type != XML_ELEMENT_NODE || xmlStrcasecmp(head->name, BAD_CAST "head")) continue;
      for (xmlNode *node = head->children; node; node = node->next) {
        if (node->type != XML_ELEMENT_NODE) continue;
        if (!xmlStrcasecmp(node->name, BAD_CAST "title")) {
          xmlChar *value = xmlNodeGetContent(node);
          take_field(&s.title, &s.title_rank, 1, value);
          xmlFree(value);
        } else if (!xmlStrcasecmp(node->name, BAD_CAST "meta")) {
          take_meta(&s, node);
        }
      }
      break;
    }
    xmlFreeDoc(doc);
  }
  GnOgMetadata *result = NULL;
  if (!s.title && !s.description)
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "No preview text in this page");
  else
    result = gn_og_metadata_new(source_url, s.title, s.description, s.site, s.image);
  g_free(s.title);
  g_free(s.description);
  g_free(s.site);
  g_free(s.image);
  return result;
}
