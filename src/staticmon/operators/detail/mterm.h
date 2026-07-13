#pragma once
#include <boost/mp11.hpp>
#include <cstdint>
#include <staticmon/common/mp_helpers.h>
#include <staticmon/operators/detail/operator_types.h>
#include <string>
#include <string_view>

using namespace boost::mp11;

template<typename L, typename T, typename Term>
using t_res_t = typename Term::template ResT<L, T>;

template<std::size_t VarId>
struct tvar {
  template<typename L>
  using var_idx = mp_find<L, mp_size_t<VarId>>;

  template<typename L, typename T>
  using ResT = mp_at<T, var_idx<L>>;

  template<typename L, typename T>
  static ResT<L, T> eval(const T &row) {
    static_assert(var_idx<L>::value != mp_size<L>::value,
                  "term variable not found");
    return std::get<var_idx<L>::value>(row);
  }
};

template<typename Cst>
struct tcst {
  using value_type = typename Cst::value_type;

  template<typename L, typename T>
  using ResT = clean_monitor_cst_ty<value_type>;

  template<typename L, typename T>
  static ResT<L, T> eval(const T &) {
    // A string constant has value_type std::string_view but ResT
    // std::string (via clean_monitor_cst_ty); construct the string
    // explicitly since string_view -> string is not implicit. Test
    // value_type, not ResT (which is never string_view).
    if constexpr (std::is_same_v<value_type, std::string_view>)
      return std::string(Cst::value);
    else {
      return Cst::value;
    }
  }
};

template<typename Term>
struct tf2i {
  template<typename L, typename T>
  using ResT = std::int64_t;

  template<typename L, typename T>
  static std::int64_t eval(const T &row) {
    auto rec_res = Term::template eval<L, T>(row);
    static_assert(std::is_same_v<decltype(rec_res), double>,
                  "nested term of f2i must be float");
    return static_cast<std::int64_t>(rec_res);
  }
};

template<typename Term>
struct ti2f {
  template<typename L, typename T>
  using ResT = double;

  template<typename L, typename T>
  static double eval(const T &row) {
    auto rec_res = Term::template eval<L, T>(row);
    static_assert(std::is_same_v<decltype(rec_res), std::int64_t>,
                  "nested term of f2i must be float");
    return static_cast<double>(rec_res);
  }
};

template<typename Term>
struct tuminus {
  template<typename L, typename T>
  using ResT = typename Term::template result_info<L, T>::ResT;

  template<typename L, typename T>
  static ResT<L, T> eval(const T &row) {
    auto rec_res = Term::template eval<L, T>(row);
    static_assert(std::is_same_v<decltype(rec_res), ResT<L, T>>,
                  "nested term does not match computed type");
    return -rec_res;
  }
};

struct plus_tag;
struct minus_tag;
struct mult_tag;
struct mod_tag;
struct div_tag;

template<typename Tag, typename Term1, typename Term2>
struct binop_term_impl {
  template<typename L, typename T>
  using ResT = t_res_t<L, T, Term1>;

  template<typename L, typename T>
  static ResT<L, T> eval(const T &row) {
    using t1_res_t = t_res_t<L, T, Term1>;
    using t2_res_t = t_res_t<L, T, Term2>;
    static_assert(std::is_same_v<t1_res_t, t2_res_t>,
                  "computed types are not equal");
    auto rec_res1 = Term1::template eval<L, T>(row);
    auto rec_res2 = Term2::template eval<L, T>(row);
    static_assert(std::is_same_v<decltype(rec_res1), t1_res_t>,
                  "rec_res1 has wrong type");
    static_assert(std::is_same_v<decltype(rec_res2), t2_res_t>,
                  "rec_res2 has wrong type");
    if constexpr (std::is_same_v<Tag, plus_tag>) {
      return rec_res1 + rec_res2;
    } else if constexpr (std::is_same_v<Tag, minus_tag>) {
      return rec_res1 - rec_res2;
    } else if constexpr (std::is_same_v<Tag, mult_tag>) {
      return rec_res1 * rec_res2;
    } else if constexpr (std::is_same_v<Tag, mod_tag>) {
      return rec_res1 % rec_res2;
    } else if constexpr (std::is_same_v<Tag, div_tag>) {
      return rec_res1 / rec_res2;
    } else {
      static_assert(always_false_v<Tag>, "unknown tag");
    }
  }
};

template<typename Term1, typename Term2>
struct tplus : binop_term_impl<plus_tag, Term1, Term2> {};

template<typename Term1, typename Term2>
struct tminus : binop_term_impl<minus_tag, Term1, Term2> {};

template<typename Term1, typename Term2>
struct tmult : binop_term_impl<mult_tag, Term1, Term2> {};

template<typename Term1, typename Term2>
struct tmod : binop_term_impl<mod_tag, Term1, Term2> {};

template<typename Term1, typename Term2>
struct tdiv : binop_term_impl<div_tag, Term1, Term2> {};
