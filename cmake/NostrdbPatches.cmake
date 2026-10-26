# Apply the downstream queue to build-directory copies, never the submodule.
include_guard(GLOBAL)

function(nostrc_patch_nostrdb target)
  if(NOT TARGET ${target})
    return()
  endif()
  find_program(NOSTRC_PATCH_EXECUTABLE patch REQUIRED)
  set(_queue "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../third_party/patches/nostrdb")
  set(_overlay "${CMAKE_BINARY_DIR}/nostrdb-patched/${target}")
  get_target_property(_sources ${target} SOURCES)
  set(_vendor "")
  foreach(_source IN LISTS _sources)
    if(_source MATCHES "/src/nostrdb.c$")
      get_filename_component(_src_dir "${_source}" DIRECTORY)
      get_filename_component(_vendor "${_src_dir}" DIRECTORY)
      break()
    endif()
  endforeach()
  if(NOT _vendor)
    message(FATAL_ERROR "${target}: missing nostrdb source; reconcile the patch queue")
  endif()

  # Copy both source directories, including headers: quoted includes must use
  # patched headers too. Only SHA256 needs a copy from CCAN.
  file(GLOB_RECURSE _inputs LIST_DIRECTORIES FALSE CONFIGURE_DEPENDS
    "${_vendor}/src/*" "${_vendor}/deps/lmdb/*"
    "${_vendor}/ccan/ccan/crypto/sha256/*")
  foreach(_input IN LISTS _inputs)
    file(RELATIVE_PATH _relative "${_vendor}" "${_input}")
    configure_file("${_input}" "${_overlay}/${_relative}" COPYONLY)
  endforeach()
  configure_file("${_vendor}/test.c" "${_overlay}/test.c" COPYONLY)
  foreach(_patch IN ITEMS 0001-lmdb-empty-values.patch
      0002-tag-timestamp-alignment.patch 0003-unsigned-varint-shift.patch
      0004-ccan-sha256-alignment.patch 0005-json-token-alignment.patch
      0006-json-packed-kind.patch 0007-packed-note-id.patch
      0008-native-replay-test-length.patch
      0009-content-block-size-alignment.patch 0010-tag-metadata-alignment.patch)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_queue}/${_patch}")
    execute_process(
      COMMAND "${NOSTRC_PATCH_EXECUTABLE}" -f -F 0 -p1 -i "${_queue}/${_patch}"
      WORKING_DIRECTORY "${_overlay}"
      RESULT_VARIABLE _result OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    if(NOT _result EQUAL 0)
      message(FATAL_ERROR "nostrdb patch ${_patch} failed; reconcile the vendor update:\n${_out}${_err}")
    endif()
  endforeach()
  set(_patched_sources "")
  foreach(_source IN LISTS _sources)
    file(RELATIVE_PATH _relative "${_vendor}" "${_source}")
    if(_relative MATCHES "^(src|deps/lmdb|ccan/ccan/crypto/sha256)/")
      list(APPEND _patched_sources "${_overlay}/${_relative}")
    else()
      list(APPEND _patched_sources "${_source}")
    endif()
  endforeach()
  set_property(TARGET ${target} PROPERTY SOURCES "${_patched_sources}")
  target_include_directories(${target} BEFORE PUBLIC
    "${_overlay}/src" "${_overlay}/deps/lmdb" "${_overlay}/ccan")
endfunction()
