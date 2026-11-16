/* The probe client: start, poll while the daemon works, render the stage
 * block as it fills in, then the path, cleanup and verdict (upstream
 * fipsctl.rs run_probe and the probe rendering below it). Every string the
 * operator reads is upstream's, verbatim. */
#include "fipsctl.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define POLL_INTERVAL_MS 250u
static const char *const SPINNER[10] = {"\xe2\xa0\x8b", "\xe2\xa0\x99", "\xe2\xa0\xb9", "\xe2\xa0\xb8", "\xe2\xa0\xbc",
                                        "\xe2\xa0\xb4", "\xe2\xa0\xa6", "\xe2\xa0\xa7", "\xe2\xa0\x87", "\xe2\xa0\x8f"};
static const char *const STAGES[5] = {"bloom", "discovery", "path", "session", "rtt"};
enum { IND = 2, NAME_W = 9, VERDICT_W = 7, ELAPSED_W = 6, TEXT_COL = IND + NAME_W + 1 + 1 + 1 + VERDICT_W + 1 + ELAPSED_W + 2 };

uint64_t fipsctl_probe_ceiling_ms(uint64_t budget_ms, bool has_timeout, uint64_t timeout_secs)
{
    if (has_timeout) return timeout_secs > UINT64_MAX / 1000 ? UINT64_MAX : timeout_secs * 1000;
    return budget_ms > UINT64_MAX - 5000 ? UINT64_MAX : budget_ms + 5000;
}

/* ---- field helpers -------------------------------------------------------- */

static const char *field(const json_t *v, const char *key)
{
    const char *s = json_string_value(json_object_get(v, key));
    return s ? s : "";
}

static bool as_u64(const json_t *v, const char *key, uint64_t *out)
{
    const json_t *f = json_object_get(v, key);
    if (!json_is_integer(f) || json_integer_value(f) < 0) return false;
    *out = (uint64_t)json_integer_value(f);
    return true;
}

static bool as_bool(const json_t *v, const char *key)
{
    const json_t *f = json_object_get(v, key);
    return json_is_true(f);
}

/* `a_b_c` -> `a b c`, freshly allocated. */
static char *unsnake(const char *s)
{
    char *out = strdup(s);
    if (!out) return NULL;
    for (char *p = out; *p; ++p) if (*p == '_') *p = ' ';
    return out;
}

/* First 8 characters of an address, the fipstop convention. */
static char *short_addr(const char *addr) { return fipsctl_take_chars(addr, 8); }

/* ---- strings ---------------------------------------------------------------- */

typedef struct lines { char **items; size_t count, cap; } lines;

static void lines_push(lines *l, char *item)
{
    if (!item) return;
    if (l->count == l->cap) {
        size_t cap = l->cap ? l->cap * 2 : 8;
        char **grown = realloc(l->items, cap * sizeof(*grown));
        if (!grown) { free(item); return; }
        l->items = grown; l->cap = cap;
    }
    l->items[l->count++] = item;
}

void fipsctl_lines_free(char **items, size_t count)
{
    for (size_t i = 0; i < count; ++i) free(items[i]);
    free(items);
}

static char *join(const char *const *parts, size_t count, const char *sep)
{
    size_t len = 1, sl = strlen(sep);
    for (size_t i = 0; i < count; ++i) len += strlen(parts[i]) + (i ? sl : 0);
    char *out = malloc(len);
    if (!out) return NULL;
    out[0] = 0;
    for (size_t i = 0; i < count; ++i) { if (i) strcat(out, sep); strcat(out, parts[i]); }
    return out;
}

static char *trim_end(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\n' || s[n - 1] == '\r')) s[--n] = 0;
    return s;
}

static char *replace_all(const char *s, const char *needle, const char *with)
{
    size_t nl = strlen(needle), wl = strlen(with), count = 0;
    for (const char *p = strstr(s, needle); p; p = strstr(p + nl, needle)) ++count;
    char *out = malloc(strlen(s) + count * (wl > nl ? wl - nl : 0) + 1);
    if (!out) return NULL;
    char *w = out;
    for (const char *p = s;;) {
        const char *hit = strstr(p, needle);
        if (!hit) { strcpy(w, p); break; }
        memcpy(w, p, (size_t)(hit - p)); w += hit - p;
        memcpy(w, with, wl); w += wl;
        p = hit + nl;
    }
    return out;
}

char *fipsctl_clip(const char *line, size_t width)
{
    if (fipsctl_utf8_chars(line) <= width) return strdup(line);
    char *head = fipsctl_take_chars(line, width ? width - 1 : 0);
    if (!head) return NULL;
    char *out = fipsctl_aprintf("%s\xe2\x80\xa6", head);
    free(head);
    return out;
}

/* Coordinates root-first, each entry shortened, ending in `marker`. */
char *fipsctl_render_coords(const json_t *coords, const char *marker)
{
    lines entries = {0};
    size_t i; json_t *v;
    if (json_is_array(coords)) json_array_foreach((json_t *)coords, i, v)
        if (json_is_string(v)) lines_push(&entries, short_addr(json_string_value(v)));
    char *out;
    if (!entries.count) out = strdup("-");
    else if (entries.count == 1) out = fipsctl_aprintf("%s [root]", entries.items[0]);
    else {
        for (size_t a = 0, b = entries.count - 1; a < b; ++a, --b) {
            char *t = entries.items[a]; entries.items[a] = entries.items[b]; entries.items[b] = t;
        }
        char *last = fipsctl_aprintf("%s [%s]", entries.items[entries.count - 1], marker);
        free(entries.items[entries.count - 1]);
        entries.items[entries.count - 1] = last;
        out = join((const char *const *)entries.items, entries.count, " > ");
    }
    fipsctl_lines_free(entries.items, entries.count);
    return out;
}

static char *bold(const char *text, bool tty)
{
    return tty ? fipsctl_aprintf("\x1b[1m%s\x1b[0m", text) : strdup(text);
}

static void short_coords(const json_t *path, const char *key, lines *out)
{
    const json_t *a = json_object_get(path, key);
    size_t i; json_t *v;
    if (json_is_array(a)) json_array_foreach((json_t *)a, i, v)
        if (json_is_string(v)) lines_push(out, short_addr(json_string_value(v)));
}

static bool position(const lines *l, const char *needle, size_t *at)
{
    for (size_t i = 0; i < l->count; ++i)
        if (!strcmp(l->items[i], needle)) { *at = i; return true; }
    return false;
}

/* This node to the target on one line, up to the least common ancestor
 * (emphasised on a terminal) and down the target's own coordinates. */
char *fipsctl_walk_line(const json_t *path, const char *lca_full, bool tty)
{
    char *lca = short_addr(lca_full);
    lines ours = {0}, theirs = {0}, chain = {0};
    short_coords(path, "our_coords", &ours);
    short_coords(path, "their_coords", &theirs);
    char *out = NULL;
    if (!ours.count || !lca) { out = strdup("-"); goto done; }
    size_t at;
    size_t climb = position(&ours, lca, &at) ? at + 1 : ours.count;
    for (size_t i = 0; i < climb; ++i) lines_push(&chain, strdup(ours.items[i]));
    size_t lca_at = chain.count - 1;
    size_t descend = position(&theirs, lca, &at) ? at : 0;
    for (size_t i = descend; i > 0; --i) lines_push(&chain, strdup(theirs.items[i - 1]));
    size_t last = chain.count - 1;
    lines rendered = {0};
    for (size_t i = 0; i < chain.count; ++i) {
        char *addr = i == lca_at ? bold(chain.items[i], tty) : strdup(chain.items[i]);
        if (!addr) continue;
        if (i == 0) lines_push(&rendered, fipsctl_aprintf("%s [self]", addr));
        else if (i == last) lines_push(&rendered, fipsctl_aprintf("%s [target]", addr));
        else lines_push(&rendered, strdup(addr));
        free(addr);
    }
    out = join((const char *const *)rendered.items, rendered.count, " > ");
    fipsctl_lines_free(rendered.items, rendered.count);
done:
    fipsctl_lines_free(ours.items, ours.count);
    fipsctl_lines_free(theirs.items, theirs.count);
    fipsctl_lines_free(chain.items, chain.count);
    free(lca);
    return out;
}

/* ---- stage text ------------------------------------------------------------- */

static char *rtt_text(const json_t *rtt)
{
    uint64_t reports = 0, ms;
    (void)as_u64(rtt, "reports_seen", &reports);
    const char *reason = field(rtt, "reason");
    if (!reason[0]) {
        if (!as_u64(rtt, "rtt_ms", &ms)) return strdup("measured");
        if (reports == 1) return fipsctl_aprintf("%llu ms round trip", (unsigned long long)ms);
        return fipsctl_aprintf("%llu ms round trip (%llu report exchanges)", (unsigned long long)ms, (unsigned long long)reports);
    }
    if (!strcmp(reason, "no_report")) return strdup("no receiver report of any kind within the budget");
    if (!strcmp(reason, "no_echo")) return strdup("reports arrived but none carried a usable timestamp echo");
    if (!strcmp(reason, "sub_millisecond"))
        return fipsctl_aprintf("reports arrived but every sample measured under 1 ms (%llu reports) "
                               "-- expected on loopback and same-host meshes", (unsigned long long)reports);
    if (!strcmp(reason, "bad_timestamp_echo"))
        return fipsctl_aprintf("receiver reports carried timestamp echoes that failed arithmetic "
                               "validation (%llu reports). The echo was stale, bogus, or the "
                               "session clock wrapped. Worth investigating.", (unsigned long long)reports);
    if (!strcmp(reason, "not_reached")) return strdup("not reached");
    return strdup(reason);
}

static char *settled_text(const char *name, const json_t *stage)
{
    const char *reason = field(stage, "reason");
    uint64_t n;
    if (!strcmp(name, "bloom")) {
        if (!reason[0]) {
            if (as_u64(stage, "fanout", &n) && n > 1) return fipsctl_aprintf("claimed by a peer filter, %llu peers asked", (unsigned long long)n);
            return strdup("claimed by a peer filter");
        }
        if (!strcmp(reason, "cached")) return strdup("coordinates already cached");
        if (!strcmp(reason, "direct_peer")) return strdup("target is a directly connected peer");
        if (!strcmp(reason, "bloom_miss")) return strdup("no peer filter holds this address");
        if (!strcmp(reason, "backoff_suppressed")) return strdup("held down by lookup backoff");
        if (!strcmp(reason, "no_tree_peers")) return strdup("claimed, but no tree peer to ask");
        return unsnake(reason);
    }
    if (!strcmp(name, "discovery")) {
        if (!reason[0]) return strdup("coordinates from mesh lookup");
        if (!strcmp(reason, "cached") || !strcmp(reason, "direct_peer")) return strdup("no lookup needed");
        if (!strcmp(reason, "no_response")) {
            if (!as_u64(stage, "attempts", &n)) return strdup("no reply");
            if (n == 1) return strdup("no reply to 1 request");
            return fipsctl_aprintf("no reply to %llu requests", (unsigned long long)n);
        }
        if (!strcmp(reason, "bloom_unconfirmed")) {
            if (!as_u64(stage, "attempts", &n)) return strdup("a peer filter claimed this address; nothing answered for it");
            if (n == 1) return strdup("a peer filter claimed this address; 1 request went unanswered");
            return fipsctl_aprintf("a peer filter claimed this address; %llu requests went unanswered", (unsigned long long)n);
        }
        if (!strcmp(reason, "already_pending")) return strdup("joined a lookup already in flight, which failed");
        return unsnake(reason);
    }
    if (!strcmp(name, "path")) {
        if (!reason[0]) return strdup("computed locally");
        if (!strcmp(reason, "disjoint_trees")) return strdup("different spanning-tree roots");
        if (!strcmp(reason, "no_next_hop")) return strdup("no next hop toward this target");
        if (!strcmp(reason, "direct_peer")) return strdup("target is a directly connected peer");
        return unsnake(reason);
    }
    if (!strcmp(name, "session")) {
        if (!reason[0]) return strdup("established (XK handshake)");
        if (!strcmp(reason, "preexisting")) return strdup("a session to this endpoint already existed");
        if (!strcmp(reason, "send_error")) return fipsctl_aprintf("could not send: %s", field(stage, "detail"));
        if (!strcmp(reason, "handshake_timeout")) return strdup("gave up waiting for the handshake to complete");
        return unsnake(reason);
    }
    return rtt_text(stage);
}

/* Only what the daemon has observed; a running row never previews an outcome. */
static char *running_text(const char *name, const json_t *stage)
{
    uint64_t n;
    if (!strcmp(name, "bloom")) return strdup("asking the peer filters");
    if (!strcmp(name, "discovery"))
        return strdup(as_u64(stage, "attempts", &n) ? "waiting for a reply" : "sending the first request");
    if (!strcmp(name, "path")) return strdup("computing from coordinates");
    if (!strcmp(name, "session")) return strdup("XK handshake in flight");
    if (!as_u64(stage, "reports_seen", &n)) n = 0;
    if (n == 0) return strdup("waiting for a receiver report");
    if (n == 1) return strdup("waiting for a usable timestamp echo (1 report so far)");
    return fipsctl_aprintf("waiting for a usable timestamp echo (%llu reports so far)", (unsigned long long)n);
}

/* One line per LookupRequest sent; the parenthesised figure is the configured
 * timeout of that attempt, not a measurement. */
static void attempt_lines(const json_t *stage, const char *verdict, lines *out)
{
    uint64_t attempts;
    if (!as_u64(stage, "attempts", &attempts)) return;
    const json_t *ladder = json_object_get(stage, "attempt_timeouts_secs");
    for (uint64_t n = 1; n <= attempts; ++n) {
        bool last = n == attempts;
        const char *status = last && !strcmp(verdict, "ok") ? "replied"
                           : last && !strcmp(verdict, "running") ? "waiting" : "no reply";
        char timeout[32] = "";
        const json_t *secs = json_is_array(ladder) ? json_array_get(ladder, (size_t)(n - 1)) : NULL;
        if (json_is_integer(secs) && json_integer_value(secs) >= 0)
            snprintf(timeout, sizeof(timeout), "(%llus)", (unsigned long long)json_integer_value(secs));
        const char *mark = last && !strcmp(verdict, "running") ? FIPSCTL_SPIN_HERE : " ";
        char *line = fipsctl_aprintf("attempt %-3llu%s %-9s %s", (unsigned long long)n, mark, status, timeout);
        if (line) lines_push(out, trim_end(line));
    }
}

/* ---- rows and the block ------------------------------------------------------ */

static void row_free(fipsctl_stage_row *r)
{
    free(r->verdict); free(r->reason); free(r->text);
    fipsctl_lines_free(r->subs, r->sub_count);
}

void fipsctl_stage_rows_free(fipsctl_stage_row *rows, size_t count)
{
    for (size_t i = 0; i < count; ++i) row_free(&rows[i]);
    free(rows);
}

size_t fipsctl_stage_rows(const json_t *report, fipsctl_stage_row **out)
{
    uint64_t total_ms = 0, accounted = 0, ms;
    (void)as_u64(report, "elapsed_ms", &total_ms);
    fipsctl_stage_row *rows = calloc(5, sizeof(*rows));
    if (!rows) { *out = NULL; return 0; }
    size_t count = 0;
    for (size_t i = 0; i < 5; ++i) {
        const char *name = STAGES[i];
        const json_t *stage = json_object_get(report, name);
        const char *verdict = field(stage, "verdict");
        bool settled = !strcmp(verdict, "ok") || !strcmp(verdict, "failed") || !strcmp(verdict, "skipped");
        fipsctl_stage_row *r = &rows[count++];
        r->name = name;
        r->verdict = strdup(verdict);
        r->reason = strdup(field(stage, "reason"));
        r->text = settled ? settled_text(name, stage) : !strcmp(verdict, "running") ? running_text(name, stage) : strdup("");
        if (as_u64(stage, "elapsed_ms", &ms)) {
            accounted = accounted > UINT64_MAX - ms ? UINT64_MAX : accounted + ms;
            r->has_elapsed = true; r->elapsed_ms = ms;
        } else if (!strcmp(verdict, "running")) {
            r->has_elapsed = true; r->elapsed_ms = total_ms > accounted ? total_ms - accounted : 0;
        }
        r->settled = settled;
        if (!strcmp(name, "discovery")) {
            lines subs = {0};
            attempt_lines(stage, verdict, &subs);
            r->subs = subs.items; r->sub_count = subs.count;
        }
        /* Hidden while pending too: a successful path stage restates the path
         * section's heading, and a row shown then withdrawn would be all the
         * viewer saw of it. */
        r->hidden = !strcmp(name, "path") && strcmp(verdict, "failed") && strcmp(verdict, "skipped");
    }
    /* Drop the stages a failure left unattempted; a skip and anything after a
     * failed path preview are findings and stay. */
    while (count > 1 && rows[count - 1].reason && !strcmp(rows[count - 1].reason, "not_reached"))
        row_free(&rows[--count]);
    *out = rows;
    return count;
}

/* `name [spinner] verdict elapsed text` in fixed columns, trailing space removed. */
char *fipsctl_stage_line(const fipsctl_stage_row *row, const char *spin)
{
    const char *mark = spin && !strcmp(row->verdict, "running") ? spin : " ";
    char elapsed[32] = "";
    if (row->has_elapsed) snprintf(elapsed, sizeof(elapsed), "%.1fs", (double)row->elapsed_ms / 1000.0);
    char *line = fipsctl_aprintf("%*s%-*s %s %-*s %*s  %s", IND, "", NAME_W, row->name, mark, VERDICT_W, row->verdict,
                                 ELAPSED_W, elapsed, row->text ? row->text : "");
    return line ? trim_end(line) : NULL;
}

size_t fipsctl_block_lines(const fipsctl_stage_row *rows, size_t count, const char *spin, char ***out)
{
    lines l = {0};
    const char *mark = spin ? spin : " ";
    for (size_t i = 0; i < count; ++i) {
        const fipsctl_stage_row *row = &rows[i];
        if (row->hidden) continue;
        if (l.count && (!strcmp(row->name, "path") || !strcmp(row->name, "session"))) lines_push(&l, strdup(""));
        lines_push(&l, fipsctl_stage_line(row, spin));
        for (size_t s = 0; s < row->sub_count; ++s) {
            char *sub = replace_all(row->subs[s], FIPSCTL_SPIN_HERE, mark);
            if (sub) { lines_push(&l, fipsctl_aprintf("%*s%s", TEXT_COL, "", sub)); free(sub); }
        }
    }
    *out = l.items;
    return l.count;
}

/* ---- notes and sections -------------------------------------------------------- */

const char *fipsctl_overall_note(const json_t *report)
{
    const char *overall = field(report, "overall");
    if (!strcmp(overall, "failed")) {
        const json_t *discovery = json_object_get(report, "discovery");
        if (discovery && !strcmp(field(discovery, "reason"), "bloom_unconfirmed"))
            return "             A peer's bloom filter claimed this address and then no lookup\n"
                   "             answered for it. A filter cannot miss a key that IS on the\n"
                   "             mesh, so an absent address produces exactly this whenever some\n"
                   "             peer's filter false-positives on it -- the same finding the\n"
                   "             fast bloom_miss reports, reached the slow way. An address that\n"
                   "             is present but whose lookups are being lost produces it too.\n"
                   "             The wait says nothing about which; it is the ladder running to\n"
                   "             the end.";
        return NULL;
    }
    if (!strcmp(overall, "partial")) {
        const json_t *rtt = json_object_get(report, "rtt");
        if (!rtt) return NULL;
        const char *reason = field(rtt, "reason");
        if (!strcmp(reason, "no_report"))
            return "             The handshake completed, so our packets reached them. Nothing has\n"
                   "             come back since, so the reverse path may be broken and this\n"
                   "             endpoint may be reachable in one direction only.";
        if (!strcmp(reason, "no_echo") || !strcmp(reason, "sub_millisecond") || !strcmp(reason, "bad_timestamp_echo"))
            return "             The endpoint IS reachable: the session handshake completed end to\n"
                   "             end and receiver reports came back. Only the round-trip\n"
                   "             measurement did not land inside the budget.";
        return NULL;
    }
    return NULL;
}

void fipsctl_probe_header(FILE *out, const json_t *report)
{
    const json_t *target = json_object_get(report, "target");
    fprintf(out, "probe %s\n", field(target, "display_name"));
    fprintf(out, "  npub       %s\n", field(target, "npub"));
    fprintf(out, "  node_addr  %s\n", field(target, "node_addr"));
    fprintf(out, "  ipv6       %s\n", field(target, "ipv6_addr"));
    fprintf(out, "\n");
}

void fipsctl_probe_stages(FILE *out, const json_t *report)
{
    fipsctl_stage_row *rows;
    size_t count = fipsctl_stage_rows(report, &rows);
    char **lines_out;
    size_t n = fipsctl_block_lines(rows, count, NULL, &lines_out);
    for (size_t i = 0; i < n; ++i) fprintf(out, "%s\n", lines_out[i]);
    fipsctl_lines_free(lines_out, n);
    fipsctl_stage_rows_free(rows, count);
}

static void probe_close(FILE *out, const json_t *report)
{
    const json_t *cleanup = json_object_get(report, "cleanup");
    fprintf(out, "\n");
    bool torn = as_bool(cleanup, "session_created_and_torn_down");
    const char *reason = json_string_value(json_object_get(cleanup, "left_intact_reason"));
    if (torn) fprintf(out, "  cleanup    session was opened by this probe and has been torn down\n");
    else if (reason) fprintf(out, "  cleanup    session left in place (%s)\n", reason);
    else fprintf(out, "  cleanup    nothing to clean up\n");
    uint64_t warmups = 0, tick = 0, elapsed = 0;
    (void)as_u64(cleanup, "warmups_sent", &warmups);
    if (warmups > 0 && !torn)
        fprintf(out, "             %llu coords-warmup message(s) were sent on a session this probe does not own\n",
                (unsigned long long)warmups);
    (void)as_u64(report, "tick_ms", &tick);
    (void)as_u64(report, "elapsed_ms", &elapsed);
    fprintf(out, "  overall    %s, %llu ms   (stage timings quantized to the %llu ms tick)\n", field(report, "overall"),
            (unsigned long long)elapsed, (unsigned long long)tick);
    const char *note = fipsctl_overall_note(report);
    if (note) fprintf(out, "%s\n", note);
}

void fipsctl_probe_detail(FILE *out, const json_t *report, bool tty)
{
    const json_t *path = json_object_get(report, "path");
    const json_t *session = json_object_get(report, "session");
    if (!strcmp(field(path, "reason"), "not_reached")) { probe_close(out, report); return; }
    fprintf(out, "\npath:\n");
    if (as_bool(path, "coords_known")) {
        uint64_t our_depth = 0, their_depth = 0, up, down, dist;
        (void)as_u64(path, "our_depth", &our_depth);
        (void)as_u64(path, "their_depth", &their_depth);
        char *ours = fipsctl_render_coords(json_object_get(path, "our_coords"), "self");
        char *theirs = fipsctl_render_coords(json_object_get(path, "their_coords"), "target");
        fprintf(out, "  ours       depth %-3llu  %s\n", (unsigned long long)our_depth, ours ? ours : "-");
        fprintf(out, "  theirs     depth %-3llu  %s\n", (unsigned long long)their_depth, theirs ? theirs : "-");
        free(ours); free(theirs);
        fprintf(out, "\n");
        const char *lca = json_string_value(json_object_get(path, "lca"));
        if (lca) {
            char *walk = fipsctl_walk_line(path, lca, tty);
            fprintf(out, "  route      %s\n", walk ? walk : "-");
            free(walk);
        } else fprintf(out, "  route      none (different spanning-tree roots)\n");
        fprintf(out, "\n");
        if (as_u64(path, "tree_hops_up", &up) && as_u64(path, "tree_hops_down", &down) && as_u64(path, "tree_distance", &dist))
            fprintf(out, "  walk       %llu up, %llu down   tree distance %llu (upper bound; crosslinks may shorten it)\n",
                    (unsigned long long)up, (unsigned long long)down, (unsigned long long)dist);
    } else {
        fprintf(out, "  coords     none cached for this target, so no tree walk was computed\n");
    }
    const json_t *hop = json_object_get(path, "next_hop");
    if (hop && !json_is_null(hop)) {
        char *addr = short_addr(field(hop, "node_addr"));
        char *class = unsnake(field(hop, "class"));
        fprintf(out, "  next hop   %s (%s)   class %s\n", addr ? addr : "", field(hop, "display_name"), class ? class : "");
        free(addr); free(class);
        if (as_bool(hop, "leaves_tree_walk")) {
            fprintf(out, "             (leaves the tree walk above; the real path is shorter\n");
            fprintf(out, "              than the tree distance)\n");
        }
    } else {
        char *why = unsnake(field(path, "no_hop_reason"));
        fprintf(out, "  next hop   none (%s)\n", why ? why : "");
        free(why);
    }
    uint64_t mtu;
    if (as_u64(session, "path_mtu", &mtu)) fprintf(out, "  path mtu   %llu bytes (observed)\n", (unsigned long long)mtu);
    probe_close(out, report);
}

/* ---- live progress --------------------------------------------------------------- */

static void header_once(fipsctl_probe_progress *p, const json_t *report)
{
    if (p->header) return;
    fipsctl_probe_header(p->out, report);
    p->header = true;
}

size_t fipsctl_settled_since(fipsctl_probe_progress *p, const fipsctl_stage_row *rows, size_t count, char ***out)
{
    size_t settled = 0;
    while (settled < count && rows[settled].settled) ++settled;
    char **all;
    size_t n = fipsctl_block_lines(rows, settled, NULL, &all);
    lines fresh = {0};
    for (size_t i = p->printed; i < n; ++i) lines_push(&fresh, strdup(all[i]));
    fipsctl_lines_free(all, n);
    p->printed = n;
    *out = fresh.items;
    return fresh.count;
}

static void emit(fipsctl_probe_progress *p, const fipsctl_stage_row *rows, size_t count)
{
    char **fresh;
    size_t n = fipsctl_settled_since(p, rows, count, &fresh);
    for (size_t i = 0; i < n; ++i) fprintf(p->out, "%s\n", fresh[i]);
    fipsctl_lines_free(fresh, n);
}

static void redraw(fipsctl_probe_progress *p, const fipsctl_stage_row *rows, size_t count)
{
    const char *spin = SPINNER[p->frame % 10];
    char **l;
    size_t n = fipsctl_block_lines(rows, count, spin, &l);
    if (p->drawn > 0) fprintf(p->out, "\x1b[%zuA", p->drawn);
    for (size_t i = 0; i < n; ++i) {
        char *clipped = fipsctl_clip(l[i], FIPSCTL_LIVE_WIDTH);
        fprintf(p->out, "\x1b[2K%s\n", clipped ? clipped : "");
        free(clipped);
    }
    /* A failure shortens the block: blank the rows the last frame drew below
     * the new last one and walk the cursor back up over them. */
    size_t stale = p->drawn > n ? p->drawn - n : 0;
    for (size_t i = 0; i < stale; ++i) fprintf(p->out, "\x1b[2K\n");
    if (stale) fprintf(p->out, "\x1b[%zuA", stale);
    fflush(p->out);
    fipsctl_lines_free(l, n);
    p->drawn = n;
    p->frame++;
}

static void erase(fipsctl_probe_progress *p)
{
    if (!p->drawn) return;
    fprintf(p->out, "\x1b[%zuA", p->drawn);
    for (size_t i = 0; i < p->drawn; ++i) fprintf(p->out, "\x1b[2K\n");
    fprintf(p->out, "\x1b[%zuA", p->drawn);
    fflush(p->out);
    p->drawn = 0;
}

void fipsctl_probe_update(fipsctl_probe_progress *p, const json_t *report)
{
    header_once(p, report);
    fipsctl_stage_row *rows;
    size_t count = fipsctl_stage_rows(report, &rows);
    if (p->tty) redraw(p, rows, count); else emit(p, rows, count);
    fipsctl_stage_rows_free(rows, count);
}

void fipsctl_probe_finish(fipsctl_probe_progress *p, const json_t *report)
{
    header_once(p, report);
    if (p->tty) { erase(p); fipsctl_probe_stages(p->out, report); }
    else {
        fipsctl_stage_row *rows;
        size_t count = fipsctl_stage_rows(report, &rows);
        emit(p, rows, count);
        fipsctl_stage_rows_free(rows, count);
    }
    fipsctl_probe_detail(p->out, report, p->tty);
}

/* ---- the run --------------------------------------------------------------------- */

/* `data` of an ok reply (a copy), or NULL after printing an error reply. */
static json_t *probe_data(const json_t *value, FILE *err)
{
    const char *status = json_string_value(json_object_get(value, "status"));
    if (status && !strcmp(status, "error")) {
        const char *message = json_string_value(json_object_get(value, "message"));
        fprintf(err, "error: %s\n", message ? message : "unknown error");
        return NULL;
    }
    json_t *data = json_object_get(value, "data");
    return data ? json_incref(data) : json_null();
}

static void sleep_ms(unsigned ms)
{
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
}

int fipsctl_run_probe(const char *socket_path, const char *npub, bool json, bool has_timeout, uint64_t timeout_secs,
                      FILE *out, FILE *err)
{
    char *error = NULL;
    char *start = fipsctl_build_command("probe_start", json_pack("{s:s}", "npub", npub));
    if (!start) { fprintf(err, "error: out of memory\n"); return 1; }
    json_t *value = fipsctl_send_request(socket_path, start, &error);
    free(start);
    if (!value) { fprintf(err, "error: %s\n", error ? error : "request failed"); free(error); return 1; }
    json_t *data = probe_data(value, err);
    json_decref(value);
    if (!data) return 1;
    uint64_t probe_id = 0, budget_ms = 30000;
    (void)as_u64(data, "probe_id", &probe_id);
    (void)as_u64(data, "budget_ms", &budget_ms);
    json_decref(data);
    uint64_t ceiling_ms = fipsctl_probe_ceiling_ms(budget_ms, has_timeout, timeout_secs);
    char *poll = fipsctl_build_command("probe_poll", json_pack("{s:I}", "probe_id", (json_int_t)probe_id));
    if (!poll) { fprintf(err, "error: out of memory\n"); return 1; }
    fipsctl_probe_progress progress = {.tty = isatty(fileno(out)) == 1, .out = out};
    uint64_t waited_ms = 0;
    int rc = 1;
    for (;;) {
        sleep_ms(POLL_INTERVAL_MS);
        waited_ms += POLL_INTERVAL_MS;
        value = fipsctl_send_request(socket_path, poll, &error);
        if (!value) { fprintf(err, "error: %s\n", error ? error : "request failed"); free(error); break; }
        data = probe_data(value, err);
        json_decref(value);
        if (!data) break;
        json_t *report = json_object_get(data, "report");
        if (!report) report = json_null();
        const char *state = json_string_value(json_object_get(data, "state"));
        if (state && !strcmp(state, "done")) {
            if (json) {
                char *text = fipsctl_json_pretty(report);
                fprintf(out, "%s\n", text ? text : "null");
                free(text);
            } else fipsctl_probe_finish(&progress, report);
            const char *overall = json_string_value(json_object_get(report, "overall"));
            rc = overall && !strcmp(overall, "ok") ? 0 : 1;
            json_decref(data);
            break;
        }
        if (!json) fipsctl_probe_update(&progress, report);
        json_decref(data);
        if (waited_ms >= ceiling_ms) {
            /* Best effort: the daemon-side deadline cleans up regardless. */
            char *cancel = fipsctl_build_command("probe_cancel", json_pack("{s:I}", "probe_id", (json_int_t)probe_id));
            if (cancel) { json_t *ignored = fipsctl_send_request(socket_path, cancel, NULL); json_decref(ignored); free(cancel); }
            fprintf(err, "error: probe did not complete within %llu ms\n", (unsigned long long)ceiling_ms);
            break;
        }
    }
    free(poll);
    return rc;
}
