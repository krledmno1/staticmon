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

  // Can this (desugared) formula's output stream lag behind the time-points
  // fed to it? Any future operator delays verdicts, so an operator whose
  // definition contains one emits fewer tables than the batch has time-points
  // until later input resolves them. Conservative recursion into nested
  // binders and regex.
  static bool has_future_op(const parser::formula &f) {
    using namespace parser;
    return std::visit(
      [&](const auto &v) -> bool {
        using T = std::remove_cvref_t<decltype(v)>;
        if constexpr (std::is_same_v<T, fo_pred> || std::is_same_v<T, fo_cmp> ||
                      std::is_same_v<T, fo_matches>) {
          return false;
        } else if constexpr (std::is_same_v<T, fo_neg>) {
          return has_future_op(*v.arg);
        } else if constexpr (std::is_same_v<T, fo_prop>) {
          return has_future_op(*v.l) || has_future_op(*v.r);
        } else if constexpr (std::is_same_v<T, fo_quant> ||
                             std::is_same_v<T, fo_agg>) {
          return has_future_op(*v.body);
        } else if constexpr (std::is_same_v<T, fo_temporal_un>) {
          if (v.op == temporal_unop::next ||
              v.op == temporal_unop::eventually)
            return true;
          return has_future_op(*v.body);
        } else if constexpr (std::is_same_v<T, fo_temporal_bin>) {
          if (v.op == temporal_binop::until ||
              v.op == temporal_binop::release)
            return true;
          return has_future_op(*v.l) || has_future_op(*v.r);
        } else if constexpr (std::is_same_v<T, fo_let>) {
          return has_future_op(*v.bound) || has_future_op(*v.body);
        } else {  // fo_regex: conservative
          return true;
        }
      },
      f.node);
  }

  // Does the formula reference any predicate in `keys` (name, arity), other
  // than `own`? Shadowing by inner binders is ignored (over-approximates,
  // which only disables an optimization). Used to disable mfrz's bounded-past
  // window when the body reads an enclosing-binder-bound predicate whose
  // stream can LAG: such a stream is positional (aligned from time-point 0,
  // with fewer entries than the batch has time-points until later input
  // resolves them), so a mid-stream replay start would misalign it.
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

  // Depth contribution of referencing a predicate bound INSIDE the analyzed
  // body (its definition is recomputed by the replay, so its temporal reach
  // flows into the reference). Predicates absent from the map contribute 0:
  // trace predicates, the frozen predicate's broadcast, and (lag-free)
  // enclosing binders, whose values are recorded data in the replayed history,
  // never recomputed. A nullopt VALUE means any reference makes the depth
  // unbounded (LETPAST: the recursion reaches arbitrarily far back).
  using depth_env = std::map<std::pair<std::string, std::size_t>,
                             std::optional<std::size_t>>;

  // The furthest timestamp-distance the FRZ body can look into the past
  // (MonPoly's body_depth, extended compositionally through nested binders --
  // docs/optimization-plan.md, opt B): boolean/quantifier/aggregation
  // structure over atoms, ONCE/SINCE with finite interval upper bounds, and
  //   - nested FRZ p = a IN b: max(depth a, depth b with p -> 0) -- the frozen
  //     p is CONSTANT at the node's index, so temporal operators over it add
  //     no data reach beyond a at that index;
  //   - nested LET p = a IN b: depth b with p -> depth a -- a p-atom at
  //     temporal offset o contributes o + depth a (p@j = a@j, recomputed by
  //     the instance from replayed data);
  //   - nested LETPAST: p -> unbounded.
  // PREV/NEXT (time-point relative), future operators and regex disable the
  // window (nullopt = replay the whole prefix).
  static std::optional<std::size_t> frz_body_depth(const parser::formula &f,
                                                   const depth_env &env) {
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
        if constexpr (std::is_same_v<T, fo_pred>) {
          auto it = env.find({v.name, v.args.size()});
          if (it == env.end())
            return 0;  // trace / frozen / enclosing (recorded data)
          return it->second;  // body-bound: its definition's reach (or nullopt)
        } else if constexpr (std::is_same_v<T, fo_cmp> ||
                             std::is_same_v<T, fo_matches>) {
          return 0;
        } else if constexpr (std::is_same_v<T, fo_neg>) {
          return frz_body_depth(*v.arg, env);
        } else if constexpr (std::is_same_v<T, fo_quant>) {
          return frz_body_depth(*v.body, env);
        } else if constexpr (std::is_same_v<T, fo_agg>) {
          return frz_body_depth(*v.body, env);
        } else if constexpr (std::is_same_v<T, fo_prop>) {
          auto a = frz_body_depth(*v.l, env), b = frz_body_depth(*v.r, env);
          if (a && b)
            return std::max(*a, *b);
          return std::nullopt;
        } else if constexpr (std::is_same_v<T, fo_temporal_un>) {
          if (v.op != temporal_unop::once)
            return std::nullopt;  // prev/next/eventually (post-desugar set)
          auto u = ub(v.intv);
          auto d = frz_body_depth(*v.body, env);
          if (u && d)
            return *u + *d;
          return std::nullopt;
        } else if constexpr (std::is_same_v<T, fo_temporal_bin>) {
          if (v.op != temporal_binop::since)
            return std::nullopt;
          auto u = ub(v.intv);
          auto a = frz_body_depth(*v.l, env), b = frz_body_depth(*v.r, env);
          if (u && a && b)
            return *u + std::max(*a, *b);
          return std::nullopt;
        } else if constexpr (std::is_same_v<T, fo_let>) {
          std::pair<std::string, std::size_t> key{v.head.name,
                                                  v.head.args.size()};
          if (v.kind == let_kind::frz) {
            // The inner frozen predicate is constant at the node's index:
            // references cost 0, and the node's own reach is the larger of
            // the definition's and the body's.
            auto da = frz_body_depth(*v.bound, env);
            depth_env e2 = env;
            e2[key] = std::size_t{0};
            auto db = frz_body_depth(*v.body, e2);
            if (da && db)
              return std::max(*da, *db);
            return std::nullopt;
          }
          if (v.kind == let_kind::let) {
            // p@j = a@j, recomputed from replayed data: each reference
            // contributes the definition's reach at its temporal offset.
            auto da = frz_body_depth(*v.bound, env);
            depth_env e2 = env;
            e2[key] = da;  // nullopt definition -> unbounded on reference
            return frz_body_depth(*v.body, e2);
          }
          // LETPAST: the recursion reaches arbitrarily far back; any
          // reference is unbounded (an unreferenced definition is harmless).
          depth_env e2 = env;
          e2[key] = std::nullopt;
          return frz_body_depth(*v.body, e2);
        } else {  // fo_regex
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
            if (auto gj = try_translate_cluster(f))
              return gj;
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

  // ---- conjunction-cluster flattening (docs/LFTJ-STATICMON.md) ------------
  // The front-end counterpart of VeriMon's verified convert_multiway pass:
  // collect the maximal plain-join/anti-join AND chain rooted at f, and --
  // when the 6.5 shape gate holds -- emit one ex_genjoin instead of the
  // binary chain. Fused shapes (constraint/assignment conjuncts) stop the
  // descent and enter the cluster as single positive children, so the
  // existing msimpleop machinery around them is untouched.

  // In-order collection preserves the left-to-right positive order, which
  // keeps mgenjoin's folded layout identical to the binary chain's.
  void collect_cluster(const parser::formula &f,
                       std::vector<const parser::formula *> &pos,
                       std::vector<const parser::formula *> &neg) {
    using namespace parser;
    auto descend_or_add = [&](const formula &g, bool neg_position) {
      const auto *a = std::get_if<fo_prop>(&g.node);
      if (a && a->op == prop_binop::and_ && !is_special_and(*a->l, *a->r)) {
        collect_cluster(g, pos, neg);
        return;
      }
      if (neg_position) {
        if (const auto *n = std::get_if<fo_neg>(&g.node)) {
          neg.push_back(&*n->arg);
          return;
        }
      }
      pos.push_back(&g);
    };
    const auto *a = std::get_if<fo_prop>(&f.node);
    descend_or_add(*a->l, false);
    // only the right operand of an AND is an anti-join position (mirrors the
    // binary translation)
    descend_or_add(*a->r, true);
  }

  // The 6.5 shape gate: >= 3 conjuncts; or 2 positives whose shared-variable
  // set is a strict subset of both (the quadratic-intermediate pattern); or a
  // negative with >= 2 positives (subsumed by >= 3, kept for clarity).
  static bool cluster_gate(const std::vector<const parser::formula *> &pos,
                           const std::vector<const parser::formula *> &neg) {
    if (pos.size() + neg.size() >= 3)
      return true;
    if (pos.size() == 2 && neg.empty()) {
      auto fv1 = free_vars(*pos[0]), fv2 = free_vars(*pos[1]);
      std::vector<std::string> shared;
      for (const auto &x : fv1)
        if (std::find(fv2.begin(), fv2.end(), x) != fv2.end())
          shared.push_back(x);
      return shared.size() < fv1.size() && shared.size() < fv2.size();
    }
    return false;
  }

  exformula_ptr try_translate_cluster(const parser::formula &f) {
    using namespace parser;
    std::vector<const formula *> pos, neg;
    collect_cluster(f, pos, neg);
    if (!cluster_gate(pos, neg))
      return nullptr;
    // MAnds side conditions (RANF; Monitor.thy): >= 1 positive, and every
    // negative's free variables covered by the positives' union. Guaranteed
    // by monitorability -- enforce as a hard error to catch translator bugs.
    if (pos.empty())
      fail("internal: conjunction cluster without a positive conjunct");
    std::vector<std::string> pos_fv;
    for (const auto *p : pos)
      for (const auto &x : free_vars(*p))
        if (std::find(pos_fv.begin(), pos_fv.end(), x) == pos_fv.end())
          pos_fv.push_back(x);
    for (const auto *n : neg)
      if (!subset(free_vars(*n), pos_fv))
        fail("internal: cluster negative not covered by the positives");
    ex_genjoin node;
    for (const auto *p : pos)
      node.pos.push_back(translate(*p));
    for (const auto *n : neg)
      node.neg.push_back(translate(*n));
    return mk_exformula(exformula{std::move(node)});
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
        frz_depth = frz_body_depth(*l.body, {});
        // A body reading a LAGGING enclosing-binder-bound predicate must
        // replay from time-point 0: a lagging stream is positional (aligned
        // from the start, with fewer entries per batch than time-points until
        // later input resolves them), so a windowed mid-stream start would
        // misalign it. Lag-free enclosing predicates -- one table per fed
        // time-point in every batch -- are per-batch self-contained in the
        // recorded history and window safely; their values are replayed data,
        // never recomputed, so they contribute no depth either.
        if (frz_depth) {
          std::vector<std::pair<std::string, std::size_t>> lagging;
          for (const auto &e : bound_scope_)
            if (!e.lag_free)
              lagging.push_back({e.name, e.arity});
          if (!lagging.empty() &&
              references_bound_pred(*l.body, lagging, {l.head.name, arity}))
            frz_depth.reset();
        }
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

    // Whether this binder's database stream can lag (see scope_entry). For a
    // FRZ the runtime representation decides: temporal mode broadcasts and is
    // always lag-free; the current-only (mlet) path carries the definition's
    // possibly-lagging output stream, exactly like LET. An elided FRZ
    // (frz_occ == 0) is never referenced, so its value is irrelevant.
    bool lag_free =
      !past && (frz ? (frz_occ >= 2 || !has_future_op(*l.bound))
                    : !has_future_op(*l.bound));

    if (past) {
      bind_pred();
      // recursive: in scope inside f1 too
      bound_scope_.push_back({l.head.name, arity, lag_free});
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
      bound_scope_.push_back({l.head.name, arity, lag_free});
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
  // last; consulted by the FRZ window analysis. `lag_free` records whether the
  // predicate's database stream is complete per batch (one table per fed
  // time-point) -- it depends on the binder's RUNTIME representation, not its
  // surface kind:
  //   - temporal-mode FRZ (mfrz): always lag-free -- instances are created
  //     only once the frozen verdict exists, and feed() broadcasts exactly
  //     ts.size() tables in every batch; future operators in the definition
  //     delay instance creation, never the per-batch completeness of p;
  //   - current-only FRZ (compiled to mlet) and LET: the entry is the
  //     definition's output stream, which lags iff the definition contains a
  //     future operator;
  //   - LETPAST: conservatively lagging (its recursion feeds synthetic
  //     batches).
  struct scope_entry {
    std::string name;
    std::size_t arity;
    bool lag_free;
  };
  std::vector<scope_entry> bound_scope_;
  // Inside a LETPAST definition (its recursion feeds synthetic batches that
  // temporal-mode FRZ cannot replay -- rejected there).
  bool in_letpast_bound_ = false;
  std::vector<pred_info> predicates_;
};

}  // namespace staticmon::compile
