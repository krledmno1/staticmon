#include <absl/flags/flag.h>
#include <absl/flags/marshalling.h>
#include <absl/flags/parse.h>
#include <absl/flags/usage.h>
#include <absl/strings/string_view.h>
#include <config.h>
#include <iostream>
#include <memory>
#include <staticmon/monitor/monitor_driver.h>
#include <stdexcept>
#include <string>

ABSL_FLAG(bool, use_socket, false,
          "use the unix domain socket monitoring interface");

ABSL_FLAG(bool, verbose, false,
          "announce each time point as it is read (matches monpoly -verbose)");

#ifdef ENABLE_SOCK_INTF
#include <staticmon/socket_input/uds_monitor_driver.h>
ABSL_FLAG(std::string, socket_path, "cppmon_uds",
          "path of the unix socket to create");
#endif

#ifdef ENABLE_FILE_INPUT
#include <staticmon/file_input/file_monitor_driver.h>
ABSL_FLAG(std::string, log, "log", "path to log in monpoly format");
#endif

ABSL_FLAG(std::string, vpath, "", "output file of the monitor's verdicts");

extern "C" __attribute__((visibility("default"))) const char *
__asan_default_options() {
  return "detect_leaks=false";
}

int main(int argc, char *argv[]) {
  absl::SetProgramUsageMessage("Explicit MFOTL monitor written in C++");
  absl::ParseCommandLine(argc, argv);
  std::unique_ptr<monitor_driver> driver;

  std::optional<std::string> vpath =
    absl::GetFlag(FLAGS_vpath) == ""
      ? std::nullopt
      : std::optional(absl::GetFlag(FLAGS_vpath));

  bool verbose = absl::GetFlag(FLAGS_verbose);

  // Driver construction can fail (e.g. a missing log file or an unbindable
  // socket); a malformed trace surfaces as a parse error. Handle both inside the
  // try so we exit cleanly with a message instead of aborting on an uncaught
  // exception.
  try {
#ifdef ENABLE_SOCK_INTF
    if (absl::GetFlag(FLAGS_use_socket)) {
      driver.reset(new uds_monitor_driver(absl::GetFlag(FLAGS_socket_path),
                                          std::move(vpath), verbose));
    }
#endif
#ifdef ENABLE_FILE_INPUT
    if (!absl::GetFlag(FLAGS_use_socket)) {
      driver.reset(new file_monitor_driver(absl::GetFlag(FLAGS_log),
                                           std::move(vpath), verbose));
    }
#endif
    if (!driver) {
      std::cerr << "staticmon: selected input mode not compiled in\n";
      return 2;
    }
    driver->do_monitor();
  } catch (const std::exception &e) {
    std::cerr << "staticmon: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
