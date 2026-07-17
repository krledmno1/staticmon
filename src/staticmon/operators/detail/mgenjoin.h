#pragma once
#include <boost/container/devector.hpp>
#include <boost/mp11.hpp>
#include <cassert>
#include <cstdint>
#include <optional>
#include <staticmon/common/mp_helpers.h>
#include <staticmon/common/table.h>
#include <staticmon/operators/detail/operator_types.h>
#include <tuple>
#include <vector>

using namespace boost::mp11;

// mgenjoin<mp_list<Pos...>, mp_list<Neg...>> -- a flattened conjunction
// cluster (docs/LFTJ-STATICMON.md): >= 1 positive children joined, then every
// negative child anti-joined. The I/O contract deliberately matches VeriMon's
// verified mmulti_join' A_pos A_neg L (Optimized_Join.thy,
// mmulti_join'_correct): a tuple is in the result iff it is over the union of
// the positive layouts, its restriction to each positive layout is in that
// child's table, and its restriction to each negative layout is NOT in that
// child's table. The translator guarantees the MAnds side conditions
// (>= 1 positive; every negative's columns covered by the positives' union).
//
// WP-J1 implementation: a left-to-right fold of the existing binary
// table_join / table_anti_join -- semantically the final algorithm, evaluated
// the old way, so the cluster plumbing (flattening, n-ary batch alignment,
// layouts) can be validated in isolation. WP-J2 replaces the fold with the
// specialized LFTJ enumeration.

// Layout/type accumulator for the left-to-right positive join fold.
template<typename L, typename T>
struct genjoin_acc {
  using ResL = L;
  using ResT = T;
};

template<typename Acc, typename F>
using genjoin_step =
  genjoin_acc<typename table_util::join_result_info<
                typename Acc::ResL, typename F::ResL, typename Acc::ResT,
                typename F::ResT>::ResL,
              typename table_util::join_result_info<
                typename Acc::ResL, typename F::ResL, typename Acc::ResT,
                typename F::ResT>::ResT>;

template<typename PosList, typename NegList>
struct mgenjoin;

template<typename Pos0, typename... Pos, typename... Neg>
struct mgenjoin<mp_list<Pos0, Pos...>, mp_list<Neg...>> {
  using PosL = mp_list<Pos0, Pos...>;
  using folded = mp_fold<mp_list<Pos...>,
                         genjoin_acc<typename Pos0::ResL, typename Pos0::ResT>,
                         genjoin_step>;
  using ResL = typename folded::ResL;
  using ResT = typename folded::ResT;
  using res_tab_t = table_util::tab_t_of_row_t<ResT>;

  template<typename F>
  using opt_tab_of = std::optional<table_util::tab_t_of_row_t<typename F::ResT>>;
  template<typename F>
  using queue_of = boost::container::devector<opt_tab_of<F>>;

  std::vector<std::optional<res_tab_t>> eval(database &db, const ts_list &ts) {
    // Evaluate every child on the batch, appending outputs to its queue
    // (children progress independently; queues align them positionally, the
    // n-ary generalization of bin_op_buffer).
    feed_all(db, ts, std::index_sequence_for<Pos0, Pos..., Neg...>{});
    std::size_t n = ready(std::index_sequence_for<Pos0, Pos..., Neg...>{});
    std::vector<std::optional<res_tab_t>> res;
    res.reserve(n);
    for (std::size_t i = 0; i < n; ++i)
      res.emplace_back(reduce_one());
    return res;
  }

private:
  template<std::size_t... Is>
  void feed_all(database &db, const ts_list &ts, std::index_sequence<Is...>) {
    (feed_one<Is>(db, ts), ...);
  }

  template<std::size_t I>
  void feed_one(database &db, const ts_list &ts) {
    auto out = std::get<I>(kids_).eval(db, ts);
    auto &q = std::get<I>(qs_);
    q.insert(q.end(), std::make_move_iterator(out.begin()),
             std::make_move_iterator(out.end()));
  }

  template<std::size_t... Is>
  std::size_t ready(std::index_sequence<Is...>) const {
    return std::min({std::get<Is>(qs_).size()...});
  }

  // One aligned time-point: pop every child's front, fold the positive joins,
  // then subtract the negatives.
  std::optional<res_tab_t> reduce_one() {
    auto fronts = pop_fronts(std::index_sequence_for<Pos0, Pos..., Neg...>{});
    if (!all_pos_nonempty(fronts, std::index_sequence_for<Pos0, Pos...>{}))
      return std::nullopt;
    auto joined =
      fold_pos<1, typename Pos0::ResL>(std::move(*std::get<0>(fronts)), fronts);
    if (joined.empty())
      return std::nullopt;
    apply_negs<0>(joined, fronts);
    if (joined.empty())
      return std::nullopt;
    return joined;
  }

  template<std::size_t... Is>
  auto pop_fronts(std::index_sequence<Is...>) {
    auto fronts = std::tuple(std::move(std::get<Is>(qs_).front())...);
    (std::get<Is>(qs_).pop_front(), ...);
    return fronts;
  }

  template<typename Fronts, std::size_t... Is>
  static bool all_pos_nonempty(const Fronts &fronts,
                               std::index_sequence<Is...>) {
    return (std::get<Is>(fronts).has_value() && ...);
  }

  // Left-to-right join fold; the accumulator's layout/type change per level,
  // so this recurses on the child index with an auto return type. (An empty
  // intermediate keeps folding -- joins from an empty side are cheap -- which
  // keeps the recursion straight-line.)
  template<std::size_t I, typename CurL, typename CurTab, typename Fronts>
  auto fold_pos(CurTab cur, Fronts &fronts) {
    if constexpr (I == 1 + sizeof...(Pos)) {
      return cur;
    } else {
      using F = mp_at_c<PosL, I>;
      auto joined = table_util::table_join<CurL, typename F::ResL>(
        cur, *std::get<I>(fronts));
      using JL = typename table_util::join_result_info<
        CurL, typename F::ResL, typename CurTab::key_type,
        typename F::ResT>::ResL;
      return fold_pos<I + 1, JL>(std::move(joined), fronts);
    }
  }

  template<std::size_t J, typename Fronts>
  void apply_negs(res_tab_t &cur, Fronts &fronts) {
    if constexpr (J < sizeof...(Neg)) {
      constexpr std::size_t idx = 1 + sizeof...(Pos) + J;
      using N = mp_at_c<mp_list<Neg...>, J>;
      auto &ntab = std::get<idx>(fronts);
      if (ntab && !cur.empty())
        cur = table_util::table_anti_join<ResL, typename N::ResL>(cur, *ntab);
      apply_negs<J + 1>(cur, fronts);
    }
  }

  std::tuple<Pos0, Pos..., Neg...> kids_;
  std::tuple<queue_of<Pos0>, queue_of<Pos>..., queue_of<Neg>...> qs_;
};
