include_guard(DIRECTORY)
if(COMMAND nostrc_describe_test AND TEST nostr-gtk-resource-build)
  get_filename_component(_nostr_gtk_resource_repo
    "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
  file(GLOB_RECURSE _nostr_gtk_resource_fixture_inputs CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/data/*")
  nostrc_describe_test(nostr-gtk-resource-build
    OWNER nostr.gtk
    INPUTS
      "${CMAKE_CURRENT_LIST_FILE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt"
      "${_nostr_gtk_resource_repo}/tests/common/test_blueprint_resources.py"
      "${_nostr_gtk_resource_repo}/cmake/NostrcGtkResources.cmake"
      ${_nostr_gtk_resource_fixture_inputs}
    EXPECTED pass)
endif()

# The perf registration invokes xvfb-run, not the executable as argv[0].
# Preserve the wrapper and identify its actual linked target explicitly.
if(COMMAND nostrc_describe_test AND TEST test_nostr_gtk_bind_latency_perf)
  nostrc_describe_test(test_nostr_gtk_bind_latency_perf OWNER nostr.gtk
    TARGETS test_nostr_gtk_bind_latency_budget REQUIRES display)
endif()

# Exact registration/input relationships retained from the ab5 production audit.
if(TEST "test_nostr_gtk_bind_latency_budget")
  nostrc_add_test_owners("test_nostr_gtk_bind_latency_budget" OWNER shared.tests)
endif()
if(TEST "test_nostr_gtk_bind_latency_perf")
  nostrc_add_test_owners("test_nostr_gtk_bind_latency_perf" OWNER shared.tests)
endif()
if(TEST "test_nostr_gtk_content_parser")
  nostrc_add_test_owners("test_nostr_gtk_content_parser" OWNER nip06 nip11 nip46 nip49 nip55l relayd)
endif()
if(TEST "test_nostr_gtk_markdown_pango")
  nostrc_add_test_owners("test_nostr_gtk_markdown_pango" OWNER nip06 nip11 nip34 nip46 nip49 nip55l relayd)
endif()
if(TEST "test_nostr_gtk_media")
  nostrc_add_test_owners("test_nostr_gtk_media" OWNER nip06 nip11 nip34 nip46 nip49 nip55l relayd shared.tests)
endif()
if(TEST "test_nostr_gtk_nip34_issue_model")
  nostrc_add_test_owners("test_nostr_gtk_nip34_issue_model" OWNER nip34)
endif()
if(TEST "test_nostr_gtk_nip34_issue_view")
  nostrc_add_test_owners("test_nostr_gtk_nip34_issue_view" OWNER nip06 nip11 nip34 nip46 nip49 nip55l relayd shared.tests)
endif()
if(TEST "test_nostr_gtk_og_metadata")
  nostrc_add_test_owners("test_nostr_gtk_og_metadata" OWNER nip06 nip11 nip34 nip46 nip49 nip55l relayd)
endif()
if(TEST "test_nostr_gtk_og_preview_card")
  nostrc_add_test_owners("test_nostr_gtk_og_preview_card" OWNER nip06 nip11 nip34 nip46 nip49 nip55l relayd shared.tests)
endif()
if(TEST "test_nostr_gtk_portable")
  nostrc_add_test_owners("test_nostr_gtk_portable" OWNER nip06 nip11 nip34 nip46 nip49 nip55l relayd shared.tests)
endif()
if(TEST "test_nostr_gtk_portable_i18n")
  nostrc_add_test_owners("test_nostr_gtk_portable_i18n" OWNER nip06 nip11 nip34 nip46 nip49 nip55l relayd shared.tests)
endif()
if(TEST "test_nostr_gtk_reference_card")
  nostrc_add_test_owners("test_nostr_gtk_reference_card" OWNER nip06 nip11 nip34 nip46 nip49 nip55l relayd shared.tests)
endif()
if(TEST "test_nostr_gtk_standalone_core")
  nostrc_add_test_owners("test_nostr_gtk_standalone_core" OWNER gnostr nip06 nip11 nip34 nip46 nip49 nip55l relayd shared.tests)
endif()
