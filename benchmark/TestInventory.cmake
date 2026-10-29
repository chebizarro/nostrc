if(TEST "bench_relay_perf_baseline")
  nostrc_describe_test("bench_relay_perf_baseline"
    OWNER libnostr libgo
    TARGETS relay_perf_baseline
    INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/relay_perf_fixture.py"
    EXPECTED pass)
endif()
