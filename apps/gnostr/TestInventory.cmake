include_guard(DIRECTORY)
if(COMMAND nostrc_describe_test AND TEST gnostr-resource-build)
  get_filename_component(_gnostr_resource_repo
    "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
  # The fixture copies and hashes the full data tree, not just manifest entries.
  file(GLOB_RECURSE _gnostr_resource_fixture_inputs CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/data/*")
  nostrc_describe_test(gnostr-resource-build
    OWNER gnostr
    INPUTS
      "${CMAKE_CURRENT_LIST_FILE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt"
      "${_gnostr_resource_repo}/tests/common/test_blueprint_resources.py"
      "${_gnostr_resource_repo}/cmake/NostrcGtkResources.cmake"
      ${_gnostr_resource_fixture_inputs}
    EXPECTED pass)
endif()
