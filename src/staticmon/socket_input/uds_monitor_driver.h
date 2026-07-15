#pragma once
#include <optional>
#include <staticmon/monitor/monitor.h>
#include <staticmon/monitor/monitor_driver.h>
#include <staticmon/operators/operators.h>
#include <staticmon/socket_input/socket_monitor_half.h>

class uds_monitor_driver : public monitor_driver {
public:
  uds_monitor_driver(const std::string &socket_path,
                     std::optional<std::string> verdict_path,
                     bool verbose = false)
      : monitor_(verdict_printer(std::move(verdict_path)), verbose),
        net_(socket_path) {}
  ~uds_monitor_driver() noexcept override = default;
  void do_monitor() override {

    for (;;) {
      auto opt_db = net_.read_database();
      if (opt_db) {
        auto &[ts, db] = *opt_db;
        monitor_.step(db, make_vector(ts));
      } else {
        monitor_.last_step();
        net_.send_eof();
        return;
      }
    }
  }

private:
  monitor monitor_;
  socket_monitor_half net_;
};
