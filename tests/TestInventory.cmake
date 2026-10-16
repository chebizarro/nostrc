# Audit ownership for retained legacy root-ASan exclusions. No execution or
# expected-pass exception is implied by describing their actual linked inputs.
if(COMMAND nostrc_describe_test)
  if(TEST test_json_envelope)
    nostrc_describe_test(test_json_envelope OWNER shared.tests libjson
      TARGETS test_json_envelope)
  endif()
  if(TEST concurrency_subscription_shutdown_asan)
    nostrc_describe_test(concurrency_subscription_shutdown_asan OWNER shared.tests)
  endif()
endif()
