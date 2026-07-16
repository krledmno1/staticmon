# Correctness tests, organized by tested module x testing method:
#   parser/    -- the C++ formula parser                      (fixture)
#   frontend/  -- staticmon-headers typing + monitorability   (fixture)
#   monitor/   -- the compiled monitor's verdicts vs VeriMon  (fixture + live)
# Each module has components/ (generator, oracle, comparator, ...) and methods/
# (fixture, live). Fixture methods replay committed golden data -- no monpoly at
# test time; regenerated offline by each method's regen*.sh. The monitor's live
# method runs a user-provided VeriMon. Monitor compilation is NATIVE-first
# (staticmon-impl against the builddir), docker fallback (smbench container);
# force with STATICMON_TEST_MODE=native|docker.
#
#   ctest -L fast                                        # parser + frontend (instant)
#   ctest -L monitor                                     # monitor fixture replay (compiles per formula)
#   STATICMON_VERIMON=/path/to/monpoly ctest -L live     # live random diff
#
# Tests exit 77 ("Skipped", not "Failed"; SKIP_RETURN_CODE 77) when a prerequisite
# is missing: python3; a configured native builddir or the smbench container; or
# $STATICMON_VERIMON.
set(TD "${CMAKE_CURRENT_SOURCE_DIR}")

# ---- fast : replay committed fixtures against a freshly built tool -----------
add_test(NAME parser
         COMMAND bash "${TD}/parser/methods/fixture/run.sh" "$<TARGET_FILE:parser_dump>")
add_test(NAME frontend
         COMMAND bash "${TD}/frontend/methods/fixture/run.sh" "$<TARGET_FILE:staticmon-headers>")
set_tests_properties(parser frontend PROPERTIES LABELS fast SKIP_RETURN_CODE 77)

# ---- monitor fixture : compiled-monitor verdicts vs frozen VeriMon ----------
# bench_up is native-aware: a no-op when a native build is configured, else it
# builds+starts the smbench container (the docker fallback for compilation).
add_test(NAME bench_up   COMMAND bash "${TD}/monitor/components/ctest-bench-up.sh")
add_test(NAME bench_down COMMAND bash "${TD}/monitor/components/ctest-bench-down.sh")
set_tests_properties(bench_up   PROPERTIES FIXTURES_SETUP   bench LABELS monitor SKIP_RETURN_CODE 77)
set_tests_properties(bench_down PROPERTIES FIXTURES_CLEANUP bench LABELS monitor)

add_test(NAME monitor_generated
         COMMAND bash "${TD}/monitor/methods/fixture/run.sh" cases-generated "$<TARGET_FILE:staticmon-headers>")
add_test(NAME monitor_corpus
         COMMAND bash "${TD}/monitor/methods/fixture/run.sh" cases-corpus "$<TARGET_FILE:staticmon-headers>")
# Hand-written cases pinning translation/codegen bugs the auto-generated suites
# cannot cover (regen drops formulas staticmon fails to compile).
add_test(NAME monitor_regression
         COMMAND bash "${TD}/monitor/methods/fixture/run.sh" cases-regression "$<TARGET_FILE:staticmon-headers>")
set_tests_properties(monitor_generated monitor_corpus monitor_regression
                     PROPERTIES LABELS monitor FIXTURES_REQUIRED bench SKIP_RETURN_CODE 77)

# ---- monitor live : random formulas vs a USER-PROVIDED VeriMon --------------
add_test(NAME monitor_live
         COMMAND bash "${TD}/monitor/methods/live/run.sh" "$<TARGET_FILE:staticmon-headers>")
set_tests_properties(monitor_live PROPERTIES LABELS live SKIP_RETURN_CODE 77)
