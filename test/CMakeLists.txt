# Behavioral / differential tests, driven by the shell harnesses. The fixture
# suites' oracle outputs (VeriMon verdicts, MonPoly typing/monitorability, parser
# ASTs) are committed, so those run no monpoly at test time -- fixtures are
# (re)generated offline by each suite's regen.sh. The `random` suite is the one
# exception: a LIVE randomized differential test against a user-provided VeriMon
# binary (no monpoly dependency is committed).
#
#   ctest --preset conan-release             # everything (docker tests self-provision)
#   ctest --preset conan-release -L fast     # native-only: parser_diff + pipeline_diff
#   ctest --preset conan-release -L docker   # behavioral + monpoly_suite (needs docker)
#   STATICMON_VERIMON=/path/to/monpoly ctest --preset conan-release -L random
#
# Each harness exits 77 -- reported as "Skipped", not "Failed" (SKIP_RETURN_CODE
# 77 below) -- when a runtime prerequisite is missing: docker (the bench
# container) for the docker suites, python3, or (random) $STATICMON_VERIMON.
set(TD "${CMAKE_CURRENT_SOURCE_DIR}")

# ---- fast (native) : replay committed fixtures against a freshly built tool --
add_test(NAME parser_diff
         COMMAND bash "${TD}/parser_diff/run_diff.sh" "$<TARGET_FILE:parser_dump>")
add_test(NAME pipeline_diff
         COMMAND bash "${TD}/pipeline_diff/run_check_diff.sh" "$<TARGET_FILE:staticmon-headers>")
set_tests_properties(parser_diff pipeline_diff
                     PROPERTIES LABELS fast SKIP_RETURN_CODE 77)

# ---- bench container fixture : build behavioral image + run smbench ----------
add_test(NAME bench_up   COMMAND bash "${TD}/ctest-bench-up.sh")
add_test(NAME bench_down COMMAND bash "${TD}/ctest-bench-down.sh")
set_tests_properties(bench_up   PROPERTIES FIXTURES_SETUP   bench LABELS docker SKIP_RETURN_CODE 77)
set_tests_properties(bench_down PROPERTIES FIXTURES_CLEANUP bench LABELS docker)

# ---- docker : compiled-monitor verdicts vs VeriMon, per formula --------------
add_test(NAME behavioral
         COMMAND bash "${TD}/behavioral/run_behavioral.sh" "$<TARGET_FILE:staticmon-headers>")
add_test(NAME monpoly_suite
         COMMAND bash "${TD}/monpoly_suite/run_suite.sh" "$<TARGET_FILE:staticmon-headers>")
set_tests_properties(behavioral monpoly_suite
                     PROPERTIES LABELS docker FIXTURES_REQUIRED bench SKIP_RETURN_CODE 77)

# ---- random : live randomized differential test vs a USER-PROVIDED VeriMon ----
# Generates random formulas + traces and compares staticmon to VeriMon live.
# Skips unless $STATICMON_VERIMON points at a monpoly build (run with -verified);
# the repo keeps no monpoly dependency. Native staticmon-impl compile (cached).
add_test(NAME random_diff
         COMMAND bash "${TD}/random_diff/run.sh" "$<TARGET_FILE:staticmon-headers>")
set_tests_properties(random_diff PROPERTIES LABELS random SKIP_RETURN_CODE 77)
