#ifndef GH_LINK_POLICY_H
#define GH_LINK_POLICY_H

#include <glib.h>
#include <nostr-gtk-1.0/gn-markdown.h>

G_BEGIN_DECLS

/* Which text in a message becomes a link, and what a click on one may do
 * (privacy charter PD-3, §2.1, PT-3). GTK-free; nothing here fetches.
 *
 *  - Only raw http:// and https:// text and NIP-21 nostr: URIs (npub,
 *    nprofile, note, nevent, naddr) are links. A link's text is always the
 *    address itself (there is no anchor text) and all other text is escaped,
 *    so markup in a message renders literally.
 *  - javascript:, data:, file:, every other scheme, nostr:nsec (a secret key
 *    is never a link) and malformed addresses stay plain text.
 *  - A click opens nothing by itself: gh_link_policy_classify() says whether
 *    an https address with a plain ASCII host may open, or whether the full
 *    address, with its host in ASCII (punycode), must be confirmed first
 *    (http:, a non-ASCII or punycode host, or a user name before the host as
 *    in "https://bank.example@evil.example").
 */

typedef enum {
  GH_LINK_KIND_WEB,  /* http:// or https:// */
  GH_LINK_KIND_NOSTR /* nostr:npub1…, nprofile1…, note1…, nevent1…, naddr1… */
} GhLinkKind;

typedef struct {
  gsize start; /* byte offsets of the link in the text */
  gsize end;
  GhLinkKind kind;
  gchar *uri;  /* the link's text, exactly as written */
} GhLink;

void gh_link_free(GhLink *link);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GhLink, gh_link_free)

/* Every link of @text (UTF-8), in order and never overlapping. Invalid UTF-8
 * has no links. */
GPtrArray *gh_link_policy_find_links(const gchar *text);

/* Pango markup for @text: every character escaped, each link an
 * <a href="URI">URI</a>. Invalid UTF-8 is shown with replacement
 * characters and no links. */
gchar *gh_link_policy_to_markup(const gchar *text);
/* Like the above, but valid nostr:npub mentions show cached display names;
 * mentions of @account_pubkey are bold. The original URI remains the link. */
gchar *gh_link_policy_to_mention_markup(const gchar *text, const gchar *account_pubkey);

/* Formats bounded portable Markdown tokens without fetching or embedding
 * markup from the message. A Markdown label is never itself a link: its
 * literal target is shown separately and only linked if this policy accepts
 * that raw address. The document can be parsed off the UI thread. */
gchar *gh_link_policy_format_markdown(const GnMarkdownDocument *document,
                                      const gchar *account_pubkey);
gchar *gh_link_policy_to_markdown_markup(const gchar *text,
                                         const gchar *account_pubkey);

typedef enum {
  GH_LINK_ACTION_REFUSE,  /* not an address Groundhog opens */
  GH_LINK_ACTION_OPEN,    /* https with a plain ASCII host: open on click */
  GH_LINK_ACTION_CONFIRM, /* show the full address first (see reasons) */
  GH_LINK_ACTION_NOSTR    /* a nostr: URI, handled inside Groundhog */
} GhLinkAction;

typedef enum {
  GH_LINK_CONFIRM_NONE = 0,
  GH_LINK_CONFIRM_INSECURE = 1 << 0, /* http: is readable and changeable in transit */
  GH_LINK_CONFIRM_IDN = 1 << 1,      /* non-ASCII or punycode host: look-alike letters */
  GH_LINK_CONFIRM_USER_INFO = 1 << 2 /* "name@" before the real host */
} GhLinkConfirmReasons;

/* What a click on @uri may do. For OPEN and CONFIRM, @open_uri (nullable)
 * receives the address to open: the same address with a lowercase scheme,
 * its host in lowercase ASCII (punycode) and any other non-ASCII text
 * percent-encoded. It is also what a confirmation shows. @reasons (nullable)
 * receives why CONFIRM is needed (NONE otherwise). */
GhLinkAction gh_link_policy_classify(const gchar *uri, GhLinkConfirmReasons *reasons,
                                     gchar **open_uri);

/* The lowercase ASCII (punycode) host of an http(s) address, or NULL. */
gchar *gh_link_policy_dup_host(const gchar *uri);

/* Whether a preview may be offered for @uri: https with a valid host
 * (charter §2.1 fetches previews over https only). */
gboolean gh_link_policy_can_preview(const gchar *uri);

/* The first link of @text that can be previewed, or NULL. */
gchar *gh_link_policy_dup_preview_uri(const gchar *text);

G_END_DECLS
#endif
