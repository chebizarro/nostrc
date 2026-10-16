# The perf registration invokes xvfb-run, not the executable as argv[0].
# Preserve the wrapper and identify its actual linked target explicitly.
if(COMMAND nostrc_describe_test AND TEST test_nostr_gtk_bind_latency_perf)
  nostrc_describe_test(test_nostr_gtk_bind_latency_perf OWNER nostr.gtk
    TARGETS test_nostr_gtk_bind_latency_budget REQUIRES display)
endif()
