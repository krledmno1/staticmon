#pragma once
// The exformula intermediate representation: a value-typed mirror of the
// `exformula` / `term` / `simple_op` types of MonPoly's -explicitmon codegen
// (explicitmon.ml; see docs/explicitmon-pipeline.md §4). This is the target of
// stage 4 (translation) and the input to stage 5 (codegen). It is a plain
// algebraic value type — NOT templates; the templates are the emitted output.
//
// Traceability: constructor names mirror the OCaml ones so the port stays
// auditable against that backend.

#include <cstdint>
#include <memory>
#include <optional>
#include <staticmon/parser/formula_ast.h>  // reuse big_int, constant, agg_op
#include <string>
#include <variant>
#include <vector>

namespace staticmon::compile {

using parser::agg_op;
using parser::big_int;
using parser::constant;

using var_id = std::size_t;
using pred_id = std::size_t;

// Concrete argument type of a predicate slot / value (MonPoly tcst minus
// TRegexp, which the backend does not support).
enum class val_type { t_int, t_str, t_float };

// ---- exformula terms (post-translation; variables are ids) ---------------
enum class ex_term_unop { f2i, i2f, uminus };
enum class ex_term_binop { plus, minus, mult, div, mod };

struct ex_term;
using ex_term_ptr = std::shared_ptr<ex_term>;

struct ex_tvar {
  var_id id;
};
struct ex_tcst {
  constant value;
};
struct ex_tunary {
  ex_term_unop op;
  ex_term_ptr arg;
};
struct ex_tbinary {
  ex_term_binop op;
  ex_term_ptr l, r;
};

struct ex_term {
  std::variant<ex_tvar, ex_tcst, ex_tunary, ex_tbinary> node;
};

inline ex_term_ptr mk_ex_term(ex_term t) {
  return std::make_shared<ex_term>(std::move(t));
}

// ---- predicate arguments -------------------------------------------------
struct ex_pvar {
  val_type type;
  var_id id;
};
struct ex_pcst {
  constant value;
};
using ex_predarg = std::variant<ex_pvar, ex_pcst>;

// ---- intervals (translated: open bounds folded into closed +/-1) ---------
struct ex_bnd {
  big_int value;
};
struct ex_inf {};
using ex_bound = std::variant<ex_bnd, ex_inf>;

struct ex_interval {
  ex_bound lower, upper;
};

// ---- simple (fusable, row-local) operations ------------------------------
enum class cst_type { eq, less, less_eq };

struct sop_and_assign {
  var_id res_var;
  ex_term term;
};
struct sop_and_rel {
  bool negated;
  cst_type op;
  ex_term l, r;
};
struct sop_exists {
  std::vector<var_id> vars;
};
using simple_op = std::variant<sop_and_assign, sop_and_rel, sop_exists>;

// ---- aggregation info ----------------------------------------------------
struct ex_aggreg_info {
  var_id res_var;
  agg_op op;
  var_id agg_var;
  std::vector<var_id> group_by;
};

enum class join_type { nat_join, anti_join };

// ---- exformula -----------------------------------------------------------
struct exformula;
using exformula_ptr = std::shared_ptr<exformula>;

struct ex_predicate {
  pred_id id;
  std::vector<ex_predarg> args;
};
struct ex_builtin_tp {
  ex_predarg arg;
};
struct ex_builtin_ts {
  ex_predarg arg;
};
struct ex_builtin_tpts {
  ex_predarg arg1, arg2;
};
struct ex_and {
  join_type jt;
  exformula_ptr l, r;
};
struct ex_or {
  exformula_ptr l, r;
};
struct ex_neg {
  exformula_ptr arg;
};
struct ex_eq {
  ex_term l, r;
};
struct ex_empty_rel {};
struct ex_temporal_un {  // prev/next/once/eventually
  enum class kind { prev, next, once, eventually } k;
  ex_interval intv;
  exformula_ptr arg;
};
struct ex_since {  // since/until
  bool future;     // until if true, since if false
  bool negated;    // left operand was negated
  ex_interval intv;
  exformula_ptr l, r;
};
struct ex_once_agg {
  ex_aggreg_info info;
  ex_interval intv;
  exformula_ptr arg;
};
struct ex_since_agg {
  ex_aggreg_info info;
  bool negated;
  ex_interval intv;
  exformula_ptr l, r;
};
struct ex_aggregation {
  ex_aggreg_info info;
  exformula_ptr arg;
};
struct ex_fused {
  std::vector<simple_op> sops;
  exformula_ptr arg;
};
struct ex_let {
  bool past;                        // LETPAST if true, LET otherwise
  pred_id id;                       // id of the bound predicate
  std::vector<var_id> pred_layout;  // PredL: parameter var ids, in order
  exformula_ptr bound;              // f1 (the definition)
  exformula_ptr body;               // f2 (uses the bound predicate)
};

struct exformula {
  std::variant<ex_predicate, ex_builtin_tp, ex_builtin_ts, ex_builtin_tpts,
               ex_and, ex_or, ex_neg, ex_eq, ex_empty_rel, ex_temporal_un,
               ex_since, ex_once_agg, ex_since_agg, ex_aggregation, ex_fused,
               ex_let>
    node;
};

inline exformula_ptr mk_exformula(exformula f) {
  return std::make_shared<exformula>(std::move(f));
}

// Result of translation: the formula plus the metadata codegen needs.
struct pred_info {
  std::string name;
  std::size_t arity;
  pred_id id;
  std::vector<val_type> arg_types;
};

struct translated {
  exformula_ptr formula;
  std::vector<var_id> free_variables;  // in the input formula's order
  std::vector<pred_info> predicates;   // in Pred_map fold order
};

}  // namespace staticmon::compile
