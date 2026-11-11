#include "gh-call-ice-policy.h"
#include <gio/gio.h>
#include <string.h>

gboolean gh_call_ice_address_allowed(const gchar *text)
{
  /* No DNS resolution: this also rejects mDNS names and scoped IPv6 literals. */
  g_autoptr(GInetAddress) address = g_inet_address_new_from_string(text);
  if (!address || strchr(text, '%')) return FALSE;
  if (g_inet_address_get_is_any(address) || g_inet_address_get_is_loopback(address) ||
      g_inet_address_get_is_link_local(address) || g_inet_address_get_is_site_local(address) ||
      g_inet_address_get_is_multicast(address)) return FALSE;
  const guint8 *b = g_inet_address_to_bytes(address);
  if (g_inet_address_get_family(address) == G_SOCKET_FAMILY_IPV4)
    return !(b[0] == 0 || b[0] >= 224 || b[0] == 10 || b[0] == 127 ||
        (b[0] == 169 && b[1] == 254) || (b[0] == 172 && (b[1] & 0xf0) == 16) ||
        (b[0] == 192 && b[1] == 168) || (b[0] == 100 && (b[1] & 0xc0) == 64) ||
        (b[0] == 192 && b[1] == 0 && b[2] == 0) || (b[0] == 198 && (b[1] & 0xfe) == 18));
  /* Only native global unicast. Conservatively exclude transition/special-use
   * prefixes (including embedded IPv4), rather than permit a private target
   * through an address translation mechanism whose prefix we cannot know. */
  return (b[0] & 0xe0) == 0x20 &&
      !(b[0] == 0x20 && b[1] == 0x02) &&
      !(b[0] == 0x20 && b[1] == 0x01 && (b[2] < 2 || (b[2] == 0x0d && b[3] == 0xb8)));
}

static gchar **candidate_fields(const gchar *candidate)
{
  if (!candidate || strlen(candidate) > 4096 || strchr(candidate, '\r') || strchr(candidate, '\n')) return NULL;
  const gchar *p = g_str_has_prefix(candidate, "a=") ? candidate + 2 : candidate;
  if (!g_str_has_prefix(p, "candidate:")) return NULL;
  gchar **fields = g_strsplit(p, " ", -1);
  guint n = g_strv_length(fields);
  if (n < 8 || (n - 8) % 2 || strcmp(fields[6], "typ")) goto invalid;
  for (guint i = 0; i < n; i++)
    if (!*fields[i] || strpbrk(fields[i], "\t\v\f")) goto invalid;
  return fields;
invalid:
  g_strfreev(fields);
  return NULL;
}

gboolean gh_call_ice_candidate_allowed(const gchar *candidate, gboolean allow_local, gboolean relay_only)
{
  g_auto(GStrv) f = candidate_fields(candidate);
  if (!f) return FALSE;
  const gchar *type = f[7];
  if (relay_only && strcmp(type, "relay")) return FALSE;
  if (strcmp(type, "host") && strcmp(type, "srflx") && strcmp(type, "prflx") && strcmp(type, "relay")) return FALSE;
  return allow_local || (strcmp(type, "host") && gh_call_ice_address_allowed(f[4]));
}

gchar *gh_call_ice_candidate_for_peer(const gchar *candidate, gboolean allow_local, gboolean relay_only)
{
  if (!gh_call_ice_candidate_allowed(candidate, allow_local, relay_only)) return NULL;
  g_auto(GStrv) f = candidate_fields(candidate);
  if (!allow_local) {
    for (guint i = 8; f[i]; i += 2) {
      /* Scrub every occurrence, including a duplicate supplied by a plugin. */
      if (!strcmp(f[i], "raddr") || !strcmp(f[i], "rport")) {
        g_free(f[i + 1]);
        f[i + 1] = g_strdup(!strcmp(f[i], "raddr") ? "0.0.0.0" : "0");
      }
    }
  }
  return g_strjoinv(" ", f);
}

gchar *gh_call_ice_sdp_for_peer(const GstSDPMessage *sdp, gboolean allow_local, gboolean relay_only)
{
  g_autofree gchar *raw = gst_sdp_message_as_text(sdp);
  g_auto(GStrv) lines = g_strsplit(raw, "\r\n", -1);
  GString *wire = g_string_new(NULL);
  for (guint i = 0; lines[i]; i++) {
    const gchar *line = lines[i];
    if (!*line) continue;
    g_autofree gchar *replacement = NULL;
    if (g_str_has_prefix(line, "a=candidate:")) {
      g_autofree gchar *candidate = gh_call_ice_candidate_for_peer(line + 2, allow_local, relay_only);
      if (!candidate) continue;
      replacement = g_strconcat("a=", candidate, NULL);
    } else if (!allow_local && g_str_has_prefix(line, "c=")) {
      replacement = g_strdup("c=IN IP4 0.0.0.0");
    } else if (!allow_local && g_str_has_prefix(line, "o=")) {
      g_auto(GStrv) f = g_strsplit(line, " ", -1);
      if (g_strv_length(f) == 6)
        replacement = g_strdup_printf("%s %s %s IN IP4 0.0.0.0", f[0], f[1], f[2]);
      else continue;
    } else if (!allow_local && g_str_has_prefix(line, "a=rtcp:")) {
      replacement = g_strdup("a=rtcp:9 IN IP4 0.0.0.0");
    } else if (!allow_local && g_str_has_prefix(line, "a=remote-candidates:")) {
      continue;
    }
    g_string_append_printf(wire, "%s\r\n", replacement ? replacement : line);
  }
  return g_string_free(wire, FALSE);
}
