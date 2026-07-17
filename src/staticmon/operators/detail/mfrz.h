#pragma once
#include <cassert>
#include <cstdint>
#include <deque>
#include <optional>
#include <staticmon/common/mp_helpers.h>
#include <staticmon/common/table.h>
#include <staticmon/operators/detail/mlet.h>
#include <staticmon/operators/detail/operator_types.h>
#include <vector>

// FRZ p(xs) = f1 IN f2 -- the freeze operator (temporal mode).
//
// Semantics: within f2, the predicate p at ANY queried time-point denotes f1's
// satisfactions at the OUTER time-point i (a constant stream), unlike LET,
// where p at j denotes f1's satisfactions at j. The front-end routes here only
// when f2 uses p under a temporal operator; the other cases collapse to mlet
// (p used only at the current time-point) or to f2 alone (p unused).
//
// Runtime (MonPoly's per-instance mode, algorithm.ml EFrz/FrzPerInst): for
// each outer time-point k, once f1's verdict at k is available, a FRESH f2
// sub-monitor is instantiated and driven over the recorded input history --
// with p bound to the constant frozen table at every index -- and then fed
// each subsequent batch incrementally. Its output at absolute time-point k
// (the diagonal) is the FRZ verdict at k; all other outputs are discarded.
// Because instances are ordinary sub-monitors driven through eval(db, ts),
// nested FRZ inside f2 needs no special handling: an inner mfrz records its
// own history from exactly what the enclosing instance replays into it.
//
// Bounded-past optimization (MonPoly's fpi_depth): when the front-end proves
// f2 purely bounded-past with temporal depth d (no PREV/NEXT, no future
// operators, no nested binders), an instance's replay is trimmed to input
// batches with timestamps in [ts_k - d, ts_k] -- older data cannot affect the
// diagonal at k -- and the history is trimmed the same way as f1's frontier
// advances. Depth == frz_no_depth disables trimming (whole-prefix replay).

inline constexpr std::size_t frz_no_depth = static_cast<std::size_t>(-1);

template<std::size_t pred_id, typename PredL, std::size_t Depth,
         typename MFormula1, typename MFormula2>
struct mfrz {
  using ResL = typename MFormula2::ResL;
  using ResT = typename MFormula2::ResT;
  using res_tab_t = table_util::tab_t_of_row_t<ResT>;
  using reorder_mask =
    table_util::get_reorder_mask<typename MFormula1::ResL, PredL>;

  static constexpr bool bounded = Depth != frz_no_depth;

  struct hist_entry {
    std::size_t first_tp;  // absolute time-point of ts.front()
    ts_list ts;
    database db;  // the batch's database (never contains pred_id: fresh id)
  };

  struct instance {
    std::size_t target_tp;    // outer time-point k this instance freezes
    std::size_t next_out_tp;  // absolute tp of the instance's next output
    database_table frozen;    // f1's verdict at k, in PredL column order
    bool done = false;
    std::optional<res_tab_t> result;  // valid once done
    // Wrapped so the (potentially large) sub-monitor state is destroyed as
    // soon as the diagonal is collected; the slot itself stays queued until
    // all earlier instances have resolved (ordered emission).
    std::optional<MFormula2> f2;
  };

  // Feed one (db, ts) batch to an instance: bind p to the frozen table at
  // every new index, evaluate, and scan the outputs for the diagonal. The db
  // is mutated in place (insert p, erase after), mirroring mlet; pred ids are
  // unique per formula, so no entry can be shadowed.
  void feed(instance &inst, database &db, const ts_list &ts) {
    bool did_emplace =
      db.emplace(pred_id, database::mapped_type(ts.size(), inst.frozen))
        .second;
    assert(did_emplace);
    (void) did_emplace;
    auto outs = inst.f2->eval(db, ts);
    db.erase(pred_id);
    for (auto &tab : outs) {
      if (inst.next_out_tp == inst.target_tp) {
        inst.result = std::move(tab);
        inst.done = true;
        inst.f2.reset();
      }
      ++inst.next_out_tp;
      if (inst.done)
        break;  // later outputs of this instance are never needed
    }
  }

  std::vector<std::optional<res_tab_t>> eval(database &db, const ts_list &ts) {
    // 1. Advance f1; its new verdicts are the frozen snapshots for the outer
    //    time-points [alpha_tp_, alpha_tp_ + |l_tabs|).
    auto l_tabs = f1_.eval(db, ts);

    // 2. Record the batch (db copied WITHOUT p, which is not present: the
    //    bound predicate's id is fresh and enclosing binders erase theirs).
    if (!ts.empty()) {
      hist_.push_back(hist_entry{seen_tp_, ts, db});
      seen_tp_ += ts.size();
    }

    // 3. Feed the new batch to instances created in earlier steps (instances
    //    created below replay the history, which already includes this batch).
    if (!ts.empty())
      for (auto &inst : active_)
        if (!inst.done)
          feed(inst, db, ts);

    // 4. One fresh instance per new snapshot, caught up over the history.
    for (auto &tab : l_tabs) {
      instance inst;
      inst.target_tp = alpha_tp_++;
      if (tab)
        inst.frozen = tab_to_db_tab<reorder_mask>(*tab);
      inst.f2.emplace();
      // Replay window: bounded bodies only need batches whose timestamps can
      // fall within [ts_k - Depth, ts_k]. ts_k is in hist_ (snapshot k implies
      // k < seen_tp_, and trimming below never outruns f1's frontier).
      std::size_t begin = 0;
      if constexpr (bounded) {
        std::size_t ts_k = ts_of_tp(inst.target_tp);
        std::size_t lo = ts_k >= Depth ? ts_k - Depth : 0;
        while (begin < hist_.size() && hist_[begin].ts.back() < lo)
          ++begin;
      }
      inst.next_out_tp = begin < hist_.size()
                           ? hist_[begin].first_tp
                           : seen_tp_;  // unreachable: hist_ covers target_tp
      for (std::size_t i = begin; i < hist_.size() && !inst.done; ++i)
        feed(inst, hist_[i].db, hist_[i].ts);
      // Bounded bodies resolve during replay (no future operators, and the
      // history reaches the instance's own time-point).
      assert(!bounded || inst.done);
      active_.push_back(std::move(inst));
    }

    // 5. Trim the history against f1's frontier: every future snapshot k' has
    //    k' >= alpha_tp_, hence ts_k' >= ts(alpha_tp_), so batches entirely
    //    older than ts(alpha_tp_) - Depth can never be replayed again.
    if constexpr (bounded) {
      if (!hist_.empty()) {
        std::size_t frontier_ts = alpha_tp_ < seen_tp_
                                    ? ts_of_tp(alpha_tp_)
                                    : hist_.back().ts.back();
        std::size_t lo = frontier_ts >= Depth ? frontier_ts - Depth : 0;
        while (!hist_.empty() && hist_.front().ts.back() < lo)
          hist_.pop_front();
      }
    }

    // 6. Emit resolved verdicts in outer-time-point order: instances are
    //    queued in creation order (= consecutive outer tps), and a later
    //    instance never outputs before an earlier one is popped.
    std::vector<std::optional<res_tab_t>> res;
    while (!active_.empty() && active_.front().done) {
      res.emplace_back(std::move(active_.front().result));
      active_.pop_front();
    }
    return res;
  }

private:
  // Timestamp of an absolute time-point that is still in the history.
  std::size_t ts_of_tp(std::size_t tp) const {
    for (const auto &e : hist_)
      if (tp < e.first_tp + e.ts.size())
        return e.ts[tp - e.first_tp];
    assert(false && "time-point not in history");
    return 0;
  }

  MFormula1 f1_;
  std::deque<hist_entry> hist_;
  std::deque<instance> active_;
  std::size_t seen_tp_ = 0;   // trace time-points recorded so far
  std::size_t alpha_tp_ = 0;  // f1 verdicts consumed so far (next snapshot tp)
};
