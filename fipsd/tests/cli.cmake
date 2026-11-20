# Runs the real fipsd binary through the documented flags (cli-fips.md).
function(run_fipsd out_rc out_stdout out_stderr)
  execute_process(COMMAND "${FIPSD}" ${ARGN}
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  set(${out_rc} "${rc}" PARENT_SCOPE)
  set(${out_stdout} "${out}" PARENT_SCOPE)
  set(${out_stderr} "${err}" PARENT_SCOPE)
endfunction()

# expect(<message> <if-condition tokens...>): the tokens reach if() unquoted.
function(expect message)
  if(NOT (${ARGN}))
    message(FATAL_ERROR "${message}")
  endif()
endfunction()

# An empty element would vanish from ${ARGN}, so empty checks get their own helper.
function(expect_empty value message)
  if(NOT "${value}" STREQUAL "")
    message(FATAL_ERROR "${message}")
  endif()
endfunction()

set(short_re "^fipsd ${EXPECTED_VERSION}( \\(rev [0-9a-f]+(-dirty)?\\))?\n")

# -V: short version, exit 0, nothing on stderr.
run_fipsd(rc out err -V)
expect("-V exited ${rc}: ${err}" rc EQUAL 0)
expect("-V printed '${out}'" out MATCHES "${short_re}$")
expect_empty("${err}" "-V wrote to stderr: ${err}")

# --version: short version, target triple, core version.
run_fipsd(rc out err --version)
expect("--version exited ${rc}: ${err}" rc EQUAL 0)
expect("--version printed '${out}'"
  out MATCHES "${short_re}target: [^\n]+\nlibfips-core: ${EXPECTED_CORE_VERSION}\n$")

# -h / --help: usage on stdout, exit 0.
foreach(flag -h --help)
  run_fipsd(rc out err ${flag})
  expect("${flag} exited ${rc}: ${err}" rc EQUAL 0)
  expect("${flag} printed '${out}'" out MATCHES "Usage: fipsd \\[OPTIONS\\]")
  foreach(needle "-c, --config <FILE>" "--log-file <FILE>" "-V" "--version" "-h, --help")
    string(FIND "${out}" "${needle}" pos)
    expect("${flag} output lacks '${needle}'" pos GREATER -1)
  endforeach()
  expect_empty("${err}" "${flag} wrote to stderr: ${err}")
endforeach()

# Help wins over version, as with clap.
run_fipsd(rc out err -V --help)
expect("-V --help: rc ${rc}, '${out}'" rc EQUAL 0 AND out MATCHES "Usage: fipsd")

# Usage errors: exit 2, diagnostic plus usage on stderr, nothing on stdout.
run_fipsd(rc out err --bogus)
expect("--bogus exited ${rc}" rc EQUAL 2)
expect("--bogus stderr: '${err}'" err MATCHES "error: unexpected argument '--bogus' found")
expect("--bogus stderr: '${err}'" err MATCHES "Usage: fipsd \\[OPTIONS\\]" AND err MATCHES "--help")
expect_empty("${out}" "--bogus wrote to stdout: ${out}")

run_fipsd(rc out err -c)
expect("-c without value exited ${rc}" rc EQUAL 2)
expect("-c stderr: '${err}'" err MATCHES "a value is required for '--config <FILE>'")

run_fipsd(rc out err --log-file)
expect("--log-file without value exited ${rc}" rc EQUAL 2)
expect("--log-file stderr: '${err}'" err MATCHES "a value is required for '--log-file <FILE>'")

run_fipsd(rc out err stray)
expect("positional exited ${rc}" rc EQUAL 2)
expect("positional stderr: '${err}'" err MATCHES "error: unexpected argument 'stray' found")

run_fipsd(rc out err -x)
expect("-x exited ${rc}" rc EQUAL 2)
expect("-x stderr: '${err}'" err MATCHES "error: unexpected argument '-x' found")

if(CMAKE_HOST_WIN32)
  # The service flags exist here (never run --install-service: from an
  # elevated shell it registers a real service). Outside the service control
  # manager, --service fails at once with a hint instead of hanging.
  run_fipsd(rc out err --help)
  expect("--help lists the service commands: '${out}'"
    out MATCHES "--install-service" AND out MATCHES "--uninstall-service")
  run_fipsd(rc out err --service)
  expect("--service from a console exited ${rc}" rc EQUAL 1)
  expect("--service stderr: '${err}'" err MATCHES "--service is for the service control manager")
else()
  # Windows service flags exist only there and are rejected like any unknown flag.
  foreach(flag --install-service --uninstall-service --service)
    run_fipsd(rc out err ${flag})
    expect("${flag} exited ${rc}" rc EQUAL 2)
  endforeach()
endif()
