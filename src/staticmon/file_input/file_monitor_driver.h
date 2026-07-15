#pragma once
#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <staticmon/file_input/parser/trace_parser.h>
#include <staticmon/monitor/monitor.h>
#include <staticmon/monitor/monitor_driver.h>
#include <staticmon/operators/operators.h>
#include <string>

class file_monitor_driver : public monitor_driver {
public:
  file_monitor_driver(const std::filesystem::path &log_path,
                      std::optional<std::string> verdict_path,
                      bool verbose = false)
      : log_(std::ifstream(log_path)),
        monitor_(verdict_printer(verdict_path, verbose), verbose) {
    // A missing/unreadable log otherwise reads as an empty trace (no verdicts,
    // no error) -- surface it instead.
    if (!log_.is_open())
      throw std::runtime_error("cannot open log file '" + log_path.string() +
                               "' (no such file or not readable)");
  }
  ~file_monitor_driver() noexcept override = default;

  // Split the trace into time-points and monitor them incrementally. A
  // time-point is `@ <ts> <db>`; it ends at a top-level ';' (emitted at once)
  // or at the next top-level '@' (MonPoly also accepts an omitted ';'), or at
  // EOF. '@' and ';' inside a quoted string argument are not delimiters. This
  // accepts one- or multi-line time-points, with or without ';'.
  void do_monitor() override {
    std::string tp;
    bool in_str = false, esc = false, started = false;
    char c;
    auto flush = [&] {
      if (tp.find_first_not_of(" \t\r\n") != std::string::npos) {
        auto [ts, db] = parser_.parse_database(tp);
        monitor_.step(db, make_vector(ts));
      }
      tp.clear();
      started = false;
    };
    while (log_.get(c)) {
      if (in_str) {
        tp.push_back(c);
        if (esc)
          esc = false;
        else if (c == '\\')
          esc = true;
        else if (c == '"')
          in_str = false;
        continue;
      }
      if (c == '"') {
        in_str = true;
        started = true;
      } else if (c == '@' && started) {
        flush();  // previous time-point had no ';'
      } else if (c == ';') {
        tp.push_back(c);
        flush();  // ';' terminates the time-point
        continue;
      } else if (!std::isspace(static_cast<unsigned char>(c))) {
        started = true;
      }
      tp.push_back(c);
    }
    flush();  // trailing time-point without a ';'
    monitor_.last_step();
  }

private:
  parse::trace_parser parser_;
  std::ifstream log_;
  monitor monitor_;
};
