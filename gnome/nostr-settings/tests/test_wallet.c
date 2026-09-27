/* test_wallet — budgets.json enumeration (read-only) and formatting; when
 * the wallet agent is part of the build, the file is produced by the
 * agent's own NwaBudgetStore. SPDX-License-Identifier: MIT */
#include "nss-wallet.h"

#include <glib/gstdio.h>
#include <string.h>

#ifdef NSS_TEST_WALLET_WRITER
#include "nwa-budget.h"

static GDateTime *
fixed_clock(gpointer d)
{
  (void)d;
  return g_date_time_new_local(2026, 9, 26, 12, 0, 0);
}
#endif

static gchar *tmp;

static void
test_budgets(void)
{
  g_autofree gchar *p = g_build_filename(tmp, "budgets.json", NULL);
  GError *e = NULL;
  g_autoptr(GPtrArray) none = nss_wallet_budgets_load(p, "2026-09-26", &e);
  g_assert_no_error(e);
  g_assert_cmpuint(none->len, ==, 0);

  g_assert_true(g_file_set_contents(p,
    "{\"version\":1,\"apps\":{"
    "\"org.gnostr.Client\":{\"limit_msat_per_day\":21000000,\"allow_read\":true,"
    "\"day\":\"2026-09-26\",\"spent_msat\":1500000},"
    "\"exe:/usr/bin/zapper\":{\"limit_msat_per_day\":0,\"allow_read\":false,"
    "\"day\":\"2026-09-25\",\"spent_msat\":999000},"
    "\"bad\":7}}", -1, NULL));
  g_autoptr(GPtrArray) b = nss_wallet_budgets_load(p, "2026-09-26", &e);
  g_assert_no_error(e);
  g_assert_cmpuint(b->len, ==, 2);
  NssBudget *x = g_ptr_array_index(b, 0);   /* sorted: "exe:" < "org." */
  g_assert_cmpstr(x->app_id, ==, "exe:/usr/bin/zapper");
  g_assert_cmpuint(x->spent_today_msat, ==, 0);  /* yesterday's spend */
  g_assert_false(x->allow_read);
  NssBudget *y = g_ptr_array_index(b, 1);
  g_assert_cmpstr(y->app_id, ==, "org.gnostr.Client");
  g_assert_cmpuint(y->limit_msat_per_day, ==, 21000000);
  g_assert_cmpuint(y->spent_today_msat, ==, 1500000);
  g_assert_true(y->allow_read);
  g_assert_true(y->allow_receive);     /* version 1: allow_read meant both */
  g_assert_false(x->allow_receive);

  /* Version 2 keeps the two grants apart (nostrc-muhk). */
  g_assert_true(g_file_set_contents(p,
    "{\"version\":2,\"apps\":{\"org.gnostr.Client\":{\"allow_read\":true,\"allow_receive\":false},"
    "\"org.example.Inv\":{\"allow_read\":false,\"allow_receive\":true}}}", -1, NULL));
  g_autoptr(GPtrArray) v2 = nss_wallet_budgets_load(p, "2026-09-26", &e);
  g_assert_no_error(e);
  g_assert_cmpuint(v2->len, ==, 2);
  NssBudget *inv = g_ptr_array_index(v2, 0);
  g_assert_cmpstr(inv->app_id, ==, "org.example.Inv");
  g_assert_false(inv->allow_read);
  g_assert_true(inv->allow_receive);
  NssBudget *cl = g_ptr_array_index(v2, 1);
  g_assert_true(cl->allow_read);
  g_assert_false(cl->allow_receive);

  g_assert_true(g_file_set_contents(p, "{not json", -1, NULL));
  g_assert_null(nss_wallet_budgets_load(p, NULL, &e));
  g_assert_nonnull(e);
  g_clear_error(&e);

#ifdef NSS_TEST_WALLET_WRITER
  g_remove(p);
  NwaBudgetStore *s = nwa_budget_store_new(p, fixed_clock, NULL);
  nwa_budget_store_set_limit(s, "org.example.App", 5000000);
  nwa_budget_store_set_allow_read(s, "org.example.App", TRUE);
  nwa_budget_store_set_allow_receive(s, "org.example.App", FALSE);
  guint res = nwa_budget_store_reserve(s, "org.example.App", 1000000, FALSE);
  g_assert_cmpuint(res, !=, 0);
  g_assert_true(nwa_budget_store_save(s, &e));
  nwa_budget_store_free(s);
  g_autoptr(GPtrArray) w = nss_wallet_budgets_load(p, "2026-09-26", &e);
  g_assert_no_error(e);
  g_assert_cmpuint(w->len, ==, 1);
  NssBudget *z = g_ptr_array_index(w, 0);
  g_assert_cmpuint(z->limit_msat_per_day, ==, 5000000);
  g_assert_cmpuint(z->spent_today_msat, >=, 1000000);   /* + fee reserve */
  g_assert_true(z->allow_read);
  g_assert_false(z->allow_receive);   /* the agent writes version 2 */
#endif
}

static void
test_format(void)
{
  g_autofree gchar *a = nss_format_sats(21000000);
  g_assert_cmpstr(a, ==, "21,000 sats");
  g_autofree gchar *b = nss_format_sats(1999);
  g_assert_cmpstr(b, ==, "1 sat");
  g_autofree gchar *c = nss_format_sats(0);
  g_assert_cmpstr(c, ==, "0 sats");
  g_autofree gchar *l1 = nss_wallet_app_label("exe:/usr/bin/zapper");
  g_assert_cmpstr(l1, ==, "zapper (unverified)");
  g_autofree gchar *l2 = nss_wallet_app_label("org.gnostr.Client");
  g_assert_cmpstr(l2, ==, "org.gnostr.Client");
  g_autofree gchar *l3 = nss_wallet_app_label(NSS_WALLET_SHELL_APP_ID);
  g_assert_cmpstr(l3, ==, "GNOME Shell");
  g_autofree gchar *l4 = nss_wallet_app_label("https://snort.social");
  g_assert_cmpstr(l4, ==, "Website https://snort.social");
  g_autofree gchar *path = nss_wallet_budgets_path();
  g_assert_true(g_str_has_suffix(path, "/nostr-wallet/budgets.json"));
}

/* The shape org.nostr.Wallet1.ListApps returns (gnome/dbus/org.nostr.Wallet1.xml). */
static void
test_list_apps_variant(void)
{
  GVariant *v = g_variant_new_parsed(
    "{'org.gnostr.Client': {'limit_msat_per_day': <uint64 21000000>, 'spent_today_msat': <uint64 1500>,"
    " 'allow_read': <true>},"
    " 'exe:/usr/bin/gnome-shell': {'limit_msat_per_day': <uint64 0>, 'spent_today_msat': <uint64 0>,"
    " 'allow_read': <true>, 'allow_receive': <false>}, 'https://a.example': @a{sv} {}}");
  g_variant_ref_sink(v);
  g_autoptr(GPtrArray) a = nss_wallet_apps_from_variant(v);
  g_variant_unref(v);
  g_assert_cmpuint(a->len, ==, 3);
  NssBudget *s = g_ptr_array_index(a, 0);
  g_assert_cmpstr(s->app_id, ==, NSS_WALLET_SHELL_APP_ID);
  g_assert_true(s->allow_read);
  g_assert_false(s->allow_receive);     /* split grant from a current agent */
  NssBudget *w = g_ptr_array_index(a, 1);
  g_assert_cmpstr(w->app_id, ==, "https://a.example");   /* missing keys: zero */
  g_assert_cmpuint(w->limit_msat_per_day, ==, 0);
  NssBudget *g = g_ptr_array_index(a, 2);
  g_assert_cmpuint(g->limit_msat_per_day, ==, 21000000);
  g_assert_cmpuint(g->spent_today_msat, ==, 1500);
  g_assert_true(g->allow_read);
  g_assert_true(g->allow_receive);      /* older agent: no key, read meant both */
  g_autoptr(GPtrArray) none = nss_wallet_apps_from_variant(NULL);
  g_assert_cmpuint(none->len, ==, 0);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/nostr-settings/wallet/list-apps", test_list_apps_variant);
  tmp = g_dir_make_tmp("nss-wallet-XXXXXX", NULL);
  g_test_add_func("/nostr-settings/wallet/budgets", test_budgets);
  g_test_add_func("/nostr-settings/wallet/format", test_format);
  int rc = g_test_run();
  g_autofree gchar *cmd = g_strdup_printf("rm -rf '%s'", tmp);
  if (system(cmd) != 0 && rc == 0)
    rc = 1;
  return rc;
}
