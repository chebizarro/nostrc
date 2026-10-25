"""Package boundary tools have executable, bounded affected-check ownership."""
import unittest

from selector_support import ROOT, change, s


class PackageBoundarySelection(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.policy = s.load_inventory(ROOT / "tests/selection/inventory.json")

    def test_script_and_allowlist_edits_select_only_boundary_commands(self):
        paths = (
            "scripts/check-package-boundaries.sh",
            "scripts/check-package-boundaries.py",
            "scripts/test-package-boundaries.py",
            "scripts/package-boundaries.allowlist",
        )
        expected = {"package.boundaries.guard", "package.boundaries.regressions"}
        for path in paths:
            for context in ("ci", "local"):
                with self.subTest(path=path, context=context):
                    self.assertEqual(s.classify(self.policy, path)["nodes"], ["package.boundaries"])
                    plan = s.make_plan(self.policy, [change(path)], head="a" * 40,
                                       base="b" * 40, context=context)
                    self.assertEqual(set(plan["selected_checks"]), expected)
                    for name in expected:
                        check = self.policy["checks"][name]
                        self.assertEqual(check["kind"], "command")
                        self.assertEqual(check["build_targets"], [])
                        self.assertIn("selection.operational.linux", check["profiles"])
                        self.assertIn("selection.operational.macos", check["profiles"])

    def test_porthome_move_retains_real_consumers_and_omits_prerequisite_suites(self):
        for path in ("libporthome/src/crypto.c", "libporthome/src/index.c",
                     "libporthome/include/porthome/compat-nh.h"):
            with self.subTest(path=path):
                self.assertEqual(s.classify(self.policy, path)["nodes"], ["libporthome"])
                plan = s.make_plan(self.policy, [change(path)], head="a" * 40,
                                   base="b" * 40, context="ci")
                selected = set(plan["selected_checks"])
                self.assertTrue({"supported.libporthome", "supported.libhanami",
                                 "homed.units", "nostr_seal.unit",
                                 "nostr_seal.streaming"} <= selected)
                self.assertNotIn("supported.libblossom", selected)
                self.assertNotIn("supported.nips.bud02", selected)

    def test_porthome_test_only_input_does_not_select_hanami_or_homed(self):
        plan = s.make_plan(self.policy, [change("libporthome/tests/test_api.c")],
                           head="a" * 40, base="b" * 40, context="ci")
        self.assertEqual(set(plan["selected_checks"]),
                         {"supported.libporthome", "root.asan.libporthome"})

    def test_blossom_runtime_selects_glib_consumers_not_upstream_suites(self):
        plan = s.make_plan(self.policy, [change("libblossom/src/client.c")],
                           head="a" * 40, base="b" * 40, context="ci")
        selected = set(plan["selected_checks"])
        self.assertEqual(plan["effective_mode"], "affected")
        self.assertTrue({"supported.nostr.gobject.blossom", "groundhog.blossom",
                         "groundhog.media"} <= selected)
        self.assertFalse({"root.asan.libgo", "root.asan.nip17"} & selected)
        self.assertIn("nostr.gobject", self.policy["nodes"]["gnome.nostr_share"]["depends_on"])
        test_plan = s.make_plan(self.policy, [change("libblossom/tests/test_blossom_guards.c")],
                                head="a" * 40, base="b" * 40, context="ci")
        self.assertNotIn("supported.nostr.gobject.blossom", test_plan["selected_checks"])

    def test_porthome_v2_tests_are_registered_for_all_porthome_profiles(self):
        targets = {"test_porthome_seal_v2", "test_porthome_ring_derivation",
                   "test_porthome_pad_buckets", "test_porthome_chunker",
                   "test_porthome_dir_node", "test_porthome_head"}
        for name in ("supported.libporthome", "root.asan.libporthome"):
            check = self.policy["checks"][name]
            self.assertTrue(targets <= set(check["build_targets"]))
            for profile in check["profiles"]:
                self.assertTrue(targets <= set(check["ctest_tests"][profile]))
        for source in ("test_seal_v2.c", "test_ring_derivation.c",
                       "test_pad_buckets.c", "test_chunker.c", "test_helpers.h",
                       "test_dir_node.c", "test_head.c"):
            plan = s.make_plan(self.policy, [change("libporthome/tests/" + source)],
                               head="a" * 40, base="b" * 40, context="ci")
            expected = {"supported.libporthome", "root.asan.libporthome"}
            if source in ("test_seal_v2.c", "test_helpers.h"):
                expected.add("bindings.rust.consumer")
            self.assertEqual(set(plan["selected_checks"]), expected)

    def test_vault_inputs_and_downstream_membership(self):
        expected = {"root.asan.libvault", "supported.libvault"}
        for path in ("libvault/src/fold.c", "libvault/include/vault/vault.h",
                     "libvault/tests/test_revoke_lazy.c", "libvault/src/nip59.c",
                     "libvault/include/vault/nip59.h", "libvault/tests/test_grant_nip59.c"):
            plan = s.make_plan(self.policy, [change(path)], head="a" * 40,
                               base="b" * 40, context="ci")
            wanted = expected | ({"bindings.rust.consumer"} if "/tests/" not in path else set())
            self.assertEqual(set(plan["selected_checks"]), wanted)
        for name in expected:
            check = self.policy["checks"][name]
            targets = {"test_vault_ring_derivation", "test_vault_revoke_lazy",
                       "test_vault_fold_frontier", "test_vault_codecs", "test_vault_grant_nip59"}
            self.assertEqual(set(check["build_targets"]), targets)
            for profile in check["profiles"]:
                self.assertEqual(set(check["ctest_tests"][profile]), targets)
        plan = s.make_plan(self.policy, [change("libporthome/src/head.c")],
                           head="a" * 40, base="b" * 40, context="ci")
        self.assertTrue(expected <= set(plan["selected_checks"]))
        self.assertNotIn("supported.libblossom", plan["selected_checks"])
        for path in ("nips/nip59/src/nip59.c", "nips/nip59/nip59.pc.in"):
            plan = s.make_plan(self.policy, [change(path)], head="a" * 40,
                               base="b" * 40, context="ci")
            self.assertEqual(plan["effective_mode"], "affected")
            self.assertTrue(expected <= set(plan["selected_checks"]))
            self.assertNotIn("supported.libblossom", plan["selected_checks"])

    def test_rust_inputs_select_only_installed_consumer_and_dispatch(self):
        import sys
        sys.path.insert(0, str(ROOT / ".github/workflows"))
        import ci_selection as ci
        for path in ("bindings/rust/porthome-sys/build.rs",
                     "bindings/rust/vault-sys/wrapper.h",
                     "bindings/rust/examples/vault-cli/src/main.rs",
                     "bindings/rust/Cargo.lock", "bindings/rust/test-installed.sh",
                     ".github/workflows/rust-bindings-ci.yml"):
            for context in ("ci", "local"):
                with self.subTest(path=path, context=context):
                    plan = s.make_plan(self.policy, [change(path)], head="a" * 40,
                                       base="b" * 40, context=context)
                    self.assertEqual(plan["effective_mode"], "affected")
                    self.assertEqual(plan["selected_checks"], ["bindings.rust.consumer"])
                    dispatch = ci.project(plan)
                    self.assertFalse(dispatch["broad"])
                    self.assertEqual(dispatch["families"], ["rust"])
        for path in ("libporthome/include/porthome/crypto.h",
                     "libvault/include/vault/nip59.h", "libblossom/src/client.c"):
            plan = s.make_plan(self.policy, [change(path)], head="a" * 40, context="ci")
            self.assertIn("bindings.rust.consumer", plan["selected_checks"])
            self.assertIn("rust", ci.project(plan)["families"])
        # Unrelated app edits do not exercise the installed-library consumer.
        plan = s.make_plan(self.policy, [change("gnome/groundhog/src/ui/gh-about-dialog.c")],
                           head="a" * 40, context="ci")
        self.assertNotIn("bindings.rust.consumer", plan["selected_checks"])
        self.assertNotIn("rust", ci.project(plan)["families"])

    def test_rust_job_runs_prefix_check_and_is_in_the_aggregate_gate(self):
        workflow = (ROOT / ".github/workflows/ci-selective.yml").read_text()
        self.assertIn("uses: ./.github/workflows/rust-bindings-ci.yml", workflow)
        self.assertIn("hanami, rust, gi", workflow)
        runner = (ROOT / ".github/workflows/rust-bindings-ci.yml").read_text()
        self.assertIn("bash bindings/rust/test-installed.sh", runner)
        self.assertIn("libclang-dev", runner)
        self.assertIn("ref: ${{ inputs.sha || github.sha }}", runner)
        self.assertIn('test "$(git rev-parse HEAD)" = "$REQUESTED_SHA"', runner)

    def test_unrelated_product_edit_does_not_select_boundary_tool_regressions(self):
        plan = s.make_plan(self.policy, [change("libgo/src/channel.c")],
                           head="a" * 40, base="b" * 40, context="ci")
        self.assertFalse(set(plan["selected_checks"]) & {
            "package.boundaries.guard", "package.boundaries.regressions"
        })
