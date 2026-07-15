#pragma once
#include <cstdint>
#include <limits>
#include <staticmon/common/table.h>
#include <staticmon/input_formula/formula.h>
#include <staticmon/operators/operators.h>
#include <type_traits>
#include <vector>

inline constexpr size_t MAXIMUM_TIMESTAMP = std::numeric_limits<size_t>::max();

struct monitor {
  using T = typename input_formula::ResT;
  using L = typename input_formula::ResL;
  using rec_tab_t = table_util::tab_t_of_row_t<T>;
  using ResT = typename table_util::reorder_info<L, free_variables, T>::ResT;
  using reorder_mask = table_util::get_reorder_mask<L, free_variables>;

  monitor(verdict_printer printer, bool verbose = false)
      : printer_(std::move(printer)), verbose_(verbose) {}

  std::vector<ResT> make_verdicts(rec_tab_t &tab) {
    std::vector<ResT> verdicts;
    verdicts.reserve(tab.size());
    for (auto &row : tab)
      verdicts.emplace_back(project_row<reorder_mask>(std::move(row)));
    return verdicts;
  }

  void step(database &db, const ts_list &ts) {
    for (std::size_t t : ts) {
      // `monpoly -verbose` announces each time point as it is read. The
      // end-of-log flush (t == MAXIMUM_TIMESTAMP) is not a real time point, so
      // last_step() decides whether to announce it (see below).
      if (verbose_ && t < MAXIMUM_TIMESTAMP)
        printer_.print_time_point(max_tp_);
      tp_ts_map_.emplace(max_tp_, t);
      max_tp_++;
    }
    auto sats = f_.eval(db, ts);
    std::size_t new_curr_tp = curr_tp_;
    std::size_t n = sats.size();
    for (std::size_t i = 0; i < n; ++i, ++new_curr_tp) {
      std::vector<ResT> output_tab;
      if (sats[i]) {
        assert(!sats[i]->empty());
        output_tab = make_verdicts(*sats[i]);
      }
      auto it = tp_ts_map_.find(new_curr_tp);
      assert(it != tp_ts_map_.end());
      if (it->second < MAXIMUM_TIMESTAMP)
        printer_.print_verdict(it->second, new_curr_tp, output_tab);
      tp_ts_map_.erase(it);
    }
    curr_tp_ = new_curr_tp;
  }

  void last_step() {
    // For bounded-future formulas monpoly injects a final max-timestamp time
    // point to close open windows, and `-verbose` announces it; for
    // past/present formulas nothing is pending and it announces nothing.
    // curr_tp_ < max_tp_ means some ingested time points are still unflushed,
    // which is exactly monpoly's inject-or-not condition.
    if (verbose_ && curr_tp_ < max_tp_)
      printer_.print_time_point(max_tp_);
    database db;
    step(db, make_vector(static_cast<std::size_t>(MAXIMUM_TIMESTAMP)));
  }

  input_formula f_;
  std::vector<std::size_t> output_var_permutation_;
  absl::flat_hash_map<size_t, size_t> tp_ts_map_;
  std::size_t curr_tp_ = 0;
  std::size_t max_tp_ = 0;
  verdict_printer printer_;
  bool verbose_ = false;
};
