#pragma once
// Free variables of a parser::formula, matching MonPoly's MFOTL.free_vars
// (order-preserving union; Aggreg fv = res_var :: group_by; Pred fv =
// first-occurrence variable args only). Used by desugaring, translation, and
// the fused-op guards.

#include <algorithm>
#include <staticmon/parser/formula_ast.h>
#include <string>
#include <variant>
#include <vector>

namespace staticmon::compile {

namespace fv_detail {
  inline void add_unique(std::vector<std::string> &out, const std::string &v) {
    if (std::find(out.begin(), out.end(), v) == out.end())
      out.push_back(v);
  }
  inline void union_into(std::vector<std::string> &out,
                         const std::vector<std::string> &more) {
    for (const auto &v : more)
      add_unique(out, v);
  }
}  // namespace fv_detail

// Predicate.tvars: variables occurring anywhere in a term (in order).
inline void term_vars(const parser::term &t, std::vector<std::string> &out) {
  std::visit(
    [&](const auto &v) {
      using T = std::remove_cvref_t<decltype(v)>;
      if constexpr (std::is_same_v<T, parser::term_var>)
        fv_detail::add_unique(out, v.name);
      else if constexpr (std::is_same_v<T, parser::term_unary>)
        term_vars(*v.arg, out);
      else if constexpr (std::is_same_v<T, parser::term_binary>) {
        term_vars(*v.l, out);
        term_vars(*v.r, out);
      }
    },
    t.node);
}

inline std::vector<std::string> term_vars(const parser::term &t) {
  std::vector<std::string> out;
  term_vars(t, out);
  return out;
}

void free_vars(const parser::formula &f, std::vector<std::string> &out);

inline void free_vars_regex(const parser::regex &r,
                            std::vector<std::string> &out) {
  std::visit(
    [&](const auto &v) {
      using T = std::remove_cvref_t<decltype(v)>;
      if constexpr (std::is_same_v<T, parser::re_test>)
        free_vars(*v.f, out);
      else if constexpr (std::is_same_v<T, parser::re_concat> ||
                         std::is_same_v<T, parser::re_plus>) {
        free_vars_regex(*v.l, out);
        free_vars_regex(*v.r, out);
      } else if constexpr (std::is_same_v<T, parser::re_star>) {
        free_vars_regex(*v.arg, out);
      }
    },
    r.node);
}

inline void free_vars(const parser::formula &f, std::vector<std::string> &out) {
  std::visit(
    [&](const auto &v) {
      using T = std::remove_cvref_t<decltype(v)>;
      using namespace parser;
      if constexpr (std::is_same_v<T, fo_cmp>) {
        term_vars(v.l, out);
        term_vars(v.r, out);
      } else if constexpr (std::is_same_v<T, fo_matches>) {
        term_vars(v.l, out);
        term_vars(v.r, out);
        for (const auto &o : v.opts)
          if (o)
            term_vars(*o, out);
      } else if constexpr (std::is_same_v<T, fo_pred>) {
        for (const auto &a : v.args)
          if (const auto *var = std::get_if<term_var>(&a.node))
            fv_detail::add_unique(out, var->name);
      } else if constexpr (std::is_same_v<T, fo_let>) {
        free_vars(*v.body, out);
      } else if constexpr (std::is_same_v<T, fo_neg>) {
        free_vars(*v.arg, out);
      } else if constexpr (std::is_same_v<T, fo_prop>) {
        free_vars(*v.l, out);
        free_vars(*v.r, out);
      } else if constexpr (std::is_same_v<T, fo_quant>) {
        std::vector<std::string> inner;
        free_vars(*v.body, inner);
        for (auto &x : inner)
          if (std::find(v.vars.begin(), v.vars.end(), x) == v.vars.end())
            fv_detail::add_unique(out, x);
      } else if constexpr (std::is_same_v<T, fo_agg>) {
        fv_detail::add_unique(out, v.res_var);
        for (const auto &g : v.group_by)
          fv_detail::add_unique(out, g);
      } else if constexpr (std::is_same_v<T, fo_temporal_un>) {
        free_vars(*v.body, out);
      } else if constexpr (std::is_same_v<T, fo_temporal_bin>) {
        // MFOTL.free_vars: Since/Until/Trigger/Release union the RIGHT
        // operand's vars before the left (union (fv f2) (fv f1)).
        free_vars(*v.r, out);
        free_vars(*v.l, out);
      } else if constexpr (std::is_same_v<T, fo_regex>) {
        free_vars_regex(*v.re, out);
      }
    },
    f.node);
}

inline std::vector<std::string> free_vars(const parser::formula &f) {
  std::vector<std::string> out;
  free_vars(f, out);
  return out;
}

}  // namespace staticmon::compile
