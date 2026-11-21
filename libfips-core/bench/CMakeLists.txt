# Opt-in host benchmarks, never installed or registered with ctest.
find_package(Threads REQUIRED)
foreach(bench IN ITEMS noise_aead routing_next_hop coord_cache transport_addr stats_history)
  add_executable(fips_bench_${bench} ${bench}.c bench.c ../tests/fakes/host.c)
  target_link_libraries(fips_bench_${bench} PRIVATE fips_core Threads::Threads)
  target_include_directories(fips_bench_${bench} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../tests")
  target_compile_definitions(fips_bench_${bench} PRIVATE _POSIX_C_SOURCE=200809L)
  set_target_properties(fips_bench_${bench} PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
  apply_warnings(fips_bench_${bench})
  apply_sanitizers(fips_bench_${bench})
endforeach()
add_custom_target(fips_benches DEPENDS fips_bench_noise_aead
  fips_bench_routing_next_hop fips_bench_coord_cache fips_bench_transport_addr
  fips_bench_stats_history)
