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
// Execution (WP-J2): a Leapfrog-Triejoin-style enumeration specialized per
// cluster at compile time. The global attribute order is the 6.4 heuristic
// (variables on more atoms first; ties broken by first occurrence, which the
// unique sort keys below encode exactly); each positive child's table is
// materialized per time-point as a sorted vector in the order's projection
// onto its layout (an implicit trie, 6.1); the enumeration recurses over the
// order's depths with the participating children a compile-time set per
// depth, intersecting via leapfrogging sorted ranges. Intermediate results
// are never materialized -- output rows are emitted at the deepest level
// only. Correctness is independent of the order (Generic_Join.thy locale
// getIJ: any disjoint nonempty cover; a fixed one-column-at-a-time order is
// an instance). Negatives are still applied post-hoc by anti-join in this
// work package; WP-J3 turns them into check-only vetoes inside the descent.
//
// Define STATICMON_GENJOIN_FOLD to fall back to the WP-J1 execution (a
// left-to-right fold of binary table_join / table_anti_join) for A/B runs.

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
  static constexpr std::size_t n_pos = 1 + sizeof...(Pos);
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

  // ---- global attribute order (6.4) --------------------------------------
  // Sort key per variable: atoms-containing-it (descending) with the ResL
  // position as a unique descending-encoded tie-break -- keys are distinct,
  // so no stability assumption on mp_sort is needed. WIDTH bounds formula
  // variable counts.
  static constexpr std::size_t order_width = 1024;
  template<typename V>
  using count_of =
    mp_plus<mp_if<mp_contains<typename Pos0::ResL, V>, mp_size_t<1>,
                  mp_size_t<0>>,
            mp_if<mp_contains<typename Pos::ResL, V>, mp_size_t<1>,
                  mp_size_t<0>>...>;
  template<typename V>
  using sort_key =
    mp_size_t<count_of<V>::value * order_width +
              (order_width - 1 - mp_find<ResL, V>::value)>;
  template<typename A, typename B>
  using key_greater = mp_bool<(sort_key<A>::value > sort_key<B>::value)>;

  using Order = mp_sort<ResL, key_greater>;
  static constexpr std::size_t n_depth = mp_size<Order>::value;
  static_assert(n_depth == mp_size<ResL>::value, "order must cover ResL");

  // Bound-values tuple: for each depth, the type of that variable (looked up
  // through ResL/ResT); the output row is its projection back to ResL.
  using ResTL = mp_rename<ResT, mp_list>;
  template<typename V>
  using type_of_var = mp_at<ResTL, mp_find<ResL, V>>;
  using BoundT = mp_rename<mp_transform<type_of_var, Order>, std::tuple>;
  using out_mask = table_util::get_reorder_mask<Order, ResL>;

  // Per-child projections: every child's layout is a subset of ResL, so its
  // projection onto Order is a permutation of its own layout.
  template<std::size_t I>
  using child_f = mp_at_c<PosL, I>;
  template<std::size_t I>
  using child_projL =
    mp_copy_if_q<Order, mp_bind<mp_contains, typename child_f<I>::ResL, _1>>;
  template<std::size_t I>
  using child_projmask =
    table_util::get_reorder_mask<typename child_f<I>::ResL, child_projL<I>>;
  template<std::size_t I>
  using child_projT =
    mp_rename<mp_apply_idxs<mp_rename<typename child_f<I>::ResT, mp_list>,
                            child_projmask<I>>,
              std::tuple>;

  // Does child I participate at depth D, and at which local column?
  template<std::size_t I, std::size_t D>
  static constexpr bool participates =
    mp_contains<typename child_f<I>::ResL, mp_at_c<Order, D>>::value;
  template<std::size_t I, std::size_t D>
  static constexpr std::size_t local_col =
    mp_find<child_projL<I>, mp_at_c<Order, D>>::value;

  // ---- negatives as check-only vetoes (WP-J3, verified rule 3.3) ----------
  // Each negative probes at its "coverage depth": the depth at which the last
  // of its columns is bound, i.e. the max Order-index over its variables
  // (RANF guarantees they are all in Order -- get_reorder_mask static-asserts
  // it). At that depth the bound tuple is projected onto the negative's
  // layout and looked up; a hit vetoes the partial binding before any deeper
  // enumeration. Nullary negatives (no columns) are handled by a pre-check.
  template<std::size_t J>
  using neg_f = mp_at_c<mp_list<Neg...>, J>;
  template<typename V>
  using order_idx = mp_find<Order, V>;
  template<typename A, typename B>
  using idx_max = mp_size_t<(A::value > B::value ? A::value : B::value)>;
  template<std::size_t J>
  static constexpr std::size_t neg_cov_depth =
    mp_fold<mp_transform<order_idx, typename neg_f<J>::ResL>, mp_size_t<0>,
            idx_max>::value;
  template<std::size_t J>
  using neg_probe_mask =
    table_util::get_reorder_mask<Order, typename neg_f<J>::ResL>;
  template<std::size_t J>
  static constexpr bool neg_is_nullary =
    mp_empty<typename neg_f<J>::ResL>::value;

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

  // One aligned time-point: pop every child's front, compute the positive
  // join (LFTJ enumeration, or the WP-J1 binary fold under
  // STATICMON_GENJOIN_FOLD), then subtract the negatives.
  std::optional<res_tab_t> reduce_one() {
    auto fronts = pop_fronts(std::index_sequence_for<Pos0, Pos..., Neg...>{});
    if (!all_pos_nonempty(fronts, std::index_sequence_for<Pos0, Pos...>{}))
      return std::nullopt;
#ifdef STATICMON_GENJOIN_FOLD
    auto joined =
      fold_pos<1, typename Pos0::ResL>(std::move(*std::get<0>(fronts)), fronts);
    if (joined.empty())
      return std::nullopt;
    apply_negs<0>(joined, fronts);
#else
    res_tab_t joined = leapfrog_join(fronts);  // negatives vetoed in-descent
#endif
    if (joined.empty())
      return std::nullopt;
    return joined;
  }

  // ---- LFTJ enumeration ---------------------------------------------------
  // Implicit tries: each positive child's table as a sorted vector of rows in
  // its projected column order; a (begin, end) range per child narrows as the
  // descent binds values.
  template<std::size_t I>
  static std::vector<child_projT<I>> build_trie(const auto &opt_tab) {
    std::vector<child_projT<I>> v;
    v.reserve(opt_tab->size());
    for (const auto &row : *opt_tab)
      v.push_back(project_row<child_projmask<I>>(row));
    std::sort(v.begin(), v.end());
    return v;
  }

  using ranges_t = std::array<std::pair<std::size_t, std::size_t>, n_pos>;

  template<typename Fronts>
  res_tab_t leapfrog_join(const Fronts &fronts) {
    res_tab_t out;
    // Pre-check nullary negatives: if any holds (its arity-0 relation is
    // present), it vetoes every tuple -- the whole cluster is empty.
    bool nullary_veto = false;
    [&]<std::size_t... Js>(std::index_sequence<Js...>) {
      (([&] {
         if constexpr (neg_is_nullary<Js>) {
           const auto &nt = std::get<n_pos + Js>(fronts);
           if (nt && !nt->empty())
             nullary_veto = true;
         }
       }()),
       ...);
    }(std::index_sequence_for<Neg...>{});
    if (nullary_veto)
      return out;
    auto tries = [&]<std::size_t... Is>(std::index_sequence<Is...>) {
      return std::tuple(build_trie<Is>(std::get<Is>(fronts))...);
    }(std::make_index_sequence<n_pos>{});
    ranges_t rg;
    [&]<std::size_t... Is>(std::index_sequence<Is...>) {
      ((rg[Is] = {0, std::get<Is>(tries).size()}), ...);
    }(std::make_index_sequence<n_pos>{});
    BoundT bound;
    enumerate<0>(rg, bound, out, tries, fronts);
    return out;
  }

  // Veto check for negatives whose coverage depth is exactly D: probe each
  // one's (present) table with the bound tuple projected onto its layout.
  template<std::size_t D, typename Fronts>
  static bool neg_vetoed(const BoundT &bound, const Fronts &fronts) {
    bool vetoed = false;
    [&]<std::size_t... Js>(std::index_sequence<Js...>) {
      (([&] {
         if constexpr (!neg_is_nullary<Js> && neg_cov_depth<Js> == D) {
           const auto &nt = std::get<n_pos + Js>(fronts);
           if (nt) {
             typename neg_f<Js>::ResT key(project_row<neg_probe_mask<Js>>(bound));
             if (nt->contains(key))
               vetoed = true;
           }
         }
       }()),
       ...);
    }(std::index_sequence_for<Neg...>{});
    return vetoed;
  }

  // Recursive descent over the attribute order. At depth D the participating
  // children (a compile-time set) leapfrog on their local column for
  // Order[D]: repeatedly seek every participant to the current maximum key
  // until all agree, recurse on the narrowed equal-ranges, then advance past
  // the matched value.
  template<std::size_t D, typename Tries, typename Fronts>
  void enumerate(const ranges_t &rg, BoundT &bound, res_tab_t &out,
                 const Tries &tries, const Fronts &fronts) {
    if constexpr (D == n_depth) {
      out.emplace(project_row<out_mask>(bound));
    } else {
      using VD = std::tuple_element_t<D, BoundT>;
      ranges_t cur = rg;
      for (;;) {
        // max of the participants' current keys; stop if any is exhausted
        bool exhausted = false;
        std::optional<VD> maxv;
        [&]<std::size_t... Is>(std::index_sequence<Is...>) {
          (([&] {
             if constexpr (participates<Is, D>) {
               if (exhausted)
                 return;
               if (cur[Is].first == cur[Is].second) {
                 exhausted = true;
                 return;
               }
               const auto &key = std::get<local_col<Is, D>>(
                 std::get<Is>(tries)[cur[Is].first]);
               if (!maxv || maxv < key)
                 maxv = key;
             }
           }()),
           ...);
        }(std::make_index_sequence<n_pos>{});
        if (exhausted)
          return;
        // seek every participant to >= maxv; detect agreement
        bool all_equal = true;
        [&]<std::size_t... Is>(std::index_sequence<Is...>) {
          (([&] {
             if constexpr (participates<Is, D>) {
               if (exhausted)
                 return;
               auto &r = cur[Is];
               const auto &vec = std::get<Is>(tries);
               auto key_of = [&](std::size_t p) -> const VD & {
                 return std::get<local_col<Is, D>>(vec[p]);
               };
               if (key_of(r.first) < *maxv) {
                 // galloping seek within [first, second)
                 std::size_t step = 1, lo = r.first;
                 while (lo + step < r.second && key_of(lo + step) < *maxv)
                   step <<= 1;
                 std::size_t hi = std::min(r.second, lo + step + 1);
                 while (lo < hi && key_of(lo) < *maxv)
                   ++lo;  // final linear touch-up within the last gallop step
                 r.first = lo;
                 if (r.first == r.second) {
                   exhausted = true;
                   return;
                 }
               }
               if (key_of(r.first) != *maxv)
                 all_equal = false;
             }
           }()),
           ...);
        }(std::make_index_sequence<n_pos>{});
        if (exhausted)
          return;
        if (!all_equal)
          continue;  // a seek moved past maxv; recompute the maximum
        // matched: bind, narrow each participant to its equal-range, recurse
        std::get<D>(bound) = *maxv;
        ranges_t sub = cur;
        [&]<std::size_t... Is>(std::index_sequence<Is...>) {
          (([&] {
             if constexpr (participates<Is, D>) {
               const auto &vec = std::get<Is>(tries);
               std::size_t hi = sub[Is].first;
               while (hi < sub[Is].second &&
                      std::get<local_col<Is, D>>(vec[hi]) == *maxv)
                 ++hi;
               sub[Is].second = hi;
             }
           }()),
           ...);
        }(std::make_index_sequence<n_pos>{});
        // veto-at-coverage: a negative newly covered at this depth prunes the
        // whole subtree before it is enumerated (rule 3.3)
        if (!neg_vetoed<D>(bound, fronts))
          enumerate<D + 1>(sub, bound, out, tries, fronts);
        // advance every participant past the matched value and continue
        [&]<std::size_t... Is>(std::index_sequence<Is...>) {
          (([&] {
             if constexpr (participates<Is, D>)
               cur[Is].first = sub[Is].second;
           }()),
           ...);
        }(std::make_index_sequence<n_pos>{});
      }
    }
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
