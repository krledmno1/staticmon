#pragma once
// AST for MFOTL/MFODL formulas, structurally equivalent to the newest
// MonPoly's MFOTL.formula / Predicate.term (see docs/monpoly-grammar.md).
// This is the runtime parsing layer: plain value types, no metaprogramming.
// Deviation from monpoly: integers keep arbitrary precision as normalized
// decimal strings (monpoly uses zarith); regexp constants keep the pattern
// only (monpoly also stores the compiled Str.regexp).

#include <cassert>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace staticmon::parser {

// Arbitrary-precision integer, canonical decimal form ("-7", "0", "123").
// Canonical: no leading zeros, no "-0". Produced by normalize().
struct big_int {
  std::string dec;

  static big_int normalize(std::string_view s) {
    bool neg = !s.empty() && s.front() == '-';
    std::size_t i = neg ? 1 : 0;
    while (i + 1 < s.size() && s[i] == '0')
      ++i;
    std::string digits(s.substr(i));
    if (digits == "0")
      neg = false;
    return big_int{neg ? "-" + digits : digits};
  }

  friend bool operator==(const big_int &, const big_int &) = default;
};

// Constants ------------------------------------------------------------
struct cst_int {
  big_int value;
};
struct cst_float {
  double value;
};
// Content between the quotes, escape sequences kept verbatim (monpoly strips
// quotes but does not unescape).
struct cst_str {
  std::string value;
};
struct cst_regexp {
  std::string pattern;
};
using constant = std::variant<cst_int, cst_float, cst_str, cst_regexp>;

// Terms ----------------------------------------------------------------
enum class term_unop {
  f2i,
  i2f,
  i2s,
  s2i,
  f2s,
  s2f,
  day_of_month,
  month,
  year,
  format_date,
  r2s,
  s2r,
  uminus,
};

enum class term_binop { plus, minus, mult, div, mod };

struct term;
using term_ptr = std::unique_ptr<term>;

struct term_var {
  std::string name;
};
struct term_cst {
  constant value;
};
struct term_unary {
  term_unop op;
  term_ptr arg;
};
struct term_binary {
  term_binop op;
  term_ptr l, r;
};

struct term {
  std::variant<term_var, term_cst, term_unary, term_binary> node;
};

// Intervals ------------------------------------------------------------
struct bnd_open {
  big_int value;
};
struct bnd_closed {
  big_int value;
};
struct bnd_inf {};
using interval_bound = std::variant<bnd_open, bnd_closed, bnd_inf>;

struct interval {
  interval_bound lower, upper;
};

// [0, *) — the default when a temporal operator has no explicit interval.
inline interval default_interval() {
  return interval{bnd_closed{big_int{"0"}}, bnd_inf{}};
}

// Formulas -------------------------------------------------------------
enum class agg_op { cnt, min, max, sum, avg, med };

enum class cmp_op { equal, less, less_eq, substring };

enum class prop_binop { and_, or_, implies, equiv };

enum class temporal_unop { prev, next, eventually, once, always, past_always };

enum class temporal_binop { since, trigger, until, release };

enum class let_kind { let, let_past, frz };

struct formula;
using formula_ptr = std::unique_ptr<formula>;

struct regex;
using regex_ptr = std::unique_ptr<regex>;

struct fo_pred {
  std::string name;
  std::vector<term> args;  // arity == args.size()
};

// t1 <op> t2. `>`/`>=` are represented flipped as less/less_eq (parser does
// the swap, like monpoly's grammar actions).
struct fo_cmp {
  cmp_op op;
  term l, r;
};

struct fo_matches {
  term l, r;
  // absent xopttermlist == empty list; `_` == nullopt
  std::vector<std::optional<term>> opts;
};

struct fo_let {
  let_kind kind;
  fo_pred head;
  formula_ptr bound, body;
};

struct fo_neg {
  formula_ptr arg;
};

struct fo_prop {
  prop_binop op;
  formula_ptr l, r;
};

struct fo_quant {
  bool universal;
  std::vector<std::string> vars;  // nonempty (parser rejects empty)
  formula_ptr body;
};

struct fo_agg {
  std::string res_var;
  agg_op op;
  std::string agg_var;
  std::vector<std::string> group_by;
  formula_ptr body;
};

struct fo_temporal_un {
  temporal_unop op;
  interval intv;
  formula_ptr body;
};

struct fo_temporal_bin {
  temporal_binop op;
  interval intv;
  formula_ptr l, r;
};

struct fo_regex {
  bool future;  // Frex if true, Prex otherwise
  interval intv;
  regex_ptr re;
};

struct formula {
  std::variant<fo_cmp, fo_matches, fo_pred, fo_let, fo_neg, fo_prop, fo_quant,
               fo_agg, fo_temporal_un, fo_temporal_bin, fo_regex>
    node;
};

// Regexes (MFODL) --------------------------------------------------------
struct re_skip {
  std::size_t n;  // always 1 from the parser (`.` = wild)
};
struct re_test {
  formula_ptr f;
};
struct re_concat {
  regex_ptr l, r;
};
struct re_plus {
  regex_ptr l, r;
};
struct re_star {
  regex_ptr arg;
};

struct regex {
  std::variant<re_skip, re_test, re_concat, re_plus, re_star> node;
};

// Convenience constructors ----------------------------------------------
inline term_ptr mk_term(term t) { return std::make_unique<term>(std::move(t)); }
inline formula_ptr mk_formula(formula f) {
  return std::make_unique<formula>(std::move(f));
}
inline regex_ptr mk_regex(regex r) {
  return std::make_unique<regex>(std::move(r));
}

}  // namespace staticmon::parser
