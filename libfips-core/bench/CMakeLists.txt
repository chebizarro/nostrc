# Micro-benchmarks: built behind FIPS_ENABLE_BENCH, never registered with ctest.
# Baselines for nostrc-f66sr.4; port of upstream benches/*.rs.
add_executable(fips_bench_coord_cache coord_cache.c ../tests/fakes/host.c)
target_link_libraries(fips_bench_coord_cache PRIVATE fips_core)
set_target_properties(fips_bench_coord_cache PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
apply_warnings(fips_bench_coord_cache)
apply_sanitizers(fips_bench_coord_cache)
