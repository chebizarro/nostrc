add_executable(fips_core_skeleton_test test_core.c)
target_link_libraries(fips_core_skeleton_test PRIVATE fips_core)
set_target_properties(fips_core_skeleton_test PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
apply_warnings(fips_core_skeleton_test)
apply_sanitizers(fips_core_skeleton_test)
add_test(NAME FipsCoreSkeletonTest COMMAND fips_core_skeleton_test)
set_tests_properties(FipsCoreSkeletonTest PROPERTIES TIMEOUT 10 LABELS "fips;unit")
nostrc_describe_test(FipsCoreSkeletonTest OWNER libfips-core TARGETS fips_core_skeleton_test)

add_executable(fips_core_host_test test_host.c fakes/host.c)
target_link_libraries(fips_core_host_test PRIVATE fips_core)
set_target_properties(fips_core_host_test PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
apply_warnings(fips_core_host_test)
apply_sanitizers(fips_core_host_test)
add_test(NAME FipsCoreHostTest COMMAND fips_core_host_test)
set_tests_properties(FipsCoreHostTest PROPERTIES TIMEOUT 10 LABELS "fips;unit")
nostrc_describe_test(FipsCoreHostTest OWNER libfips-core TARGETS fips_core_host_test
  INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/seeded_vectors.h"
         "${CMAKE_CURRENT_SOURCE_DIR}/generate_seeded_vectors.py")

add_subdirectory(diff)

add_executable(fips_core_codec_test test_codec.c)
target_link_libraries(fips_core_codec_test PRIVATE fips_core)
set_target_properties(fips_core_codec_test PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
apply_warnings(fips_core_codec_test)
apply_sanitizers(fips_core_codec_test)
add_test(NAME FipsCoreCodecTest COMMAND fips_core_codec_test)
set_tests_properties(FipsCoreCodecTest PROPERTIES TIMEOUT 10 LABELS "fips;unit")
nostrc_describe_test(FipsCoreCodecTest OWNER libfips-core TARGETS fips_core_codec_test)
# Keep the existing explicit FIPS CI build closure complete.
add_dependencies(fips_core_host_test fips_core_codec_test)
add_executable(fips_core_transport_types_test test_transport_types.c)
target_link_libraries(fips_core_transport_types_test PRIVATE fips_core)
set_target_properties(fips_core_transport_types_test PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
apply_warnings(fips_core_transport_types_test)
apply_sanitizers(fips_core_transport_types_test)
add_test(NAME FipsCoreTransportTypesTest COMMAND fips_core_transport_types_test)
set_tests_properties(FipsCoreTransportTypesTest PROPERTIES TIMEOUT 10 LABELS "fips;unit")
nostrc_describe_test(FipsCoreTransportTypesTest OWNER libfips-core TARGETS fips_core_transport_types_test)
add_executable(fips_core_rate_limit_test test_rate_limit.c fakes/host.c)
target_link_libraries(fips_core_rate_limit_test PRIVATE fips_core)
set_target_properties(fips_core_rate_limit_test PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
apply_warnings(fips_core_rate_limit_test)
apply_sanitizers(fips_core_rate_limit_test)
add_test(NAME FipsCoreRateLimitTest COMMAND fips_core_rate_limit_test)
set_tests_properties(FipsCoreRateLimitTest PROPERTIES TIMEOUT 10 LABELS "fips;unit")
nostrc_describe_test(FipsCoreRateLimitTest OWNER libfips-core TARGETS fips_core_rate_limit_test)

add_executable(fips_core_stats_history_test test_stats_history.c fakes/host.c)
target_link_libraries(fips_core_stats_history_test PRIVATE fips_core)
set_target_properties(fips_core_stats_history_test PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
apply_warnings(fips_core_stats_history_test)
apply_sanitizers(fips_core_stats_history_test)
add_test(NAME FipsCoreStatsHistoryTest COMMAND fips_core_stats_history_test)
set_tests_properties(FipsCoreStatsHistoryTest PROPERTIES TIMEOUT 10 LABELS "fips;unit")
nostrc_describe_test(FipsCoreStatsHistoryTest OWNER libfips-core TARGETS fips_core_stats_history_test)

# FIPS CI builds the established host-test entrypoint explicitly before ctest.
# Keep these registered tests in that build closure without changing CI lanes.
add_dependencies(fips_core_host_test fips_core_rate_limit_test fips_core_stats_history_test)

# Shared utility/math ports (nostrc-u50yr.3), independent of sibling blocks.
add_executable(fips_core_utils_test test_utils.c fakes/host.c)
target_link_libraries(fips_core_utils_test PRIVATE fips_core)
set_target_properties(fips_core_utils_test PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
apply_warnings(fips_core_utils_test)
apply_sanitizers(fips_core_utils_test)
add_test(NAME FipsCoreUtilsTest COMMAND fips_core_utils_test)
set_tests_properties(FipsCoreUtilsTest PROPERTIES TIMEOUT 10 LABELS "fips;unit")
nostrc_describe_test(FipsCoreUtilsTest OWNER libfips-core TARGETS fips_core_utils_test)

add_executable(fips_core_math_test test_math.c)
target_link_libraries(fips_core_math_test PRIVATE fips_core)
set_target_properties(fips_core_math_test PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
apply_warnings(fips_core_math_test)
apply_sanitizers(fips_core_math_test)
add_test(NAME FipsCoreMathTest COMMAND fips_core_math_test)
set_tests_properties(FipsCoreMathTest PROPERTIES TIMEOUT 10 LABELS "fips;unit")
nostrc_describe_test(FipsCoreMathTest OWNER libfips-core TARGETS fips_core_math_test)
add_dependencies(fips_core_host_test fips_core_utils_test fips_core_math_test)
# Identity behavioral ports and C ownership/bounds cases.
add_executable(fips_core_identity_test test_identity.c fakes/host.c)
target_link_libraries(fips_core_identity_test PRIVATE fips_core PkgConfig::FIPS_CORE_SODIUM)
set_target_properties(fips_core_identity_test PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
apply_warnings(fips_core_identity_test)
apply_sanitizers(fips_core_identity_test)
add_test(NAME FipsCoreIdentityTest COMMAND fips_core_identity_test)
set_tests_properties(FipsCoreIdentityTest PROPERTIES TIMEOUT 10 LABELS "fips;unit")
nostrc_describe_test(FipsCoreIdentityTest OWNER libfips-core TARGETS fips_core_identity_test)
add_dependencies(fips_core_host_test fips_core_identity_test)
add_executable(fips_core_coord_cache_test test_coord_cache.c fakes/host.c)
target_link_libraries(fips_core_coord_cache_test PRIVATE fips_core)
set_target_properties(fips_core_coord_cache_test PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
apply_warnings(fips_core_coord_cache_test)
apply_sanitizers(fips_core_coord_cache_test)
add_test(NAME FipsCoreCoordCacheTest COMMAND fips_core_coord_cache_test)
# Default-capacity fills (50,000 entries) run under sanitizers.
set_tests_properties(FipsCoreCoordCacheTest PROPERTIES TIMEOUT 120 LABELS "fips;unit")
nostrc_describe_test(FipsCoreCoordCacheTest OWNER libfips-core TARGETS fips_core_coord_cache_test)
# Keep the explicit FIPS CI build closure complete (see the host-test note above).
add_dependencies(fips_core_host_test fips_core_coord_cache_test)

# Compile the same ECDH source with hooks in this executable only. The library
# remains uninstrumented; its separate ECDH object is not needed by this target.
add_executable(fips_core_ecdh_test test_ecdh.c fakes/host.c ../src/noise/ecdh.c)
target_link_libraries(fips_core_ecdh_test PRIVATE fips_core
  PkgConfig::FIPS_CORE_SODIUM PkgConfig::FIPS_CORE_SECP256K1)
target_compile_definitions(fips_core_ecdh_test PRIVATE FIPS_TEST_HOOKS FIPS_CORE_STATIC_DEFINE)
set_target_properties(fips_core_ecdh_test PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
apply_warnings(fips_core_ecdh_test)
apply_sanitizers(fips_core_ecdh_test)
add_test(NAME FipsCoreEcdhTest COMMAND fips_core_ecdh_test)
set_tests_properties(FipsCoreEcdhTest PROPERTIES TIMEOUT 10 LABELS "fips;unit")
nostrc_describe_test(FipsCoreEcdhTest OWNER libfips-core TARGETS fips_core_ecdh_test
  INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/ecdh_vectors.py")
add_dependencies(fips_core_host_test fips_core_ecdh_test)
# Noise AEAD vectors and failure atomicity (nostrc-pwo3h.1).
# The test compiles the backend itself (hidden in the shared library).
find_package(Threads REQUIRED)
add_executable(fips_core_aead_test test_aead.c ../src/noise/aead_primitive.c)
target_compile_definitions(fips_core_aead_test PRIVATE ${FIPS_AEAD_BACKEND_DEFINE})
target_link_libraries(fips_core_aead_test PRIVATE fips_core PkgConfig::FIPS_CORE_SODIUM
  OpenSSL::Crypto Threads::Threads)
set_target_properties(fips_core_aead_test PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
apply_warnings(fips_core_aead_test)
apply_sanitizers(fips_core_aead_test)
add_test(NAME FipsCoreAeadTest COMMAND fips_core_aead_test)
set_tests_properties(FipsCoreAeadTest PROPERTIES TIMEOUT 10 LABELS "fips;unit")
nostrc_describe_test(FipsCoreAeadTest OWNER libfips-core TARGETS fips_core_aead_test)
add_dependencies(fips_core_host_test fips_core_aead_test)
# OpenSSL backend: no key/keystream residue in OpenSSL's heap after an op (nostrc-u19cz.2).
if(FIPS_AEAD_BACKEND STREQUAL "openssl")
  add_executable(fips_core_aead_residue_test test_aead_residue.c ../src/noise/aead_primitive.c)
  target_compile_definitions(fips_core_aead_residue_test PRIVATE ${FIPS_AEAD_BACKEND_DEFINE})
  target_link_libraries(fips_core_aead_residue_test PRIVATE fips_core
    PkgConfig::FIPS_CORE_SODIUM OpenSSL::Crypto Threads::Threads)
  set_target_properties(fips_core_aead_residue_test PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
  apply_warnings(fips_core_aead_residue_test)
  apply_sanitizers(fips_core_aead_residue_test)
  add_test(NAME FipsCoreAeadResidueTest COMMAND fips_core_aead_residue_test)
  set_tests_properties(FipsCoreAeadResidueTest PROPERTIES TIMEOUT 30 LABELS "fips;unit")
  nostrc_describe_test(FipsCoreAeadResidueTest OWNER libfips-core
    TARGETS fips_core_aead_residue_test)
  add_dependencies(fips_core_host_test fips_core_aead_residue_test)
endif()
# Tree coordinate codec and routing helpers (nostrc-82g9n.1).
add_executable(fips_core_coord_test test_coord.c)
target_link_libraries(fips_core_coord_test PRIVATE fips_core)
set_target_properties(fips_core_coord_test PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
apply_warnings(fips_core_coord_test)
apply_sanitizers(fips_core_coord_test)
add_test(NAME FipsCoreCoordTest COMMAND fips_core_coord_test)
set_tests_properties(FipsCoreCoordTest PROPERTIES TIMEOUT 10 LABELS "fips;unit")
nostrc_describe_test(FipsCoreCoordTest OWNER libfips-core TARGETS fips_core_coord_test)
add_dependencies(fips_core_host_test fips_core_coord_test)
# Link catalog, session datagram and framing math (nostrc-82g9n.6). The test also
# replays the pinned proto fixture through the harness loader, so it links the
# test-only loader/schema and Jansson exactly as fips_diff does and reuses its
# vectors/pin definitions rather than a second hand-maintained constant.
pkg_check_modules(FIPS_LINK_JANSSON REQUIRED IMPORTED_TARGET jansson)
add_executable(fips_core_link_test test_link.c diff/loader.c diff/schema.c diff/proto_link.c fakes/host.c)
get_target_property(fips_link_diff_definitions fips_diff COMPILE_DEFINITIONS)
target_compile_definitions(fips_core_link_test PRIVATE ${fips_link_diff_definitions})
target_link_libraries(fips_core_link_test PRIVATE fips_core PkgConfig::FIPS_LINK_JANSSON OpenSSL::Crypto)
set_target_properties(fips_core_link_test PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
apply_warnings(fips_core_link_test)
apply_sanitizers(fips_core_link_test)
add_test(NAME FipsCoreLinkTest COMMAND fips_core_link_test)
set_tests_properties(FipsCoreLinkTest PROPERTIES TIMEOUT 30 LABELS "fips;unit")
nostrc_describe_test(FipsCoreLinkTest OWNER libfips-core TARGETS fips_core_link_test
  INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/vectors/proto.json"
         "${CMAKE_CURRENT_SOURCE_DIR}/vectors/proto-session-datagram.bin"
         "${CMAKE_CURRENT_SOURCE_DIR}/../UPSTREAM.md")
add_dependencies(fips_core_host_test fips_core_link_test)

# FMP lifecycle upstream ports and C bounded-storage/ingress regressions.
add_executable(fips_core_fmp_test test_fmp.c fakes/host.c)
target_link_libraries(fips_core_fmp_test PRIVATE fips_core)
set_target_properties(fips_core_fmp_test PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
apply_warnings(fips_core_fmp_test)
apply_sanitizers(fips_core_fmp_test)
add_test(NAME FipsCoreFmpTest COMMAND fips_core_fmp_test)
set_tests_properties(FipsCoreFmpTest PROPERTIES TIMEOUT 10 LABELS "fips;unit")
nostrc_describe_test(FipsCoreFmpTest OWNER libfips-core TARGETS fips_core_fmp_test)

add_executable(fips_core_fmp_vectors_test test_fmp_vectors.c
  diff/loader.c diff/schema.c diff/proto_fmp.c diff/proto_link.c fakes/host.c)
get_target_property(fips_fmp_diff_definitions fips_diff COMPILE_DEFINITIONS)
target_compile_definitions(fips_core_fmp_vectors_test PRIVATE ${fips_fmp_diff_definitions})
target_link_libraries(fips_core_fmp_vectors_test PRIVATE
  fips_core PkgConfig::FIPS_LINK_JANSSON OpenSSL::Crypto)
set_target_properties(fips_core_fmp_vectors_test PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
apply_warnings(fips_core_fmp_vectors_test)
apply_sanitizers(fips_core_fmp_vectors_test)
add_test(NAME FipsCoreFmpVectorsTest COMMAND fips_core_fmp_vectors_test)
set_tests_properties(FipsCoreFmpVectorsTest PROPERTIES TIMEOUT 30 LABELS "fips;diff")
file(GLOB fmp_vector_inputs CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/vectors/*")
nostrc_describe_test(FipsCoreFmpVectorsTest OWNER libfips-core TARGETS fips_core_fmp_vectors_test
  INPUTS ${fmp_vector_inputs} "${CMAKE_CURRENT_SOURCE_DIR}/../UPSTREAM.md")
add_dependencies(fips_core_host_test fips_core_fmp_test fips_core_fmp_vectors_test)
# Spanning-tree state, wire and regression tests (nostrc-82g9n.2).
add_executable(fips_core_stp_test test_stp.c fakes/host.c)
target_link_libraries(fips_core_stp_test PRIVATE fips_core)
set_target_properties(fips_core_stp_test PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
apply_warnings(fips_core_stp_test)
apply_sanitizers(fips_core_stp_test)
add_test(NAME FipsCoreStpTest COMMAND fips_core_stp_test)
set_tests_properties(FipsCoreStpTest PROPERTIES TIMEOUT 30 LABELS "fips;unit")
nostrc_describe_test(FipsCoreStpTest OWNER libfips-core TARGETS fips_core_stp_test)

add_executable(fips_core_stp_vectors_test test_stp_vectors.c diff/proto_stp.c diff/loader.c diff/schema.c fakes/host.c)
get_target_property(fips_stp_diff_definitions fips_diff COMPILE_DEFINITIONS)
target_compile_definitions(fips_core_stp_vectors_test PRIVATE ${fips_stp_diff_definitions})
target_link_libraries(fips_core_stp_vectors_test PRIVATE fips_core PkgConfig::FIPS_LINK_JANSSON OpenSSL::Crypto)
set_target_properties(fips_core_stp_vectors_test PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
apply_warnings(fips_core_stp_vectors_test)
apply_sanitizers(fips_core_stp_vectors_test)
add_test(NAME FipsCoreStpVectorsTest COMMAND fips_core_stp_vectors_test)
set_tests_properties(FipsCoreStpVectorsTest PROPERTIES TIMEOUT 30 LABELS "fips;diff")
nostrc_describe_test(FipsCoreStpVectorsTest OWNER libfips-core TARGETS fips_core_stp_vectors_test
  INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/vectors/proto.json" "${CMAKE_CURRENT_SOURCE_DIR}/../UPSTREAM.md")
add_dependencies(fips_core_host_test fips_core_stp_test fips_core_stp_vectors_test)
# Noise SymmetricState (nostrc-pwo3h.3): RFC 4231/5869 vectors through the private
# HKDF header, both noise.json constructed-step hashes (seeded responder key via
# the identity layer and fakes/host.c), the Python-pinned MixKey/MixHash/Split
# sequences, round trips, tamper/fork behaviour and destroy zeroization.
add_executable(fips_core_symmetric_test test_symmetric.c fakes/host.c)
target_link_libraries(fips_core_symmetric_test PRIVATE fips_core PkgConfig::FIPS_CORE_SODIUM)
set_target_properties(fips_core_symmetric_test PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
apply_warnings(fips_core_symmetric_test)
apply_sanitizers(fips_core_symmetric_test)
add_test(NAME FipsCoreSymmetricTest COMMAND fips_core_symmetric_test)
set_tests_properties(FipsCoreSymmetricTest PROPERTIES TIMEOUT 10 LABELS "fips;unit")
nostrc_describe_test(FipsCoreSymmetricTest OWNER libfips-core TARGETS fips_core_symmetric_test
  INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/noise_symmetric_vectors.py"
         "${CMAKE_CURRENT_SOURCE_DIR}/tools/ecdh_vectors.py"
         "${CMAKE_CURRENT_SOURCE_DIR}/vectors/noise.json")
add_dependencies(fips_core_host_test fips_core_symmetric_test)
# Bloom filters, bounded announcement state and protocol vectors (nostrc-82g9n.3).
add_executable(fips_core_bloom_test test_bloom.c)
target_link_libraries(fips_core_bloom_test PRIVATE fips_core OpenSSL::Crypto)
set_target_properties(fips_core_bloom_test PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
apply_warnings(fips_core_bloom_test)
apply_sanitizers(fips_core_bloom_test)
add_test(NAME FipsCoreBloomTest COMMAND fips_core_bloom_test)
set_tests_properties(FipsCoreBloomTest PROPERTIES TIMEOUT 30 LABELS "fips;unit")
nostrc_describe_test(FipsCoreBloomTest OWNER libfips-core TARGETS fips_core_bloom_test
  INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/bloom_pins.h" "${CMAKE_CURRENT_SOURCE_DIR}/tools/bloom_vectors.py")
add_executable(fips_core_bloom_vectors_test test_bloom_vectors.c diff/proto_bloom.c diff/loader.c diff/schema.c fakes/host.c)
get_target_property(fips_bloom_diff_definitions fips_diff COMPILE_DEFINITIONS)
target_compile_definitions(fips_core_bloom_vectors_test PRIVATE ${fips_bloom_diff_definitions})
target_link_libraries(fips_core_bloom_vectors_test PRIVATE fips_core PkgConfig::FIPS_LINK_JANSSON OpenSSL::Crypto)
set_target_properties(fips_core_bloom_vectors_test PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
apply_warnings(fips_core_bloom_vectors_test)
apply_sanitizers(fips_core_bloom_vectors_test)
add_test(NAME FipsCoreBloomVectorsTest COMMAND fips_core_bloom_vectors_test)
set_tests_properties(FipsCoreBloomVectorsTest PROPERTIES TIMEOUT 30 LABELS "fips;unit;diff")
nostrc_describe_test(FipsCoreBloomVectorsTest OWNER libfips-core TARGETS fips_core_bloom_vectors_test
  INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/vectors/proto.json"
         "${CMAKE_CURRENT_SOURCE_DIR}/vectors/proto-bloom.bin"
         "${CMAKE_CURRENT_SOURCE_DIR}/vectors/proto-filter-announce.bin"
         "${CMAKE_CURRENT_SOURCE_DIR}/../UPSTREAM.md")
add_dependencies(fips_core_host_test fips_core_bloom_test fips_core_bloom_vectors_test)
# Keep the explicit FIPS CI build closure complete (transport types test was missing).
add_dependencies(fips_core_host_test fips_core_transport_types_test)

# FSP wire, stateless plans, bounded queues and link-quorum ports (nostrc-82g9n.8).
add_executable(fips_core_fsp_test test_fsp.c)
target_link_libraries(fips_core_fsp_test PRIVATE fips_core)
set_target_properties(fips_core_fsp_test PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
apply_warnings(fips_core_fsp_test)
apply_sanitizers(fips_core_fsp_test)
add_test(NAME FipsCoreFspTest COMMAND fips_core_fsp_test)
set_tests_properties(FipsCoreFspTest PROPERTIES TIMEOUT 30 LABELS "fips;unit")
nostrc_describe_test(FipsCoreFspTest OWNER libfips-core TARGETS fips_core_fsp_test)

add_executable(fips_core_fsp_vectors_test test_fsp_vectors.c
  diff/loader.c diff/schema.c diff/proto_fsp.c fakes/host.c)
get_target_property(fips_fsp_diff_definitions fips_diff COMPILE_DEFINITIONS)
target_compile_definitions(fips_core_fsp_vectors_test PRIVATE ${fips_fsp_diff_definitions})
target_link_libraries(fips_core_fsp_vectors_test PRIVATE
  fips_core PkgConfig::FIPS_LINK_JANSSON OpenSSL::Crypto)
set_target_properties(fips_core_fsp_vectors_test PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
apply_warnings(fips_core_fsp_vectors_test)
apply_sanitizers(fips_core_fsp_vectors_test)
add_test(NAME FipsCoreFspVectorsTest COMMAND fips_core_fsp_vectors_test)
set_tests_properties(FipsCoreFspVectorsTest PROPERTIES TIMEOUT 30 LABELS "fips;diff")
file(GLOB fsp_vector_inputs CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/vectors/*")
nostrc_describe_test(FipsCoreFspVectorsTest OWNER libfips-core TARGETS fips_core_fsp_vectors_test
  INPUTS ${fsp_vector_inputs} "${CMAKE_CURRENT_SOURCE_DIR}/../UPSTREAM.md")
add_dependencies(fips_core_host_test fips_core_fsp_test fips_core_fsp_vectors_test)
# Routing upstream unit-test ports and C buffer/refusal regressions.
add_executable(fips_core_routing_test test_routing.c fakes/host.c)
target_link_libraries(fips_core_routing_test PRIVATE fips_core)
set_target_properties(fips_core_routing_test PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
apply_warnings(fips_core_routing_test)
apply_sanitizers(fips_core_routing_test)
add_test(NAME FipsCoreRoutingTest COMMAND fips_core_routing_test)
set_tests_properties(FipsCoreRoutingTest PROPERTIES TIMEOUT 30 LABELS "fips;unit")
nostrc_describe_test(FipsCoreRoutingTest OWNER libfips-core TARGETS fips_core_routing_test)
add_dependencies(fips_core_host_test fips_core_routing_test)
# Lookup retry ladder, bounded state, signing, and isolated differential replay.
add_executable(fips_core_lookup_test test_lookup.c fakes/host.c)
target_link_libraries(fips_core_lookup_test PRIVATE fips_core)
set_target_properties(fips_core_lookup_test PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
apply_warnings(fips_core_lookup_test)
apply_sanitizers(fips_core_lookup_test)
add_test(NAME FipsCoreLookupTest COMMAND fips_core_lookup_test)
set_tests_properties(FipsCoreLookupTest PROPERTIES TIMEOUT 120 LABELS "fips;unit")
nostrc_describe_test(FipsCoreLookupTest OWNER libfips-core TARGETS fips_core_lookup_test)

add_executable(fips_core_lookup_vectors_test test_lookup_vectors.c
  diff/proto_lookup.c diff/loader.c diff/schema.c fakes/host.c)
get_target_property(fips_lookup_diff_definitions fips_diff COMPILE_DEFINITIONS)
target_compile_definitions(fips_core_lookup_vectors_test PRIVATE ${fips_lookup_diff_definitions})
target_link_libraries(fips_core_lookup_vectors_test PRIVATE fips_core PkgConfig::FIPS_LINK_JANSSON OpenSSL::Crypto)
set_target_properties(fips_core_lookup_vectors_test PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
apply_warnings(fips_core_lookup_vectors_test)
apply_sanitizers(fips_core_lookup_vectors_test)
add_test(NAME FipsCoreLookupVectorsTest COMMAND fips_core_lookup_vectors_test)
set_tests_properties(FipsCoreLookupVectorsTest PROPERTIES TIMEOUT 30 LABELS "fips;diff")
nostrc_describe_test(FipsCoreLookupVectorsTest OWNER libfips-core TARGETS fips_core_lookup_vectors_test
  INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/vectors/proto.json"
         "${CMAKE_CURRENT_SOURCE_DIR}/vectors/proto-lookup-request.bin"
         "${CMAKE_CURRENT_SOURCE_DIR}/vectors/proto-lookup-response.bin"
         "${CMAKE_CURRENT_SOURCE_DIR}/../UPSTREAM.md")
add_dependencies(fips_core_host_test fips_core_lookup_test fips_core_lookup_vectors_test)
# IK/XK/Fork fixture tamper and C ownership cases (nostrc-pwo3h.4/.5/.6).
# Compile ECDH + handshake with test hooks here, never instrument production.
add_executable(fips_core_handshake_test test_handshake.c fakes/host.c
  diff/loader.c diff/schema.c ../src/noise/handshake.c ../src/noise/ecdh.c)
get_target_property(fips_handshake_diff_definitions fips_diff COMPILE_DEFINITIONS)
target_compile_definitions(fips_core_handshake_test PRIVATE
  ${fips_handshake_diff_definitions} FIPS_TEST_HOOKS FIPS_CORE_STATIC_DEFINE)
target_link_libraries(fips_core_handshake_test PRIVATE fips_core
  PkgConfig::FIPS_CORE_SODIUM PkgConfig::FIPS_CORE_SECP256K1
  PkgConfig::FIPS_LINK_JANSSON OpenSSL::Crypto)
set_target_properties(fips_core_handshake_test PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
apply_warnings(fips_core_handshake_test)
apply_sanitizers(fips_core_handshake_test)
add_test(NAME FipsCoreHandshakeTest COMMAND fips_core_handshake_test)
set_tests_properties(FipsCoreHandshakeTest PROPERTIES TIMEOUT 30 LABELS "fips;unit")
nostrc_describe_test(FipsCoreHandshakeTest OWNER libfips-core TARGETS fips_core_handshake_test
  INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/vectors/noise.json"
         "${CMAKE_CURRENT_SOURCE_DIR}/../UPSTREAM.md")
add_dependencies(fips_core_host_test fips_core_handshake_test)

# Transport sessions/replay and explicit worker ownership (nostrc-pwo3h.7).
# Compile cipher with test-only overflow setup, never expose hooks in production.
add_executable(fips_core_session_test test_session.c ../src/noise/cipher.c)
target_compile_definitions(fips_core_session_test PRIVATE FIPS_TEST_HOOKS FIPS_CORE_STATIC_DEFINE)
target_link_libraries(fips_core_session_test PRIVATE fips_core PkgConfig::FIPS_CORE_SODIUM)
set_target_properties(fips_core_session_test PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
apply_warnings(fips_core_session_test)
apply_sanitizers(fips_core_session_test)
add_test(NAME FipsCoreSessionTest COMMAND fips_core_session_test)
set_tests_properties(FipsCoreSessionTest PROPERTIES TIMEOUT 30 LABELS "fips;unit")
nostrc_describe_test(FipsCoreSessionTest OWNER libfips-core TARGETS fips_core_session_test)
add_dependencies(fips_core_host_test fips_core_session_test)
# Noise negative space (nostrc-pwo3h.8): tamper, length, replay, nonce, wrong-key
# and misuse matrices with exact error variants, DH cost, untouched fork originals,
# an explicit allow-list for every accepted mutation and zeroization probes.
# Handshake, ECDH and cipher are compiled with test hooks here (DH counter,
# counter injection); the production library stays uninstrumented. The point
# validity oracle is the test's own libsecp256k1 context, not the port's.
add_executable(fips_core_noise_negative_test test_noise_negative.c fakes/host.c
  ../src/noise/handshake.c ../src/noise/ecdh.c ../src/noise/cipher.c)
# These test executables compile private Noise sources with hooks, rather than
# using only the library objects; give them the same private source include root.
foreach(target IN ITEMS fips_core_ecdh_test fips_core_handshake_test fips_core_noise_negative_test)
  target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
endforeach()
target_compile_definitions(fips_core_noise_negative_test PRIVATE FIPS_TEST_HOOKS FIPS_CORE_STATIC_DEFINE)
target_link_libraries(fips_core_noise_negative_test PRIVATE fips_core
  PkgConfig::FIPS_CORE_SODIUM PkgConfig::FIPS_CORE_SECP256K1)
set_target_properties(fips_core_noise_negative_test PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
apply_warnings(fips_core_noise_negative_test)
apply_sanitizers(fips_core_noise_negative_test)
add_test(NAME FipsCoreNoiseNegativeTest COMMAND fips_core_noise_negative_test)
# Every transport length 0..MAX+1 is opened; allow for sanitizer builds.
set_tests_properties(FipsCoreNoiseNegativeTest PROPERTIES TIMEOUT 180 LABELS "fips;unit;negative")
nostrc_describe_test(FipsCoreNoiseNegativeTest OWNER libfips-core TARGETS fips_core_noise_negative_test)
add_dependencies(fips_core_host_test fips_core_noise_negative_test)
# Constant-time probe for AEAD open failures (nostrc-pwo3h.8): built in the CI
# closure so it cannot rot, never registered with ctest because wall-clock timing
# is machine-dependent. Run by hand: fips_core_ct_probe [measurements]; method and
# results are in docs/reviews/libfips-core-noise-audit-2026-11-15.md.
add_executable(fips_core_ct_probe tools/ct_probe.c)
target_link_libraries(fips_core_ct_probe PRIVATE fips_core m)
if(WIN32)
  target_link_libraries(fips_core_ct_probe PRIVATE pthread) # mingw's clock_gettime is in winpthreads
endif()
set_target_properties(fips_core_ct_probe PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
apply_warnings(fips_core_ct_probe)
apply_sanitizers(fips_core_ct_probe)
add_dependencies(fips_core_host_test fips_core_ct_probe)
# Tier A3 proto properties and locked differential sweep (nostrc-82g9n.9).
add_executable(fips_core_proto_properties_test test_proto_properties.c fakes/host.c)
get_target_property(fips_properties_diff_definitions fips_diff COMPILE_DEFINITIONS)
target_compile_definitions(fips_core_proto_properties_test PRIVATE ${fips_properties_diff_definitions})
target_link_libraries(fips_core_proto_properties_test PRIVATE fips_core PkgConfig::FIPS_LINK_JANSSON)
set_target_properties(fips_core_proto_properties_test PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
apply_warnings(fips_core_proto_properties_test)
apply_sanitizers(fips_core_proto_properties_test)
add_test(NAME FipsCoreProtoPropertiesTest COMMAND fips_core_proto_properties_test)
# A fixed, seeded, single-threaded workload: it cannot hang, only run slowly.
# GCC ASan gives each codec primitive's small local a use-after-return fake
# frame; clang's, with the same option, does not instrument them. About 20 s
# under GCC ASan against 0.3 s under clang's left 60 s too little headroom on
# a loaded host (nostrc-aqyad).
set_tests_properties(FipsCoreProtoPropertiesTest PROPERTIES TIMEOUT 180 LABELS "fips;unit;proto")
nostrc_describe_test(FipsCoreProtoPropertiesTest OWNER libfips-core TARGETS fips_core_proto_properties_test
  INPUTS ${fsp_vector_inputs})
add_dependencies(fips_core_host_test fips_core_proto_properties_test)

# Diagnostic probe: all upstream stage/path tests with synthetic host time.
add_executable(fips_core_probe_test test_probe.c fakes/host.c)
target_link_libraries(fips_core_probe_test PRIVATE fips_core)
set_target_properties(fips_core_probe_test PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
apply_warnings(fips_core_probe_test)
apply_sanitizers(fips_core_probe_test)
add_test(NAME FipsCoreProbeTest COMMAND fips_core_probe_test)
set_tests_properties(FipsCoreProbeTest PROPERTIES TIMEOUT 30 LABELS "fips;unit")
nostrc_describe_test(FipsCoreProbeTest OWNER libfips-core TARGETS fips_core_probe_test)
add_dependencies(fips_core_host_test fips_core_probe_test)
# MMP unit ports and independent per-name differential replay (nostrc-f12v2.4).
add_executable(fips_core_mmp_test test_mmp.c)
target_link_libraries(fips_core_mmp_test PRIVATE fips_core)
set_target_properties(fips_core_mmp_test PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
apply_warnings(fips_core_mmp_test)
apply_sanitizers(fips_core_mmp_test)
add_test(NAME FipsCoreMmpTest COMMAND fips_core_mmp_test)
set_tests_properties(FipsCoreMmpTest PROPERTIES TIMEOUT 30 LABELS "fips;unit")
nostrc_describe_test(FipsCoreMmpTest OWNER libfips-core TARGETS fips_core_mmp_test
  INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/mmp_reference.py")
add_executable(fips_core_mmp_vectors_test test_mmp_vectors.c diff/proto_mmp.c
  diff/loader.c diff/schema.c diff/proto_link.c fakes/host.c)
get_target_property(fips_mmp_diff_definitions fips_diff COMPILE_DEFINITIONS)
target_compile_definitions(fips_core_mmp_vectors_test PRIVATE ${fips_mmp_diff_definitions})
target_link_libraries(fips_core_mmp_vectors_test PRIVATE fips_core PkgConfig::FIPS_LINK_JANSSON OpenSSL::Crypto)
set_target_properties(fips_core_mmp_vectors_test PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
apply_warnings(fips_core_mmp_vectors_test)
apply_sanitizers(fips_core_mmp_vectors_test)
add_test(NAME FipsCoreMmpVectorsTest COMMAND fips_core_mmp_vectors_test)
set_tests_properties(FipsCoreMmpVectorsTest PROPERTIES TIMEOUT 30 LABELS "fips;diff")
nostrc_describe_test(FipsCoreMmpVectorsTest OWNER libfips-core TARGETS fips_core_mmp_vectors_test
  INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/vectors/proto.json" "${CMAKE_CURRENT_SOURCE_DIR}/../UPSTREAM.md")
add_dependencies(fips_core_host_test fips_core_mmp_test fips_core_mmp_vectors_test)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME FipsCoreMmpPinsTest COMMAND "${Python3_EXECUTABLE}" -I
  "${CMAKE_CURRENT_SOURCE_DIR}/tools/mmp_reference.py" --check "${CMAKE_CURRENT_SOURCE_DIR}/mmp_pins.h")
set_tests_properties(FipsCoreMmpPinsTest PROPERTIES TIMEOUT 10 LABELS "fips;unit")
nostrc_describe_test(FipsCoreMmpPinsTest OWNER libfips-core
  INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/mmp_reference.py" "${CMAKE_CURRENT_SOURCE_DIR}/mmp_pins.h")
# Shared rekey machine, deterministic profile and explicit key/lease ownership.
# Compile the shared sources with private hooks only in this unit target.
add_executable(fips_core_rekey_test test_rekey.c test_rekey_params.c
  test_rekey_fmp.c test_rekey_fsp.c test_rekey_next.c test_rekey_crossing.c fakes/rekey.c fakes/host.c
  ../src/proto/rekey/mod.c ../src/proto/rekey/machine.c ../src/proto/rekey/params.c
  ../src/proto/rekey/profile.c ../src/proto/rekey/hooks.c ../src/proto/rekey/access.c
  ../src/proto/rekey/candidates.c ../src/proto/rekey/cycle.c ../src/proto/rekey/epoch.c
  ../src/proto/rekey/keys.c ../src/proto/rekey/ladder.c ../src/proto/rekey/lease.c
  ../src/proto/rekey/proof.c ../src/proto/rekey/reason.c ../src/proto/rekey/record.c
  ../src/proto/rekey/resolve.c)
target_compile_definitions(fips_core_rekey_test PRIVATE FIPS_TEST_HOOKS FIPS_CORE_STATIC_DEFINE
  FIPS_REKEY_VECTOR="${CMAKE_CURRENT_SOURCE_DIR}/vectors/rekey.json")
target_link_libraries(fips_core_rekey_test PRIVATE fips_core PkgConfig::FIPS_CORE_SODIUM PkgConfig::FIPS_LINK_JANSSON)
set_target_properties(fips_core_rekey_test PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
apply_warnings(fips_core_rekey_test)
apply_sanitizers(fips_core_rekey_test)
add_test(NAME FipsCoreRekeyTest COMMAND fips_core_rekey_test)
set_tests_properties(FipsCoreRekeyTest PROPERTIES TIMEOUT 60 LABELS "fips;unit;rekey")
nostrc_describe_test(FipsCoreRekeyTest OWNER libfips-core TARGETS fips_core_rekey_test
  INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/vectors/rekey.json" "${CMAKE_CURRENT_SOURCE_DIR}/../UPSTREAM.md")
add_dependencies(fips_core_host_test fips_core_rekey_test)

# The differential trace's start_now is a private test hook, never in fips_core.
get_target_property(fips_rekey_unit_sources fips_core_rekey_test SOURCES)
set(fips_rekey_hook_sources "")
foreach(source IN LISTS fips_rekey_unit_sources)
  if(source MATCHES "^../src/proto/rekey/.*[.]c$")
    list(APPEND fips_rekey_hook_sources "${source}")
  endif()
endforeach()
target_sources(fips_diff PRIVATE fakes/rekey_profile.c ${fips_rekey_hook_sources})
target_compile_definitions(fips_diff PRIVATE FIPS_TEST_HOOKS FIPS_CORE_STATIC_DEFINE)

add_executable(fips_core_rekey_profiles_test test_rekey_profiles.c
  fakes/rekey_profile.c fakes/host.c ${fips_rekey_hook_sources})
target_compile_definitions(fips_core_rekey_profiles_test PRIVATE FIPS_TEST_HOOKS FIPS_CORE_STATIC_DEFINE)
target_link_libraries(fips_core_rekey_profiles_test PRIVATE fips_core PkgConfig::FIPS_CORE_SODIUM)
set_target_properties(fips_core_rekey_profiles_test PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
apply_warnings(fips_core_rekey_profiles_test)
apply_sanitizers(fips_core_rekey_profiles_test)
add_test(NAME FipsCoreRekeyProfilesTest COMMAND fips_core_rekey_profiles_test)
set_tests_properties(FipsCoreRekeyProfilesTest PROPERTIES TIMEOUT 60 LABELS "fips;unit;rekey")
nostrc_describe_test(FipsCoreRekeyProfilesTest OWNER libfips-core TARGETS fips_core_rekey_profiles_test)
add_dependencies(fips_core_host_test fips_core_rekey_profiles_test)
# PathSet/Bundle and independent path codec replay (dispatcher is coordinator-owned).
add_executable(fips_core_path_test test_path.c test_path_live.c test_path_machine.c test_path_net.c fakes/host.c)
target_link_libraries(fips_core_path_test PRIVATE fips_core)
set_target_properties(fips_core_path_test PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
apply_warnings(fips_core_path_test)
apply_sanitizers(fips_core_path_test)
add_test(NAME FipsCorePathTest COMMAND fips_core_path_test)
set_tests_properties(FipsCorePathTest PROPERTIES TIMEOUT 30 LABELS "fips;unit;path")
nostrc_describe_test(FipsCorePathTest OWNER libfips-core TARGETS fips_core_path_test)
add_executable(fips_core_path_vectors_test test_path_vectors.c diff/proto_path.c
  diff/loader.c diff/schema.c diff/proto_link.c fakes/host.c)
get_target_property(fips_path_diff_definitions fips_diff COMPILE_DEFINITIONS)
target_compile_definitions(fips_core_path_vectors_test PRIVATE ${fips_path_diff_definitions})
target_link_libraries(fips_core_path_vectors_test PRIVATE fips_core PkgConfig::FIPS_LINK_JANSSON OpenSSL::Crypto)
set_target_properties(fips_core_path_vectors_test PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
apply_warnings(fips_core_path_vectors_test)
apply_sanitizers(fips_core_path_vectors_test)
add_test(NAME FipsCorePathVectorsTest COMMAND fips_core_path_vectors_test)
set_tests_properties(FipsCorePathVectorsTest PROPERTIES TIMEOUT 30 LABELS "fips;diff;path")
nostrc_describe_test(FipsCorePathVectorsTest OWNER libfips-core TARGETS fips_core_path_vectors_test
  INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/vectors/proto.json" "${CMAKE_CURRENT_SOURCE_DIR}/../UPSTREAM.md")
add_dependencies(fips_core_host_test fips_core_path_test fips_core_path_vectors_test)

# Compile the actual settlement sources here too: GO_ENABLE_TSAN deliberately
# leaves fips_core uninstrumented, but these sans-IO primitives need TSan proof.
set(settle_test_sources ../src/proto/settle/params.c ../src/proto/settle/links.c ../src/proto/settle/legs.c ../src/proto/settle/gates.c ../src/proto/settle/table.c ../src/proto/settle/join.c ../src/proto/settle/model/history.c ../src/proto/settle/model/judge.c ../src/proto/settle/parity.c ../src/proto/settle/faults.c)
add_executable(fips_core_settle_test test_settle.c test_settle_gates.c test_settle_table.c test_settle_model.c test_settle_join.c test_settle_parity.c test_settle_faults.c fakes/host.c fakes/rekey.c ${settle_test_sources})
# Join invokes real rekey and bundle code; instrument those dependencies too,
# including private rekey fixture hooks, rather than relying on the TSan-excluded archive.
get_target_property(settle_rekey_sources fips_core_rekey_test SOURCES)
foreach(source IN LISTS settle_rekey_sources)
  if(source MATCHES "^../src/proto/rekey/")
    target_sources(fips_core_settle_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/${source}")
    target_sources(fips_diff_settle PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/${source}")
  endif()
endforeach()
foreach(source IN ITEMS wire set params rules candidate capable select live machine)
  target_sources(fips_core_settle_test PRIVATE "../src/proto/path/${source}.c")
  target_sources(fips_diff_settle PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src/proto/path/${source}.c")
endforeach()
target_link_libraries(fips_core_settle_test PRIVATE fips_core Threads::Threads PkgConfig::FIPS_CORE_SODIUM)
target_compile_definitions(fips_core_settle_test PRIVATE FIPS_TEST_HOOKS
  FIPS_SETTLE_PARITY_TRACE="${CMAKE_CURRENT_SOURCE_DIR}/../src/proto/settle/upstream/parity.txt")
set_target_properties(fips_core_settle_test PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
apply_warnings(fips_core_settle_test)
apply_sanitizers(fips_core_settle_test)
add_test(NAME FipsCoreSettleTest COMMAND fips_core_settle_test)
set_tests_properties(FipsCoreSettleTest PROPERTIES TIMEOUT 30 LABELS "fips;unit")
nostrc_describe_test(FipsCoreSettleTest OWNER libfips-core TARGETS fips_core_settle_test)
add_test(NAME FipsSettleFaults COMMAND "${Python3_EXECUTABLE}" -I
  "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_settle_faults.py"
  --runner "$<TARGET_FILE:fips_diff_settle>" --unit "$<TARGET_FILE:fips_core_settle_test>")
set_tests_properties(FipsSettleFaults PROPERTIES TIMEOUT 120 LABELS "fips;unit;mutation")
nostrc_describe_test(FipsSettleFaults OWNER libfips-core TARGETS fips_core_settle_test fips_diff_settle
  INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_settle_faults.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_settle_faults.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/settle_fault_origins.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/vectors/settle.json")
add_dependencies(fips_core_host_test fips_core_settle_test)
