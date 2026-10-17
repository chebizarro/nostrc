# Shared build-tree-only Blueprint/GResource policy for independent components.
# The caller retains its manifest, generated C name and public resource paths.
include_guard(GLOBAL)

function(nostrc_compile_blueprint_resources output_variable resource_xml resource_c blueprint_target)
  get_filename_component(data_dir "${resource_xml}" DIRECTORY)
  find_program(BLUEPRINT_COMPILER blueprint-compiler)
  find_program(GLIB_COMPILE_RESOURCES glib-compile-resources REQUIRED)

  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${resource_xml}")
  file(STRINGS "${resource_xml}" resource_entries REGEX "<file[ >]")
  set(resource_inputs)
  foreach(entry IN LISTS resource_entries)
    string(REGEX REPLACE ".*<file[^>]*>([^<]+)</file>.*" "\\1" relative_path "${entry}")
    list(APPEND resource_inputs "${data_dir}/${relative_path}")
  endforeach()

  set(compiled_ui_files)
  set(resource_dirs)
  if(BLUEPRINT_COMPILER)
    message(STATUS "Found blueprint-compiler: ${BLUEPRINT_COMPILER}")
    foreach(blp_file IN LISTS ARGN)
      file(RELATIVE_PATH blp_relative "${data_dir}" "${blp_file}")
      string(REGEX REPLACE "\\.blp$" ".ui" ui_relative "${blp_relative}")
      set(ui_file "${CMAKE_CURRENT_BINARY_DIR}/data/${ui_relative}")
      get_filename_component(ui_dir "${ui_file}" DIRECTORY)
      add_custom_command(
        OUTPUT "${ui_file}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${ui_dir}"
        COMMAND "${BLUEPRINT_COMPILER}" compile --output "${ui_file}" "${blp_file}"
        DEPENDS "${blp_file}"
        COMMENT "Compiling ${blueprint_target}: ${blp_relative}"
        VERBATIM)
      list(APPEND compiled_ui_files "${ui_file}")
      list(REMOVE_ITEM resource_inputs "${data_dir}/${ui_relative}")
    endforeach()
    add_custom_target(${blueprint_target} ALL DEPENDS ${compiled_ui_files})
    # First match wins: generated templates must beat the tracked fallback.
    list(APPEND resource_dirs "--sourcedir=${CMAKE_CURRENT_BINARY_DIR}/data")
  else()
    message(WARNING "blueprint-compiler not found - using pre-compiled .ui files")
  endif()
  # Do not search an old generated tree when reconfigured without Blueprint.
  list(APPEND resource_dirs "--sourcedir=${data_dir}")

  add_custom_command(
    OUTPUT "${resource_c}"
    COMMAND "${GLIB_COMPILE_RESOURCES}" --target=${resource_c} --generate-source
            ${resource_dirs} "${resource_xml}"
    DEPENDS "${resource_xml}" ${compiled_ui_files} ${resource_inputs}
    VERBATIM)
  set(${output_variable} "${resource_c}" PARENT_SCOPE)
endfunction()
