# NostrdbVendored.cmake — build the vendored third_party/nostrdb as a
# private static library.
#
#   nostrc_add_vendored_nostrdb(<target> <vendor_path>)
#
# Used by components/nostrdb when libnostr is configured without its own
# nostrdb backend (LIBNOSTR_WITH_NOSTRDB=OFF, the packaged build): the relay
# daemons then carry nostrdb + LMDB + flatcc statically, and libnostr.so --
# and with it nostr-authd / pam_nostr.so (scripts/check-authd-dep-purity.sh)
# -- stays free of them.
#
# The source set and the two compat shims mirror libnostr/CMakeLists.txt's
# LIBNOSTR_WITH_NOSTRDB block, which builds the same tree into libnostr.so.
include_guard(GLOBAL)

function(nostrc_add_vendored_nostrdb target vendor_path)
  if(NOT EXISTS "${vendor_path}/src/nostrdb.c")
    message(FATAL_ERROR
      "nostrdb sources not found at ${vendor_path} "
      "(git submodule update --init third_party/nostrdb)")
  endif()

  file(GLOB _core "${vendor_path}/src/*.c")
  # giftwrap.c needs headers the vendored tree does not ship; libnostr's
  # build excludes it too.
  list(FILTER _core EXCLUDE REGEX ".*/giftwrap\\.c$")
  file(GLOB _bolt11 "${vendor_path}/src/bolt11/*.c")
  file(GLOB_RECURSE _ccan "${vendor_path}/ccan/**/*.c")
  list(FILTER _ccan EXCLUDE REGEX ".*/tools/.*")
  list(FILTER _ccan EXCLUDE REGEX ".*/benchmarks?/.*")
  set(_lmdb "${vendor_path}/deps/lmdb/mdb.c" "${vendor_path}/deps/lmdb/midl.c")

  add_library(${target} STATIC ${_core} ${_bolt11} ${_ccan} ${_lmdb})
  set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)

  set(_compat "${CMAKE_CURRENT_BINARY_DIR}/${target}_compat")
  file(MAKE_DIRECTORY "${_compat}")
  if(NOT EXISTS "${vendor_path}/src/bolt11/talstr.h")
    file(WRITE "${_compat}/talstr.h"
      "#ifndef NOSTRDB_TALSTR_COMPAT_H\n#define NOSTRDB_TALSTR_COMPAT_H\n"
      "#include <ccan/tal/str/str.h>\n#endif\n")
  endif()
  # Reconcile the 3- vs 4-argument hex_encode() between vendored trees.
  file(WRITE "${_compat}/hex_compat.h"
    "#ifndef NOSTRDB_HEX_COMPAT\n#define NOSTRDB_HEX_COMPAT\n#include \"hex.h\"\n"
    "#ifndef hex_str_size\n#define hex_str_size(len) ((size_t)((len) * 2 + 1))\n#endif\n"
    "static inline int nostrdb_hex_encode3(const void *buf, size_t bufsize, char *dest) "
    "{ return hex_encode(buf, bufsize, dest); }\n"
    "#undef hex_encode\n"
    "#define __NDB_HEX_DISPATCH(_1,_2,_3,_4,NAME,...) NAME\n"
    "#define nostrdb_hex_encode4(_buf,_len,_dest,_dsize) nostrdb_hex_encode3(_buf,_len,_dest)\n"
    "#define hex_encode(...) __NDB_HEX_DISPATCH(__VA_ARGS__, nostrdb_hex_encode4, nostrdb_hex_encode3)(__VA_ARGS__)\n"
    "#endif\n")
  target_compile_options(${target} PRIVATE -include "${_compat}/hex_compat.h")

  target_include_directories(${target} PUBLIC
    "${vendor_path}/src"
    "${vendor_path}/src/bolt11"
    "${vendor_path}/ccan"
    "${vendor_path}/ccan/ccan"
    "${vendor_path}/ccan/ccan/array_size"
    "${vendor_path}/ccan/ccan/mem"
    "${vendor_path}/ccan/ccan/short_types"
    "${vendor_path}/ccan/ccan/crypto/sha256"
    "${vendor_path}/ccan/ccan/tal"
    "${vendor_path}/ccan/ccan/tal/str"
    "${vendor_path}/deps/lmdb"
    "${vendor_path}/deps/flatcc/include"
    "${vendor_path}/deps/secp256k1/include"
    "${_compat}")

  find_package(PkgConfig REQUIRED)
  pkg_check_modules(NOSTRDB_SODIUM REQUIRED IMPORTED_TARGET libsodium)
  find_package(Threads REQUIRED)
  find_library(NOSTRDB_SECP256K1_LIB NAMES secp256k1 REQUIRED)
  target_link_libraries(${target} PUBLIC
    PkgConfig::NOSTRDB_SODIUM Threads::Threads ${NOSTRDB_SECP256K1_LIB})

  # Always the vendored flatcc runtime: it must match the vendored headers,
  # and linking it statically keeps flatccrt out of the runtime closure.
  file(GLOB _flatcc_rt "${vendor_path}/deps/flatcc/src/runtime/*.c")
  target_sources(${target} PRIVATE ${_flatcc_rt})

  if(APPLE)
    target_compile_options(${target} PRIVATE -Wno-deprecated-declarations)
    target_link_libraries(${target} PUBLIC "-framework Security")
  endif()
endfunction()
