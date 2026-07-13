#pragma once
#include <absl/container/btree_set.h>
#include <absl/container/flat_hash_map.h>
#include <boost/mp11.hpp>
#include <cstdint>
#include <optional>
#include <staticmon/common/mp_helpers.h>
#include <staticmon/common/table.h>
#include <staticmon/operators/detail/operator_types.h>
#include <type_traits>
#include <vector>

using namespace boost::mp11;

struct max_agg_op;
struct min_agg_op;
struct avg_agg_op;
struct sum_agg_op;
struct cnt_agg_op;

template<typename AggVarT>
struct simple_combiner {
  using ResT = AggVarT;

  template<typename AggVarTU>
  simple_combiner(AggVarTU &&first_event)
      : res_(std::forward<AggVarTU>(first_event)) {}

  ResT finalize_group() const { return res_; }

  AggVarT res_;
};

template<typename AggTag, typename AggVarT>
struct agg_group {
  static_assert(always_false_v<AggTag>, "unknown aggregation type");
};

template<typename AggVarT>
struct agg_group<max_agg_op, AggVarT> : simple_combiner<AggVarT> {

  template<typename AggVarTU>
  agg_group(AggVarTU &&first_event)
      : simple_combiner<AggVarT>(std::forward<AggVarTU>(first_event)) {}

  template<typename AggVarTU>
  void add_event(AggVarTU &&val) {
    if (val > this->res_)
      this->res_ = std::forward<AggVarTU>(val);
  }
};

template<typename AggVarT>
struct agg_group<min_agg_op, AggVarT> : simple_combiner<AggVarT> {

  template<typename AggVarTU>
  agg_group(AggVarTU &&first_event)
      : simple_combiner<AggVarT>(std::forward<AggVarTU>(first_event)) {}

  template<typename AggVarTU>
  void add_event(AggVarTU &&val) {
    if (val < this->res_)
      this->res_ = std::forward<AggVarTU>(val);
  }
};

template<typename AggVarT>
struct agg_group<sum_agg_op, AggVarT> : simple_combiner<AggVarT> {
  static_assert(is_number_type<AggVarT>,
                "sum aggregation only for number types");

  agg_group(AggVarT first_event) : simple_combiner<AggVarT>(first_event) {}

  void add_event(AggVarT val) { this->res_ += val; }
};

template<typename AggVarT>
struct agg_group<cnt_agg_op, AggVarT> {
  using ResT = std::int64_t;

  template<typename AggVarTU>
  agg_group(AggVarTU &&) : counter_(1) {}

  template<typename AggVarTU>
  void add_event(AggVarTU &&) {
    counter_++;
  }

  ResT finalize_group() const { return counter_; }

  std::int64_t counter_;
};

template<typename AggVarT>
struct agg_group<avg_agg_op, AggVarT> {
  static_assert(is_number_type<AggVarT>,
                "average aggregtion only for number types");

  using ResT = double;

  agg_group(AggVarT first_event) : sum_(first_event), counter_(1) {}

  void add_event(AggVarT val) {
    sum_ += val;
    counter_++;
  }

  ResT finalize_group() const {
    return static_cast<double>(sum_) / static_cast<double>(counter_);
  }

  AggVarT sum_;
  std::size_t counter_;
};

template<typename GroupType, typename GroupVarT, typename AggVarT>
struct grouped_state {
  using ResVarT = typename GroupType::ResT;
  using ResT = mp_push_back<GroupVarT, ResVarT>;
  using res_tab_t = table_util::tab_t_of_row_t<ResT>;

  template<typename GroupTypeU, typename AggVarTU>
  void add_result(GroupTypeU &&group, AggVarTU &&val) {
    auto it = groups_.template find(group);
    if (it == groups_.end())
      groups_.emplace(std::forward<GroupTypeU>(group),
                      GroupType(std::forward<AggVarTU>(val)));
    else
      it->second.add_event(std::forward<AggVarTU>(val));
  }

  template<bool clear_groups>
  res_tab_t finalize_table() {
    if constexpr (mp_empty<GroupVarT>::value) {
      if (groups_.empty())
        return table_util::singleton_table(ResVarT{});
    }
    res_tab_t res;
    for (const auto &[group, group_state] : groups_)
      res.emplace(std::tuple_cat(
        group, std::forward_as_tuple(group_state.finalize_group())));
    if constexpr (clear_groups)
      clear();
    return res;
  }

  void clear() { groups_.clear(); }

  absl::flat_hash_map<GroupVarT, GroupType> groups_;
};

template<typename GroupType, typename AggVarT>
struct grouped_state<GroupType, std::tuple<>, AggVarT> {
  using ResVarT = typename GroupType::ResT;
  using ResT = std::tuple<ResVarT>;
  using res_tab_t = table_util::table<ResVarT>;

  template<typename AggVarTU>
  void add_result(AggVarTU &&val) {
    if (group_)
      group_->add_event(std::forward<AggVarTU>(val));
    else
      group_.emplace(std::forward<AggVarTU>(val));
  }

  template<bool clear_groups>
  res_tab_t finalize_table() {
    if (!group_)
      return table_util::singleton_table(ResVarT{});
    auto res = table_util::singleton_table(group_->finalize_group());
    if constexpr (clear_groups)
      clear();
    return res;
  }

  void clear() { group_.reset(); }

  std::optional<GroupType> group_;
};

template<typename AggVarT, typename Compare>
struct min_max_group_remove {
  using ResT = AggVarT;

  template<typename AggVarTU>
  min_max_group_remove(AggVarTU &&first_event) {
    group_state_.emplace(std::forward<AggVarTU>(first_event));
  }

  template<typename AggVarTU>
  void add_event(AggVarTU &&val) {
    group_state_.emplace(std::forward<AggVarTU>(val));
  }

  void remove_event(const AggVarT &val) {
    auto it = group_state_.find(val);
    assert(it != group_state_.end());
    group_state_.erase(it);
  }

  ResT finalize_group() const {
    assert(!group_state_.empty());
    return *group_state_.begin();
  }

  absl::btree_multiset<AggVarT, Compare> group_state_;
};

template<typename AggTag, typename AggVarT>
struct agg_group_with_remove {
  static_assert(always_false_v<AggTag>, "unknown temporal aggregation type");
};

template<typename AggVarT>
struct agg_group_with_remove<max_agg_op, AggVarT>
    : min_max_group_remove<AggVarT, std::greater<>> {
  using agg_group_with_remove::min_max_group_remove::min_max_group_remove;
};

template<typename AggVarT>
struct agg_group_with_remove<min_agg_op, AggVarT>
    : min_max_group_remove<AggVarT, std::less<>> {
  using agg_group_with_remove::min_max_group_remove::min_max_group_remove;
};

template<typename AggVarT>
struct agg_group_with_remove<sum_agg_op, AggVarT>
    : agg_group<sum_agg_op, AggVarT> {
  using agg_group_with_remove::agg_group::agg_group;
  void remove_event(const AggVarT &val) { this->res_ -= val; }
};

template<typename AggTag, typename AggVarT>
struct counted_group {
  using GroupType = agg_group_with_remove<AggTag, AggVarT>;
  using ResT = typename GroupType::ResT;

  template<typename AggVarTU>
  counted_group(AggVarTU &&first_event)
      : nested_group_(GroupType(std::forward<AggVarTU>(first_event))),
        counter_(1) {}

  bool empty() const { return counter_ == 0; }

  template<typename AggVarTU>
  void add_event(AggVarTU &&val) {
    nested_group_.add_event(std::forward<AggVarTU>(val));
    counter_++;
  }

  void remove_event(const AggVarT &val) {
    assert(counter_ > 0);
    nested_group_.remove_event(val);
    counter_--;
  }

  ResT finalize_group() const {
    assert(!empty());
    return nested_group_.finalize_group();
  }

  GroupType nested_group_;
  std::size_t counter_;
};

template<typename AggVarT>
struct counted_group<cnt_agg_op, AggVarT> : agg_group<cnt_agg_op, AggVarT> {
  using counted_group::agg_group::agg_group;

  bool empty() const { return this->counter_ == 0; }

  void remove_event(const AggVarT &) {
    assert(this->counter_ > 0);
    this->counter_--;
  }
};

template<typename AggVarT>
struct counted_group<avg_agg_op, AggVarT> : agg_group<avg_agg_op, AggVarT> {
  using counted_group::agg_group::agg_group;

  bool empty() const { return this->counter_ == 0; }

  void remove_event(AggVarT val) {
    assert(this->counter_ > 0);
    this->sum_ -= val;
    this->counter_--;
  }
};

template<typename GroupType, typename GroupVarT, typename AggVarT>
struct grouped_state_with_remove
    : grouped_state<GroupType, GroupVarT, AggVarT> {
  void remove_result(const GroupVarT &group, const AggVarT &val) {
    auto it = this->groups_.find(group);
    assert(it != this->groups_.end());
    it->second.remove_event(val);
    if (it->second.empty())
      this->groups_.erase(it);
  }
};

template<typename GroupType, typename AggVarT>
struct grouped_state_with_remove<GroupType, std::tuple<>, AggVarT>
    : grouped_state<GroupType, std::tuple<>, AggVarT> {
  void remove_result(const AggVarT &val) {
    assert(this->group_);
    this->group_->remove_event(val);
    if (this->group_->empty())
      this->group_.reset();
  }
};

template<bool counted_groups, typename L, typename T, std::size_t ResVar,
         typename AggTag, std::size_t AggVar, typename GroupVars>
struct get_agg_info {
  using agg_var_idx = mp_find_partial<L, mp_size_t<AggVar>>;
  using group_var_idxs =
    mp_transform_q<mp_bind_front<mp_find_partial, L>, GroupVars>;
  using GroupVarT = mp_apply_idxs<T, group_var_idxs>;
  using AggVarT = mp_at<T, agg_var_idx>;
  using GroupType = mp_if_c<counted_groups, counted_group<AggTag, AggVarT>,
                            agg_group<AggTag, AggVarT>>;
  using grouped_state_t =
    mp_if_c<counted_groups,
            grouped_state_with_remove<GroupType, GroupVarT, AggVarT>,
            grouped_state<GroupType, GroupVarT, AggVarT>>;
};

template<bool counted_groups, typename L, typename T, std::size_t ResVar,
         typename AggTag, std::size_t AggVar, typename GroupVars>
struct aggregation_impl : get_agg_info<counted_groups, L, T, ResVar, AggTag,
                                       AggVar, GroupVars>::grouped_state_t {
  using AggInfo =
    get_agg_info<counted_groups, L, T, ResVar, AggTag, AggVar, GroupVars>;
  using group_var_idxs = typename AggInfo::group_var_idxs;
  using GroupVarT = typename AggInfo::GroupVarT;
  using grouped_state_t = typename AggInfo::grouped_state_t;
  using agg_var_idx = typename AggInfo::agg_var_idx;
  using AggVarT = typename AggInfo::AggVarT;

  using ResL =
    mp_push_back<mp_apply_idxs<L, group_var_idxs>, mp_size_t<ResVar>>;
  using ResT = mp_push_back<GroupVarT, typename grouped_state_t::ResVarT>;
  using res_tab_t = table_util::tab_t_of_row_t<ResT>;

  template<typename TU>
  void add_row(TU &&row) {
    if constexpr (mp_empty<GroupVars>::value) {
      this->add_result(std::get<agg_var_idx::value>(std::move(row)));
    } else {
      // Copy (not reference) the aggregation value into a local before
      // projecting the group: when the aggregation column is also a group-by
      // column, projecting the group moves that column, which would otherwise
      // leave the aggregate reading a moved-from (e.g. empty string) value
      // (argument evaluation order is unspecified).
      auto agg_var = std::get<agg_var_idx::value>(row);
      this->add_result(project_row<group_var_idxs>(std::move(row)),
                       std::move(agg_var));
    }
  }
};


template<std::size_t ResVar, typename AggTag, std::size_t AggVar,
         typename GroupVars, typename MFormula>
struct maggregation
    : aggregation_impl<false, typename MFormula::ResL, typename MFormula::ResT,
                       ResVar, AggTag, AggVar, GroupVars> {
  using Base = typename maggregation::aggregation_impl;
  using res_tab_t = table_util::tab_t_of_row_t<typename Base::ResT>;

  std::vector<std::optional<res_tab_t>> eval(database &db, const ts_list &ts) {
    auto rec_tabs = f_.eval(db, ts);
    std::vector<std::optional<res_tab_t>> res;
    res.reserve(rec_tabs.size());
    for (auto &tab : rec_tabs) {
      if (tab) {
        for (auto it = tab->cbegin(), last = tab->cend(); it != last;) {
          auto node = tab->extract(it++);
          this->add_row(node.value());
        }
      }
      // An empty relation (tab == nullopt) contributes no rows; finalize_table
      // then yields the empty-group-by default (0) or, with a group-by, no
      // rows -- matching MonPoly. (The previous `assert(!tab->empty())` here
      // dereferenced the empty optional.)
      auto res_tab = this->template finalize_table<true>();
      if (res_tab.empty())
        res.emplace_back();
      else
        res.emplace_back(std::move(res_tab));
    }
    return res;
  }

  MFormula f_;
};

template<typename L, typename T, typename ResVar, typename AggTag,
         typename AggVar, typename GroupVars>

struct addonlyaggregation : aggregation_impl<false, L, T, ResVar::value, AggTag,
                                             AggVar::value, GroupVars> {};

template<typename L, typename T, typename ResVar, typename AggTag,
         typename AggVar, typename GroupVars>
struct aggregationwithremove
    : aggregation_impl<true, L, T, ResVar::value, AggTag, AggVar::value,
                       GroupVars> {
  using Base = typename aggregationwithremove::aggregation_impl;
  using group_var_idxs = typename Base::group_var_idxs;
  using agg_var_idx = typename Base::agg_var_idx;

  void remove_row(const T &row) {
    if constexpr (mp_empty<GroupVars>::value)
      this->remove_result(std::get<agg_var_idx::value>(row));
    else
      this->remove_result(project_row<group_var_idxs>(row),
                          std::get<agg_var_idx::value>(row));
  }
};
