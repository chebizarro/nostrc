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
        # Hanami now consumes vault rings. Product edits propagate through that
        # real edge; package test inputs still do not select those consumers.
        consumers = {"root.asan.libhanami", "supported.libhanami",
                     "core.default_build", "grotto.default_build", "groundhog.default_build",
                     "nostr_seal.publish", "nostr_seal.signer", "nostr_seal.unit"}
        for path in ("libvault/src/fold.c", "libvault/include/vault/vault.h",
                     "libvault/tests/test_revoke_lazy.c", "libvault/src/nip59.c",
                     "libvault/include/vault/nip59.h", "libvault/tests/test_grant_nip59.c"):
            plan = s.make_plan(self.policy, [change(path)], head="a" * 40,
                               base="b" * 40, context="ci")
            selected = set(plan["selected_checks"])
            if "/tests/" in path:
                self.assertEqual(selected, expected)
            else:
                self.assertTrue(expected | consumers | {"supported.nostr.gobject.vault",
                                                        "bindings.rust.consumer",
                                                        "root.asan.libvault.marmot",
                                                        "supported.libvault.marmot",
                                                        "root.asan.vaultctl", "supported.vaultctl"} <= selected)
                self.assertFalse({"supported.libporthome", "supported.libblossom"} & selected)
        for name in expected:
            check = self.policy["checks"][name]
            targets = {"test_vault_ring_derivation", "test_vault_revoke_lazy",
                       "test_vault_fold_frontier", "test_vault_codecs", "test_vault_grant_nip59", "test_vault_tree_ops"}
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

    def test_gn_vault_wrapper_selection(self):
        name = "supported.nostr.gobject.vault"
        check = self.policy["checks"][name]
        self.assertEqual(check["build_targets"], ["test_nostr_gobject_vault"])
        self.assertIn("nip59", check["nodes"])
        for profile in check["profiles"]:
            self.assertEqual(check["ctest_tests"][profile], ["test_nostr_gobject_vault"])
        for path in ("nostr-gobject/src/gn-vault.c",
                     "nostr-gobject/include/nostr-gobject-1.0/gn-vault.h",
                     "nostr-gobject/tests/test_vault.c", "libvault/src/codec.c"):
            with self.subTest(path=path):
                plan = s.make_plan(self.policy, [change(path)], head="a" * 40,
                                   base="b" * 40, context="ci")
                self.assertEqual(plan["effective_mode"], "affected")
                self.assertIn(name, plan["selected_checks"])
                self.assertNotIn("supported.libporthome", plan["selected_checks"])
        for path in ("libvault/tests/test_codecs.c", "libhanami/src/hanami.c"):
            plan = s.make_plan(self.policy, [change(path)], head="a" * 40,
                               base="b" * 40, context="ci")
            self.assertNotIn(name, plan["selected_checks"])

    def test_marmot_adapter_selection_and_dependency_direction(self):
        expected = {"root.asan.libvault.marmot", "supported.libvault.marmot"}
        for path in ("libvault-marmot/src/carrier.c", "libvault-marmot/src/seed.c",
                     "libvault-marmot/include/vault-marmot/vault-marmot.h",
                     "libvault-marmot/tests/test_grant_marmot.c"):
            plan = s.make_plan(self.policy, [change(path)], head="a" * 40,
                               base="b" * 40, context="ci")
            self.assertEqual(plan["effective_mode"], "affected")
            self.assertEqual(set(plan["selected_checks"]), expected)
        for path in ("libmarmot/src/exporter.c", "libvault/src/nip59.c",
                     "libporthome/src/crypto.c"):
            plan = s.make_plan(self.policy, [change(path)], head="a" * 40,
                               base="b" * 40, context="ci")
            self.assertTrue(expected <= set(plan["selected_checks"]))
        self.assertNotIn("libmarmot", self.policy["nodes"]["libvault"]["depends_on"])
        for name in expected:
            check = self.policy["checks"][name]
            self.assertEqual(check["build_targets"], ["test_vault_grant_marmot"])
            for profile in check["profiles"]:
                self.assertEqual(check["ctest_tests"][profile], ["test_vault_grant_marmot"])
        for name in ("root.asan.libmarmot", "supported.libmarmot"):
            check = self.policy["checks"][name]
            for profile in check["profiles"]:
                self.assertIn("marmot_test_exporter", check["ctest_tests"][profile])
        plan = s.make_plan(self.policy, [change("libhanami/src/hanami.c")],
                           head="a" * 40, base="b" * 40, context="ci")
        self.assertFalse(expected & set(plan["selected_checks"]))

    def test_hanami_sealed_registration_and_focused_test_inputs(self):
        families = {"root.asan.libhanami", "supported.libhanami"}
        self.assertIn("libvault", self.policy["nodes"]["libhanami"]["depends_on"])
        for family in families:
            check = self.policy["checks"][family]
            for tests in check["ctest_tests"].values():
                self.assertIn("test_hanami_sealed_roundtrip", tests)
                self.assertIn("test-hanami-nostr-protocol", tests)
            self.assertIn("{source}/libhanami/tests/sealed_fixture.py", check["runtime_inputs"])
        for source in ("test_hanami_sealed_roundtrip.c", "sealed_fixture.py"):
            plan = s.make_plan(self.policy, [change("libhanami/tests/" + source)],
                               head="a" * 40, base="b" * 40, context="ci")
            self.assertEqual(set(plan["selected_checks"]), families)
            self.assertEqual(plan["broadening_reasons"], [])
        plan = s.make_plan(self.policy, [change("libhanami/src/hanami-sealed.c")],
                           head="a" * 40, base="b" * 40, context="ci")
        self.assertTrue(families <= set(plan["selected_checks"]))
        self.assertNotIn("supported.libvault", plan["selected_checks"])
        self.assertNotIn("supported.libblossom", plan["selected_checks"])

    def test_vaultctl_scoped_and_transitive_selection(self):
        for path in ("tools/vaultctl/main.c", "tools/vaultctl/tests/fixtures.py", "tools/vaultctl/tests/fixture_helper.c"):
            plan = s.make_plan(self.policy, [change(path)], head="a" * 40, base="b" * 40, context="ci")
            self.assertEqual(plan["effective_mode"], "affected")
            self.assertEqual(set(plan["selected_checks"]), {"root.asan.vaultctl", "supported.vaultctl"})
        for path in ("libvault/include/vault/tree.h", "libblossom/src/client.c", "libnostr/src/simplepool.c", "nips/nip42/src/nip42.c"):
            plan = s.make_plan(self.policy, [change(path)], head="a" * 40, base="b" * 40, context="ci")
            self.assertTrue({"root.asan.vaultctl", "supported.vaultctl"} <= set(plan["selected_checks"]))
        for path in ("libvault/tests/test_tree_ops.c", "libhanami/tests/test_integration.c"):
            plan = s.make_plan(self.policy, [change(path)], head="a" * 40, base="b" * 40, context="ci")
            self.assertNotIn("supported.vaultctl", plan["selected_checks"])

    def test_vault_syncd_inputs_select_a_real_bounded_runner(self):
        import sys
        sys.path.insert(0, str(ROOT / ".github/workflows"))
        import ci_selection as ci
        check_name = "homed.vault.filesystem"
        for path in ("gnome/nostr-homed/src/porthome-syncd/nh_syncd_vault_fs.c",
                     "gnome/nostr-homed/src/porthome-syncd/nh_syncd_vault_apply.c",
                     "gnome/nostr-homed/src/porthome-syncd/nh_syncd_vault_config.c",
                     "gnome/nostr-homed/src/porthome-syncd/nh_syncd_vault_config.h",
                     "gnome/nostr-homed/tests/integration/test_syncd_vault_config.py",
                     "gnome/nostr-homed/tests/unit/test_syncd_vault_fs.c",
                     "gnome/nostr-homed/tests/integration/test_syncd_off_closure.sh",
                     ".github/workflows/vault-syncd-ci.yml"):
            with self.subTest(path=path):
                plan = s.make_plan(self.policy, [change(path)], head="a" * 40,
                                   base="b" * 40, context="ci")
                self.assertEqual(plan["effective_mode"], "affected")
                self.assertEqual(set(plan["selected_checks"]), {check_name})
                self.assertEqual(ci.project(plan)["families"], ["vault_syncd"])
        plan = s.make_plan(self.policy, [change("libvault/src/fold.c")],
                           head="a" * 40, context="ci")
        self.assertIn(check_name, plan["selected_checks"])
        for path in ("libvault/tests/test_fold_frontier.c", "tools/vaultctl/tests/fixtures.py",
                     "gnome/groundhog/src/ui/gh-about-dialog.c"):
            plan = s.make_plan(self.policy, [change(path)], head="a" * 40, context="ci")
            self.assertNotIn(check_name, plan["selected_checks"])
        check = self.policy["checks"][check_name]
        self.assertEqual(set(check["ctest_tests"]["homed.vault.linux"]), {
            "homed_syncd_vault_fs", "homed_syncd_off_closure",
            "homed_vault_pam_nostr_closure", "homed_vault_nss_nostr_closure",
            "json_glib_off_closure", "homed_syncd_vault_config", "homed_syncd_push_e2e",
            "homed_syncd_pusher_multi_server"})
        workflow = (ROOT / ".github/workflows/vault-syncd-ci.yml").read_text()
        self.assertIn("python3 scripts/test-plan.py run", workflow)
        self.assertIn("--profile homed.vault.linux", workflow)
        self.assertIn("test \"$(git rev-parse HEAD)\" = \"$REQUESTED_SHA\"", workflow)
        aggregate = (ROOT / ".github/workflows/ci-selective.yml").read_text()
        self.assertIn("uses: ./.github/workflows/vault-syncd-ci.yml", aggregate)
        self.assertIn("homed, vault_syncd, signet", aggregate)

    def test_unrelated_product_edit_does_not_select_boundary_tool_regressions(self):
        plan = s.make_plan(self.policy, [change("libgo/src/channel.c")],
                           head="a" * 40, base="b" * 40, context="ci")
        self.assertFalse(set(plan["selected_checks"]) & {
            "package.boundaries.guard", "package.boundaries.regressions"
        })
