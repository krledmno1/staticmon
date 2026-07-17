#pragma once
// Stage 4 (translation): desugared parser::formula -> exformula IR, mirroring
// explicitmon.ml (MonPoly's -explicitmon fork) translate_formula / transform_fused_op
// (see docs/explicitmon-pipeline.md §4). Assumes the input is desugared
// (no Implies/Equiv/ForAll/Always/PastAlways) and monitorable; callers gate
// on monitorability (stage 3) first.
//
// Deviation D1: Div -> tdiv, Mod -> tmod, F2i/I2f kept as tf2i/ti2f (the C++
// backend supports all of these; the -explicitmon codegen had bugs here). An
// intentional divergence from that backend; these constructs are validated
// behaviorally (against VeriMon) rather than structurally.

#include <algorithm>
#include <map>
#include <optional>
#include <staticmon/compile/exformula.h>
#include <staticmon/compile/free_vars.h>
#include <staticmon/parser/formula_ast.h>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace staticmon::compile {

struct translate_error {
  std::string message;
};

// Signature lookup: predicate name -> ordered argument types.
using schema_map = std::map<std::string, std::vector<val_type>>;

using let_param_types_map =
  std::map<const parser::fo_let *, std::vector<val_type>>;

class translator {
public:
  explicit translator(schema_map schema, let_param_types_map let_types = {})
      : schema_(std::move(schema)), let_param_types_(std::move(let_types)) {}

  // Translates a desugared formula. `orig_free` is the free-variable name
  // order of the original formula (for the free_variables output).
  translated run(const parser::formula &f,
                 const std::vector<std::string> &orig_free) {
    curr_id_ = 1;  // matches explicitmon's module-init incr (ids start at 2)
    exformula_ptr root = translate(f);
    translated out;
    out.formula = root;
    for (const auto &name : orig_free)
      out.free_variables.push_back(lookup_var(name));
    // Pred_map fold order: descending (name, arity).
    std::vector<pred_info> preds = predicates_;
    std::sort(preds.begin(), preds.end(), [](const auto &a, const auto &b) {
      if (a.name != b.name)
        return a.name > b.name;
      return a.arity > b.arity;
    });
    out.predicates = std::move(preds);
    return out;
  }

private:
  [[noreturn]] void fail(std::string m) {
    throw translate_error{std::move(m)};
  }

  // ---- id management (var and pred share one counter) --------------------
  var_id maybe_add_var(const std::string &name) {
    auto it = vmap_.find(name);
    if (it != vmap_.end())
      return it->second;
    var_id id = ++curr_id_;
    vmap_[name] = id;
    return id;
  }
  var_id lookup_var(const std::string &name) {
    auto it = vmap_.find(name);
    if (it == vmap_.end())
      fail("free variable not found: " + name);
    return it->second;
  }

  pred_id maybe_add_pred(const std::string &name, std::size_t arity,
                         const std::vector<val_type> &tys) {
    auto key = std::pair(name, arity);
    auto it = pmap_.find(key);
    if (it != pmap_.end())
      return it->second;
    pred_id id = ++curr_id_;
    pmap_[key] = id;
    predicates_.push_back(pred_info{name, arity, id, tys});
    return id;
  }

  // ---- terms -------------------------------------------------------------
  ex_term translate_term(const parser::term &t) {
    using namespace parser;
    return std::visit(
      [&](const auto &v) -> ex_term {
        using T = std::remove_cvref_t<decltype(v)>;
        if constexpr (std::is_same_v<T, term_var>) {
          return ex_term{ex_tvar{maybe_add_var(v.name)}};
        } else if constexpr (std::is_same_v<T, term_cst>) {
          return ex_term{ex_tcst{v.value}};
        } else if constexpr (std::is_same_v<T, term_unary>) {
          ex_term_unop op;
          switch (v.op) {
            case term_unop::f2i: op = ex_term_unop::f2i; break;
            case term_unop::i2f: op = ex_term_unop::i2f; break;
            case term_unop::uminus: op = ex_term_unop::uminus; break;
            default: fail("unsupported unary term (string/date conversion)");
          }
          return ex_term{ex_tunary{op, mk_ex_term(translate_term(*v.arg))}};
        } else {  // term_binary
          ex_term_binop op;
          switch (v.op) {
            case term_binop::plus: op = ex_term_binop::plus; break;
            case term_binop::minus: op = ex_term_binop::minus; break;
            case term_binop::mult: op = ex_term_binop::mult; break;
            case term_binop::div: op = ex_term_binop::div; break;
            case term_binop::mod: op = ex_term_binop::mod; break;
          }
          return ex_term{ex_tbinary{op, mk_ex_term(translate_term(*v.l)),
                                    mk_ex_term(translate_term(*v.r))}};
        }
      },
      t.node);
  }

  static val_type cst_val_type(const parser::constant &c) {
    if (std::holds_alternative<parser::cst_int>(c))
      return val_type::t_int;
    if (std::holds_alternative<parser::cst_float>(c))
      return val_type::t_float;
    return val_type::t_str;  // cst_str; regexp unsupported upstream
  }

  // ---- predicates --------------------------------------------------------
  std::vector<ex_predarg> translate_pred_args(const std::string &name,
                                              const std::vector<parser::term> &terms,
                                              std::vector<val_type> &out_tys) {
    auto sit = schema_.find(name);
    if (sit == schema_.end())
      fail("predicate not in signature: " + name);
    const auto &slot_tys = sit->second;
    if (slot_tys.size() != terms.size())
      fail("arity mismatch for predicate: " + name);
    std::vector<ex_predarg> args;
    for (std::size_t i = 0; i < terms.size(); ++i) {
      const auto &term = terms[i];
      if (const auto *var = std::get_if<parser::term_var>(&term.node)) {
        val_type ty = slot_tys[i];
        args.push_back(ex_pvar{ty, maybe_add_var(var->name)});
        out_tys.push_back(ty);
      } else if (const auto *cst = std::get_if<parser::term_cst>(&term.node)) {
        args.push_back(ex_pcst{cst->value});
        out_tys.push_back(cst_val_type(cst->value));
      } else {
        fail("predicate argument must be a variable or constant");
      }
    }
    return args;
  }

  exformula_ptr translate_pred(const parser::fo_pred &p) {
    std::vector<val_type> tys;
    auto args = translate_pred_args(p.name, p.args, tys);
    std::size_t arity = p.args.size();
    if (p.name == "tp" && arity == 1)
      return mk_exformula(exformula{ex_builtin_tp{args[0]}});
    if (p.name == "ts" && arity == 1)
      return mk_exformula(exformula{ex_builtin_ts{args[0]}});
    if (p.name == "tpts" && arity == 2)
      return mk_exformula(exformula{ex_builtin_tpts{args[0], args[1]}});
    pred_id id = maybe_add_pred(p.name, arity, tys);
    return mk_exformula(exformula{ex_predicate{id, std::move(args)}});
  }

  // ---- intervals ---------------------------------------------------------
  static ex_bound translate_bound(const parser::interval_bound &b, bool upper) {
    using namespace parser;
    if (std::holds_alternative<bnd_inf>(b))
      return ex_inf{};
    if (const auto *o = std::get_if<bnd_open>(&b)) {
      // open: lower -> +1, upper -> -1
      long long n = std::stoll(o->value.dec);
      n += upper ? -1 : 1;
      return ex_bnd{big_int::normalize(std::to_string(n))};
    }
    const auto &c = std::get<bnd_closed>(b);
    return ex_bnd{c.value};
  }
  static ex_interval translate_interval(const parser::interval &i) {
    return ex_interval{translate_bound(i.lower, false),
                       translate_bound(i.upper, true)};
  }

  // ---- FRZ analyses (on the desugared parser AST) --------------------------

  // How the FRZ body uses the frozen predicate: 0 = not at all, 1 = only at
  // the current time-point, 2 = under a temporal operator (or inside a nested
  // binder's definition, whose query index is not statically the outer one).
  // With no temporal-position use, FRZ coincides with LET: the body only ever
  // queries the predicate at the outer time-point, which is where LET's
  // binding and FRZ's frozen relation agree. (MonPoly dispatches on the body
  // containing any temporal operator at all; checking the *predicate's*
  // positions is a strictly wider cheap path with the same argument.)
  static int frz_occurrence(const parser::formula &f, const std::string &name,
                            std::size_t arity, bool temporal) {
    using namespace parser;
    return std::visit(
      [&](const auto &v) -> int {
        using T = std::remove_cvref_t<decltype(v)>;
        if constexpr (std::is_same_v<T, fo_pred>) {
          if (v.name == name && v.args.size() == arity)
            return temporal ? 2 : 1;
          return 0;
        } else if constexpr (std::is_same_v<T, fo_cmp> ||
                             std::is_same_v<T, fo_matches>) {
          return 0;
        } else if constexpr (std::is_same_v<T, fo_neg>) {
          return frz_occurrence(*v.arg, name, arity, temporal);
        } else if constexpr (std::is_same_v<T, fo_prop>) {
          return std::max(frz_occurrence(*v.l, name, arity, temporal),
                          frz_occurrence(*v.r, name, arity, temporal));
        } else if constexpr (std::is_same_v<T, fo_quant>) {
          return frz_occurrence(*v.body, name, arity, temporal);
        } else if constexpr (std::is_same_v<T, fo_agg>) {
          return frz_occurrence(*v.body, name, arity, temporal);
        } else if constexpr (std::is_same_v<T, fo_temporal_un>) {
          return frz_occurrence(*v.body, name, arity, true);
        } else if constexpr (std::is_same_v<T, fo_temporal_bin>) {
          return std::max(frz_occurrence(*v.l, name, arity, true),
                          frz_occurrence(*v.r, name, arity, true));
        } else if constexpr (std::is_same_v<T, fo_let>) {
          // A nested binder shadows the predicate for the scopes where its own
          // binding is visible: the body always, the definition only for
          // LETPAST (recursive). Occurrences inside a (non-shadowed) nested
          // definition count as temporal: the definition is re-evaluated at
          // whatever indices the nested predicate is queried at.
          bool shadows = v.head.name == name && v.head.args.size() == arity;
          int b = 0;
          if (!(shadows && v.kind == let_kind::let_past))
            b = frz_occurrence(*v.bound, name, arity, true);
          int c = shadows ? 0 : frz_occurrence(*v.body, name, arity, temporal);
          return std::max(b, c);
        } else {  // fo_regex: not in the backend fragment; be conservative
          return 2;
        }
      },
      f.node);
  }

  // Does the formula reference any predicate in `keys` (name, arity), other
  // than `own`? Shadowing by inner binders is ignored (over-approximates,
  // which only disables an optimization). Used to disable mfrz's bounded-past
  // window when the body reads an enclosing-binder-bound predicate: such a
  // predicate's database stream is positional (aligned from time-point 0, and
  // possibly lagging), so a replay must start at 0 to stay aligned.
  static bool references_bound_pred(
    const parser::formula &f,
    const std::vector<std::pair<std::string, std::size_t>> &keys,
    const std::pair<std::string, std::size_t> &own) {
    using namespace parser;
    return std::visit(
      [&](const auto &v) -> bool {
        using T = std::remove_cvref_t<decltype(v)>;
        if constexpr (std::is_same_v<T, fo_pred>) {
          std::pair<std::string, std::size_t> k{v.name, v.args.size()};
          if (k == own)
            return false;
          return std::find(keys.begin(), keys.end(), k) != keys.end();
        } else if constexpr (std::is_same_v<T, fo_cmp> ||
                             std::is_same_v<T, fo_matches>) {
          return false;
        } else if constexpr (std::is_same_v<T, fo_neg>) {
          return references_bound_pred(*v.arg, keys, own);
        } else if constexpr (std::is_same_v<T, fo_prop>) {
          return references_bound_pred(*v.l, keys, own) ||
                 references_bound_pred(*v.r, keys, own);
        } else if constexpr (std::is_same_v<T, fo_quant> ||
                             std::is_same_v<T, fo_agg>) {
          return references_bound_pred(*v.body, keys, own);
        } else if constexpr (std::is_same_v<T, fo_temporal_un>) {
          return references_bound_pred(*v.body, keys, own);
        } else if constexpr (std::is_same_v<T, fo_temporal_bin>) {
          return references_bound_pred(*v.l, keys, own) ||
                 references_bound_pred(*v.r, keys, own);
        } else if constexpr (std::is_same_v<T, fo_let>) {
          return references_bound_pred(*v.bound, keys, own) ||
                 references_bound_pred(*v.body, keys, own);
        } else {  // fo_regex: conservative
          return true;
        }
      },
      f.node);
  }

  // MonPoly's body_depth (algorithm.ml): the furthest timestamp-distance the
  // FRZ body can look into the past, defined only for purely bounded-past
  // bodies -- boolean/quantifier/aggregation structure over atoms and
  // ONCE/SINCE with finite interval upper bounds. PREV/NEXT (time-point
  // relative), future operators, nested binders and regex disable the window
  // (nullopt = replay the whole prefix).
  static std::optional<std::size_t> frz_body_depth(const parser::formula &f) {
    using namespace parser;
    auto ub = [](const interval &i) -> std::optional<std::size_t> {
      return std::visit(
        [](const auto &b) -> std::optional<std::size_t> {
          using B = std::remove_cvref_t<decltype(b)>;
          if constexpr (std::is_same_v<B, bnd_inf>) {
            return std::nullopt;
          } else {
            try {
              std::size_t v = std::stoull(b.value.dec);
              return v;
            } catch (...) {
              return std::nullopt;  // absurdly large: treat as unbounded
            }
          }
        },
        i.upper);
    };
    return std::visit(
      [&](const auto &v) -> std::optional<std::size_t> {
        using T = std::remove_cvref_t<decltype(v)>;
        if constexpr (std::is_same_v<T, fo_pred> || std::is_same_v<T, fo_cmp> ||
                      std::is_same_v<T, fo_matches>) {
          return 0;
        } else if constexpr (std::is_same_v<T, fo_neg>) {
          return frz_body_depth(*v.arg);
        } else if constexpr (std::is_same_v<T, fo_quant>) {
          return frz_body_depth(*v.body);
        } else if constexpr (std::is_same_v<T, fo_agg>) {
          return frz_body_depth(*v.body);
        } else if constexpr (std::is_same_v<T, fo_prop>) {
          auto a = frz_body_depth(*v.l), b = frz_body_depth(*v.r);
          if (a && b)
            return std::max(*a, *b);
          return std::nullopt;
        } else if constexpr (std::is_same_v<T, fo_temporal_un>) {
          if (v.op != temporal_unop::once)
            return std::nullopt;  // prev/next/eventually (post-desugar set)
          auto u = ub(v.intv);
          auto d = frz_body_depth(*v.body);
          if (u && d)
            return *u + *d;
          return std::nullopt;
        } else if constexpr (std::is_same_v<T, fo_temporal_bin>) {
          if (v.op != temporal_binop::since)
            return std::nullopt;
          auto u = ub(v.intv);
          auto a = frz_body_depth(*v.l), b = frz_body_depth(*v.r);
          if (u && a && b)
            return *u + std::max(*a, *b);
          return std::nullopt;
        } else {  // fo_let (nested binders), fo_regex
          return std::nullopt;
        }
      },
      f.node);
  }

  // ---- fused-op guards (explicitmon.ml is_and_relop/is_special_case/...) --
  static bool subset(const std::vector<std::string> &a,
                     const std::vector<std::string> &b) {
    for (const auto &x : a)
      if (std::find(b.begin(), b.end(), x) == b.end())
        return false;
    return true;
  }

  static bool is_and_relop(const parser::formula &f) {
    using namespace parser;
    if (const auto *c = std::get_if<fo_cmp>(&f.node)) {
      (void) c;
      return true;  // Equal/Less/LessEq/Substring
    }
    if (std::holds_alternative<fo_matches>(f.node))
      return true;
    if (const auto *n = std::get_if<fo_neg>(&f.node)) {
      const auto &inner = n->arg->node;
      return std::holds_alternative<fo_cmp>(inner) ||
             std::holds_alternative<fo_matches>(inner);
    }
    return false;
  }

  static bool is_special_case(const std::vector<std::string> &fv1,
                              const parser::formula &f) {
    using namespace parser;
    if (const auto *c = std::get_if<fo_cmp>(&f.node)) {
      if (c->op == cmp_op::equal) {
        bool l_var = std::holds_alternative<term_var>(c->l.node);
        bool r_var = std::holds_alternative<term_var>(c->r.node);
        if (l_var && subset(term_vars(c->r), fv1))
          return true;
        if (r_var && subset(term_vars(c->l), fv1))
          return true;
        return subset(free_vars(f), fv1);
      }
      // Less / LessEq / Substring
      return subset(free_vars(f), fv1);
    }
    if (const auto *m = std::get_if<fo_matches>(&f.node)) {
      if (!subset(term_vars(m->l), fv1) || !subset(term_vars(m->r), fv1))
        return false;
      for (const auto &o : m->opts) {
        if (!o)
          continue;
        if (std::holds_alternative<term_var>(o->node))
          continue;
        if (!subset(term_vars(*o), fv1))
          return false;
      }
      return true;
    }
    if (std::holds_alternative<fo_neg>(f.node))
      return subset(free_vars(f), fv1);
    return false;
  }

  bool is_special_and(const parser::formula &f1, const parser::formula &f2) {
    return is_and_relop(f2) && is_special_case(free_vars(f1), f2);
  }

  // is_safe_assignment f1 (Equal x y)
  static bool is_safe_assignment(const std::vector<std::string> &fv1,
                                 const parser::fo_cmp &eq) {
    using namespace parser;
    const bool l_var = std::holds_alternative<term_var>(eq.l.node);
    const bool r_var = std::holds_alternative<term_var>(eq.r.node);
    auto in = [&](const std::string &n) {
      return std::find(fv1.begin(), fv1.end(), n) != fv1.end();
    };
    if (l_var && r_var) {
      const auto &a = std::get<term_var>(eq.l.node).name;
      const auto &b = std::get<term_var>(eq.r.node).name;
      return in(a) == !in(b);
    }
    if (l_var) {
      const auto &a = std::get<term_var>(eq.l.node).name;
      return !in(a) && subset(term_vars(eq.r), fv1);
    }
    if (r_var) {
      const auto &b = std::get<term_var>(eq.r.node).name;
      return !in(b) && subset(term_vars(eq.l), fv1);
    }
    return false;
  }

  // ---- fused-op transformation ------------------------------------------
  exformula_ptr transform_fused_op(std::vector<simple_op> sops,
                                   const parser::formula &f) {
    using namespace parser;
    if (const auto *q = std::get_if<fo_quant>(&f.node); q && !q->universal) {
      // Exists: fresh (shadowing) ids for bound vars, prepend MExists.
      std::vector<var_id> new_ids;
      std::vector<std::pair<std::string, std::optional<var_id>>> saved;
      for (const auto &name : q->vars) {
        auto old = vmap_.count(name) ? std::optional<var_id>(vmap_[name])
                                     : std::nullopt;
        saved.emplace_back(name, old);
        var_id id = ++curr_id_;
        vmap_[name] = id;
        new_ids.push_back(id);
      }
      std::vector<simple_op> next = std::move(sops);
      next.insert(next.begin(), sop_exists{new_ids});
      exformula_ptr res = transform_fused_op(std::move(next), *q->body);
      for (auto &[name, old] : saved) {
        if (old)
          vmap_[name] = *old;
        else
          vmap_.erase(name);
      }
      return res;
    }
    if (const auto *a = std::get_if<fo_prop>(&f.node);
        a && a->op == prop_binop::and_) {
      const auto &f1 = *a->l;
      const auto &f2 = *a->r;
      if (const auto *eq = std::get_if<fo_cmp>(&f2.node);
          eq && eq->op == cmp_op::equal && is_special_and(f1, f2) &&
          is_safe_assignment(free_vars(f1), *eq)) {
        return translate_safe_assignment(std::move(sops), f1, *eq);
      }
      if (is_special_and(f1, f2)) {
        return translate_constraint(std::move(sops), f1, f2);
      }
    }
    // base case
    exformula_ptr inner = translate(f);
    return mk_exformula(exformula{ex_fused{std::move(sops), inner}});
  }

  exformula_ptr translate_safe_assignment(std::vector<simple_op> sops,
                                          const parser::formula &f1,
                                          const parser::fo_cmp &eq) {
    using namespace parser;
    auto fv1 = free_vars(f1);
    auto in = [&](const std::string &n) {
      return std::find(fv1.begin(), fv1.end(), n) != fv1.end();
    };
    simple_op sop = [&]() -> simple_op {
      const bool l_var = std::holds_alternative<term_var>(eq.l.node);
      const bool r_var = std::holds_alternative<term_var>(eq.r.node);
      if (l_var && r_var) {
        const auto &x = std::get<term_var>(eq.l.node).name;
        const auto &y = std::get<term_var>(eq.r.node).name;
        bool x_free = in(x);
        var_id xi = maybe_add_var(x);
        var_id yi = maybe_add_var(y);
        if (x_free)
          return sop_and_assign{yi, ex_term{ex_tvar{xi}}};
        return sop_and_assign{xi, ex_term{ex_tvar{yi}}};
      }
      // t = Var x  or  Var x = t
      if (l_var) {
        var_id xi = maybe_add_var(std::get<term_var>(eq.l.node).name);
        return sop_and_assign{xi, translate_term(eq.r)};
      }
      var_id xi = maybe_add_var(std::get<term_var>(eq.r.node).name);
      return sop_and_assign{xi, translate_term(eq.l)};
    }();
    sops.insert(sops.begin(), std::move(sop));
    return transform_fused_op(std::move(sops), f1);
  }

  exformula_ptr translate_constraint(std::vector<simple_op> sops,
                                     const parser::formula &f1,
                                     const parser::formula &f2) {
    using namespace parser;
    auto make_rel = [&](bool neg, const fo_cmp &c) -> simple_op {
      cst_type op = c.op == cmp_op::equal      ? cst_type::eq
                    : c.op == cmp_op::less      ? cst_type::less
                    : c.op == cmp_op::less_eq   ? cst_type::less_eq
                                                : cst_type::eq;
      if (c.op == cmp_op::substring)
        fail("SUBSTRING not supported by the backend");
      return sop_and_rel{neg, op, translate_term(c.l), translate_term(c.r)};
    };
    simple_op sop = [&]() -> simple_op {
      if (const auto *c = std::get_if<fo_cmp>(&f2.node))
        return make_rel(false, *c);
      if (const auto *n = std::get_if<fo_neg>(&f2.node))
        if (const auto *c = std::get_if<fo_cmp>(&n->arg->node))
          return make_rel(true, *c);
      fail("not a constraint");
    }();
    sops.insert(sops.begin(), std::move(sop));
    return transform_fused_op(std::move(sops), f1);
  }

  // ---- main translation --------------------------------------------------
  static bool is_neg_eq_same_var(const parser::formula &f) {
    using namespace parser;
    if (const auto *n = std::get_if<fo_neg>(&f.node))
      if (const auto *c = std::get_if<fo_cmp>(&n->arg->node))
        if (c->op == cmp_op::equal) {
          const auto *a = std::get_if<term_var>(&c->l.node);
          const auto *b = std::get_if<term_var>(&c->r.node);
          return a && b && a->name == b->name;
        }
    return false;
  }

  exformula_ptr translate(const parser::formula &f) {
    using namespace parser;
    return std::visit(
      [&](const auto &v) -> exformula_ptr {
        using T = std::remove_cvref_t<decltype(v)>;
        if constexpr (std::is_same_v<T, fo_pred>) {
          return translate_pred(v);
        } else if constexpr (std::is_same_v<T, fo_temporal_un>) {
          auto intv = translate_interval(v.intv);
          switch (v.op) {
            case temporal_unop::prev:
            case temporal_unop::next:
            case temporal_unop::once:
            case temporal_unop::eventually: {
              ex_temporal_un::kind k =
                v.op == temporal_unop::prev   ? ex_temporal_un::kind::prev
                : v.op == temporal_unop::next ? ex_temporal_un::kind::next
                : v.op == temporal_unop::once ? ex_temporal_un::kind::once
                                              : ex_temporal_un::kind::eventually;
              return mk_exformula(exformula{
                ex_temporal_un{k, intv, translate(*v.body)}});
            }
            default:
              fail("temporal operator should have been desugared");
          }
        } else if constexpr (std::is_same_v<T, fo_temporal_bin>) {
          auto intv = translate_interval(v.intv);
          bool future = v.op == temporal_binop::until;
          if (v.op != temporal_binop::since && v.op != temporal_binop::until)
            fail("TRIGGER/RELEASE not supported by the backend");
          // Since(Neg f1, f2) -> negated left operand.
          const auto *neg = std::get_if<fo_neg>(&v.l->node);
          bool negated = neg != nullptr;
          exformula_ptr l = translate(negated ? *neg->arg : *v.l);
          exformula_ptr r = translate(*v.r);
          return mk_exformula(exformula{ex_since{future, negated, intv, l, r}});
        } else if constexpr (std::is_same_v<T, fo_prop>) {
          if (v.op == prop_binop::or_) {
            return mk_exformula(exformula{
              ex_or{translate(*v.l), translate(*v.r)}});
          }
          if (v.op == prop_binop::and_) {
            const auto &f1 = *v.l;
            const auto &f2 = *v.r;
            if (is_special_and(f1, f2))
              return transform_fused_op({}, f);
            if (const auto *n = std::get_if<fo_neg>(&f2.node))
              return mk_exformula(exformula{
                ex_and{join_type::anti_join, translate(f1),
                       translate(*n->arg)}});
            return mk_exformula(exformula{
              ex_and{join_type::nat_join, translate(f1), translate(f2)}});
          }
          fail("propositional operator should have been desugared");
        } else if constexpr (std::is_same_v<T, fo_cmp>) {
          if (v.op == cmp_op::equal)
            return mk_exformula(exformula{
              ex_eq{translate_term(v.l), translate_term(v.r)}});
          fail("bare comparison is not monitorable");
        } else if constexpr (std::is_same_v<T, fo_neg>) {
          if (is_neg_eq_same_var(f))
            return mk_exformula(exformula{ex_empty_rel{}});
          return mk_exformula(exformula{ex_neg{translate(*v.arg)}});
        } else if constexpr (std::is_same_v<T, fo_quant>) {
          if (!v.universal)
            return transform_fused_op({}, f);
          fail("FORALL should have been desugared");
        } else if constexpr (std::is_same_v<T, fo_agg>) {
          return translate_aggregation(v);
        } else if constexpr (std::is_same_v<T, fo_let>) {
          return translate_let(v);
        } else {
          fail("unsupported fragment (regex/matches/substring at top)");
        }
      },
      f.node);
  }

  exformula_ptr translate_aggregation(const parser::fo_agg &a) {
    // Variables of the enclosing scope, captured before the body introduces its
    // own. An aggregation projects its body down to the group-by columns plus
    // the result, but variables from a sibling/outer scope must survive it --
    // e.g. `p(a) AND (y <- CNT x q(x))` is a join whose free `a` comes from the
    // sibling. explicitmon's filter_vars keeps only the group-by vars, which
    // silently drops such variables whenever the aggregation is not the
    // outermost operand (monpoly's default backend accepts these formulas).
    auto outer = vmap_;
    // Translate body first (populates vmap with agg/group-by/inner vars).
    exformula_ptr body = translate(*a.body);
    var_id agg_id = lookup_var(a.agg_var);
    std::vector<var_id> gids;
    for (const auto &g : a.group_by)
      gids.push_back(lookup_var(g));
    // Restrict to the enclosing vars + group-by columns, dropping the body's
    // inner (aggregated/local) variables. Keep any enclosing binding of the
    // result variable so maybe_add_var reuses its id: when an outer `EXISTS`
    // binds the result (e.g. `EXISTS b. (b <- CNT c f)`), the MExists projects
    // that id, so the aggregation must produce the same one; when the result is
    // free it is simply absent here and gets a fresh id.
    std::map<std::string, var_id> restricted = std::move(outer);
    for (const auto &g : a.group_by)
      restricted[g] = vmap_[g];
    vmap_ = std::move(restricted);
    var_id res_id = maybe_add_var(a.res_var);
    ex_aggreg_info info{res_id, a.op, agg_id, gids};

    if (auto *once = std::get_if<ex_temporal_un>(&body->node);
        once && once->k == ex_temporal_un::kind::once) {
      return mk_exformula(exformula{
        ex_once_agg{info, once->intv, once->arg}});
    }
    if (auto *since = std::get_if<ex_since>(&body->node);
        since && !since->future) {
      return mk_exformula(exformula{
        ex_since_agg{info, since->negated, since->intv, since->l, since->r}});
    }
    return mk_exformula(exformula{ex_aggregation{info, body}});
  }

  exformula_ptr translate_let(const parser::fo_let &l) {
    const bool past = (l.kind == parser::let_kind::let_past);
    const bool frz = (l.kind == parser::let_kind::frz);

    std::vector<std::string> params;
    for (const auto &a : l.head.args) {
      const auto *var = std::get_if<parser::term_var>(&a.node);
      if (!var)
        fail("LET parameters must be variables");
      params.push_back(var->name);
    }
    std::size_t arity = params.size();

    // FRZ mode dispatch (MonPoly's static/per-instance split, refined to the
    // frozen predicate's positions): unused -> the body alone; used only at
    // the current time-point -> plain LET; used under a temporal operator ->
    // the per-outer-time-point mfrz runtime, with a bounded-past replay
    // window when the body's temporal depth is finite.
    int frz_occ = 0;
    std::optional<std::size_t> frz_depth;
    if (frz) {
      frz_occ = frz_occurrence(*l.body, l.head.name, arity, false);
      // Temporal-mode FRZ records the input batches it sees; inside a LETPAST
      // definition the enclosing recursion evaluates with synthetic empty-ts
      // batches carrying recursive predicate data, which the history cannot
      // represent faithfully. Reject rather than risk wrong verdicts.
      if (frz_occ >= 2 && in_letpast_bound_)
        fail("FRZ with a temporally-used frozen predicate inside a LETPAST "
             "definition is not supported");
      if (frz_occ >= 2) {
        frz_depth = frz_body_depth(*l.body);
        // A body reading an enclosing-binder-bound predicate must replay from
        // time-point 0: those database streams are positionally aligned from
        // the start (and can lag behind the trace), so a windowed mid-stream
        // start would misalign them.
        if (frz_depth &&
            references_bound_pred(*l.body, bound_scope_,
                                  {l.head.name, arity}))
          frz_depth.reset();
      }
    }

    // Bound-predicate column types come from the type checker.
    auto tit = let_param_types_.find(&l);
    if (tit == let_param_types_.end())
      fail("internal: LET parameter types unavailable");
    const std::vector<val_type> &param_types = tit->second;

    // Shadow the parameter names with fresh ids for f1's scope.
    std::vector<var_id> param_ids;
    std::vector<std::pair<std::string, std::optional<var_id>>> saved_vars;
    for (const auto &p : params) {
      auto old = vmap_.count(p) ? std::optional<var_id>(vmap_[p]) : std::nullopt;
      saved_vars.emplace_back(p, old);
      var_id id = ++curr_id_;
      vmap_[p] = id;
      param_ids.push_back(id);
    }

    // Bind the predicate: a fresh id shadowing any signature predicate of the
    // same name, with the inferred parameter types. LETPAST is recursive, so
    // it is in scope inside f1; LET is in scope only in f2.
    auto pkey = std::pair(l.head.name, arity);
    std::optional<pred_id> saved_pmap =
      pmap_.count(pkey) ? std::optional<pred_id>(pmap_[pkey]) : std::nullopt;
    std::optional<std::vector<val_type>> saved_schema =
      schema_.count(l.head.name)
        ? std::optional<std::vector<val_type>>(schema_[l.head.name])
        : std::nullopt;
    pred_id lid = ++curr_id_;
    auto bind_pred = [&]() {
      pmap_[pkey] = lid;
      schema_[l.head.name] = param_types;
    };

    if (past) {
      bind_pred();
      bound_scope_.push_back(pkey);  // recursive: in scope inside f1 too
    }
    bool saved_in_lp = in_letpast_bound_;
    if (past)
      in_letpast_bound_ = true;
    exformula_ptr f1 = translate(*l.bound);
    in_letpast_bound_ = saved_in_lp;
    // PredL must match f1's actual output layout. Some operators (notably
    // aggregation) reassign a parameter's id during translation, so read the
    // ids back from the environment after f1 rather than trusting the shadow
    // ids assigned above.
    std::vector<var_id> pred_layout;
    for (const auto &p : params)
      pred_layout.push_back(lookup_var(p));
    if (!past) {
      bind_pred();
      bound_scope_.push_back(pkey);
    }
    for (auto &[name, old] : saved_vars) {
      if (old)
        vmap_[name] = *old;
      else
        vmap_.erase(name);
    }
    exformula_ptr f2 = translate(*l.body);
    bound_scope_.pop_back();

    // Restore the predicate binding.
    pmap_.erase(pkey);
    if (saved_pmap)
      pmap_[pkey] = *saved_pmap;
    if (saved_schema)
      schema_[l.head.name] = *saved_schema;
    else
      schema_.erase(l.head.name);

    if (frz) {
      if (frz_occ == 0)
        // The body never queries the frozen predicate: FRZ p = f1 IN f2 is
        // equivalent to f2 (f1 stays translated above so its trace predicates
        // are registered, but its node is dropped).
        return f2;
      if (frz_occ == 1)
        // Every use is at the current time-point, where LET's per-index
        // binding and FRZ's frozen relation agree.
        return mk_exformula(
          exformula{ex_let{false, lid, std::move(pred_layout), f1, f2}});
      return mk_exformula(
        exformula{ex_frz{lid, std::move(pred_layout), frz_depth, f1, f2}});
    }
    return mk_exformula(
      exformula{ex_let{past, lid, std::move(pred_layout), f1, f2}});
  }

  schema_map schema_;
  let_param_types_map let_param_types_;
  var_id curr_id_ = 1;
  std::map<std::string, var_id> vmap_;
  std::map<std::pair<std::string, std::size_t>, pred_id> pmap_;
  // Binder-bound predicates currently in scope (LET/LETPAST/FRZ), innermost
  // last; consulted by the FRZ window analysis.
  std::vector<std::pair<std::string, std::size_t>> bound_scope_;
  // Inside a LETPAST definition (its recursion feeds synthetic batches that
  // temporal-mode FRZ cannot replay -- rejected there).
  bool in_letpast_bound_ = false;
  std::vector<pred_info> predicates_;
};

}  // namespace staticmon::compile
