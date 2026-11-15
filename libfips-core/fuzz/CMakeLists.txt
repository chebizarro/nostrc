if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "FIPS_ENABLE_FUZZERS requires clang/libFuzzer")
endif()
if(GNOSTR_ENABLE_TSAN OR GO_ENABLE_TSAN)
  message(FATAL_ERROR "FIPS fuzzers cannot be combined with TSan")
endif()
# Instrument the implementation too, not only the input adapter.
target_compile_options(fips_core PRIVATE -fsanitize=fuzzer-no-link)
add_executable(fips_fuzz_host fuzz_host.c ../tests/fakes/host.c)
target_link_libraries(fips_fuzz_host PRIVATE fips_core)
set_target_properties(fips_fuzz_host PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
apply_warnings(fips_fuzz_host)
apply_sanitizers(fips_fuzz_host)
target_compile_options(fips_fuzz_host PRIVATE -fsanitize=fuzzer)
target_link_options(fips_fuzz_host PRIVATE -fsanitize=fuzzer)

add_executable(fips_fuzz_codec fuzz_codec.c)
target_link_libraries(fips_fuzz_codec PRIVATE fips_core)
set_target_properties(fips_fuzz_codec PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
apply_warnings(fips_fuzz_codec)
apply_sanitizers(fips_fuzz_codec)
target_compile_options(fips_fuzz_codec PRIVATE -fsanitize=fuzzer)
target_link_options(fips_fuzz_codec PRIVATE -fsanitize=fuzzer)

# NIP-19 identity decoding, including mixed case and canonical-form invariants.
add_executable(fips_fuzz_identity fuzz_identity.c)
target_link_libraries(fips_fuzz_identity PRIVATE fips_core PkgConfig::FIPS_CORE_SODIUM)
set_target_properties(fips_fuzz_identity PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
apply_warnings(fips_fuzz_identity)
apply_sanitizers(fips_fuzz_identity)
target_compile_options(fips_fuzz_identity PRIVATE -fsanitize=fuzzer)
target_link_options(fips_fuzz_identity PRIVATE -fsanitize=fuzzer)

# Noise AEAD arbitrary open and seal/open properties.
add_executable(fips_fuzz_aead fuzz_aead.c)
target_link_libraries(fips_fuzz_aead PRIVATE fips_core)
set_target_properties(fips_fuzz_aead PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
apply_warnings(fips_fuzz_aead)
apply_sanitizers(fips_fuzz_aead)
target_compile_options(fips_fuzz_aead PRIVATE -fsanitize=fuzzer)
target_link_options(fips_fuzz_aead PRIVATE -fsanitize=fuzzer)
add_executable(fips_fuzz_coord fuzz_coord.c)
target_link_libraries(fips_fuzz_coord PRIVATE fips_core)
set_target_properties(fips_fuzz_coord PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
apply_warnings(fips_fuzz_coord)
apply_sanitizers(fips_fuzz_coord)
target_compile_options(fips_fuzz_coord PRIVATE -fsanitize=fuzzer)
target_link_options(fips_fuzz_coord PRIVATE -fsanitize=fuzzer)
# SessionDatagram body decoding, TTL hop rule and encode round trips (nostrc-82g9n.6).
add_executable(fips_fuzz_link fuzz_link.c)
target_link_libraries(fips_fuzz_link PRIVATE fips_core)
set_target_properties(fips_fuzz_link PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
apply_warnings(fips_fuzz_link)
apply_sanitizers(fips_fuzz_link)
target_compile_options(fips_fuzz_link PRIVATE -fsanitize=fuzzer)
target_link_options(fips_fuzz_link PRIVATE -fsanitize=fuzzer)

# FMP wire codecs and the common pending/established ingress gate.
add_executable(fips_fuzz_fmp fuzz_fmp.c)
target_link_libraries(fips_fuzz_fmp PRIVATE fips_core)
set_target_properties(fips_fuzz_fmp PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
apply_warnings(fips_fuzz_fmp)
apply_sanitizers(fips_fuzz_fmp)
target_compile_options(fips_fuzz_fmp PRIVATE -fsanitize=fuzzer)
target_link_options(fips_fuzz_fmp PRIVATE -fsanitize=fuzzer)
# STP signing preimage and TreeAnnounce Reader/Writer properties.
add_executable(fips_fuzz_stp fuzz_stp.c)
target_link_libraries(fips_fuzz_stp PRIVATE fips_core)
set_target_properties(fips_fuzz_stp PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
apply_warnings(fips_fuzz_stp)
apply_sanitizers(fips_fuzz_stp)
target_compile_options(fips_fuzz_stp PRIVATE -fsanitize=fuzzer)
target_link_options(fips_fuzz_stp PRIVATE -fsanitize=fuzzer)
# Bloom raw/announce codecs and merge invariants (nostrc-82g9n.3).
add_executable(fips_fuzz_bloom fuzz_bloom.c)
target_link_libraries(fips_fuzz_bloom PRIVATE fips_core)
set_target_properties(fips_fuzz_bloom PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
apply_warnings(fips_fuzz_bloom)
apply_sanitizers(fips_fuzz_bloom)
target_compile_options(fips_fuzz_bloom PRIVATE -fsanitize=fuzzer)
target_link_options(fips_fuzz_bloom PRIVATE -fsanitize=fuzzer)

# FSP lenient/strict session ingress, port headers and bounded quorum.
add_executable(fips_fuzz_fsp fuzz_fsp.c)
target_link_libraries(fips_fuzz_fsp PRIVATE fips_core)
set_target_properties(fips_fuzz_fsp PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
apply_warnings(fips_fuzz_fsp)
apply_sanitizers(fips_fuzz_fsp)
target_compile_options(fips_fuzz_fsp PRIVATE -fsanitize=fuzzer)
target_link_options(fips_fuzz_fsp PRIVATE -fsanitize=fuzzer)
# Routing signal bodies (FSP prefix/type already dispatched).
add_executable(fips_fuzz_routing fuzz_routing.c)
target_link_libraries(fips_fuzz_routing PRIVATE fips_core)
set_target_properties(fips_fuzz_routing PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
apply_warnings(fips_fuzz_routing)
apply_sanitizers(fips_fuzz_routing)
target_compile_options(fips_fuzz_routing PRIVATE -fsanitize=fuzzer)
target_link_options(fips_fuzz_routing PRIVATE -fsanitize=fuzzer)
# Both mesh lookup codecs and proof preimage bounds.
add_executable(fips_fuzz_lookup fuzz_lookup.c)
target_link_libraries(fips_fuzz_lookup PRIVATE fips_core)
set_target_properties(fips_fuzz_lookup PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
apply_warnings(fips_fuzz_lookup)
apply_sanitizers(fips_fuzz_lookup)
target_compile_options(fips_fuzz_lookup PRIVATE -fsanitize=fuzzer)
target_link_options(fips_fuzz_lookup PRIVATE -fsanitize=fuzzer)
# IK/XK arbitrary handshake ingress and immutable read-on-Fork.
add_executable(fips_fuzz_handshake fuzz_handshake.c ../tests/fakes/host.c)
target_link_libraries(fips_fuzz_handshake PRIVATE fips_core PkgConfig::FIPS_CORE_SODIUM)
set_target_properties(fips_fuzz_handshake PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
apply_warnings(fips_fuzz_handshake)
apply_sanitizers(fips_fuzz_handshake)
target_compile_options(fips_fuzz_handshake PRIVATE -fsanitize=fuzzer)
target_link_options(fips_fuzz_handshake PRIVATE -fsanitize=fuzzer)

# Transport open/trial-open and replay-window sequences (nostrc-pwo3h.7).
add_executable(fips_fuzz_session fuzz_session.c)
target_link_libraries(fips_fuzz_session PRIVATE fips_core)
set_target_properties(fips_fuzz_session PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
apply_warnings(fips_fuzz_session)
apply_sanitizers(fips_fuzz_session)
target_compile_options(fips_fuzz_session PRIVATE -fsanitize=fuzzer)
target_link_options(fips_fuzz_session PRIVATE -fsanitize=fuzzer)

# No diagnostic wire format: fuzz host observations and action-list invariants.
add_executable(fips_fuzz_probe fuzz_probe.c)
target_link_libraries(fips_fuzz_probe PRIVATE fips_core)
set_target_properties(fips_fuzz_probe PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
apply_warnings(fips_fuzz_probe)
apply_sanitizers(fips_fuzz_probe)
target_compile_options(fips_fuzz_probe PRIVATE -fsanitize=fuzzer)
target_link_options(fips_fuzz_probe PRIVATE -fsanitize=fuzzer)
