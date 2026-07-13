#pragma once
// Stage 2 (type inference/check): a faithful port of MonPoly's
// Rewriting.type_check_term / type_check_formula / check_syntax
// (monpoly-develop/src/rewriting.ml:1282-1774). Symbolic type variables with
// two classes (Num/Any) are unified by whole-environment substitution; a
// type clash throws type_error (matching monpoly's Fatal error). Unresolved
// type variables default to TFloat, exactly as check_syntax does.
//
// Scope: the staticmon backend fragment. Let/LetPast/Frz and regex are
// reported as unsupported (they are rejected downstream anyway).

#include <algorithm>
#include <map>
#include <optional>
#include <stdexcept>
#include <staticmon/compile/free_vars.h>
#include <staticmon/compile/exformula.h>  // val_type
#include <staticmon/parser/formula_ast.h>
#include <string>
#include <variant>
#include <vector>

namespace staticmon::compile {

struct type_error {
  std::string message;
};

// tcst = TInt | TStr | TFloat | TRegexp
enum class tcst { t_int, t_str, t_float, t_regexp };
// tcl = TNum | TAny
enum class tcl { num, any };

// tsymb = TSymb (tcl, int) | TCst tcst
struct type_val {
  bool is_cst;
  tcst cst;   // valid iff is_cst
  tcl cls;    // valid iff !is_cst
  int idx;    // valid iff !is_cst

  static type_val of_cst(tcst c) { return {true, c, tcl::any, 0}; }
  static type_val symb(tcl cl, int n) { return {false, tcst::t_int, cl, n}; }

  friend bool operator==(const type_val &a, const type_val &b) {
    if (a.is_cst != b.is_cst)
      return false;
    if (a.is_cst)
      return a.cst == b.cst;
    return a.cls == b.cls && a.idx == b.idx;
  }
};

inline std::string string_of_type(const type_val &t) {
  if (t.is_cst)
    switch (t.cst) {
      case tcst::t_int: return "Int";
      case tcst::t_float: return "Float";
      case tcst::t_str: return "String";
      case tcst::t_regexp: return "Regexp";
    }
  if (t.cls == tcl::num)
    return "(Num t" + std::to_string(t.idx) + ") =>  t" + std::to_string(t.idx);
  return "t" + std::to_string(t.idx);
}

class type_checker {
public:
  // schema: predicate (name,arity) -> declared concrete slot types.
  struct pred_key {
    std::string name;
    std::size_t arity;
    friend bool operator==(const pred_key &, const pred_key &) = default;
  };

  explicit type_checker(std::vector<std::pair<pred_key, std::vector<tcst>>> sig) {
    for (auto &[k, tys] : sig) {
      std::vector<type_val> slot;
      for (tcst t : tys)
        slot.push_back(type_val::of_cst(t));
      sch_.emplace_back(k, std::move(slot));
    }
  }

  // Returns free-variable types in `orig_order`, matching monpoly -sigout
  // (unresolved symbolic types default to Float). Throws type_error on clash.
  std::vector<std::pair<std::string, tcst>> check(
    const parser::formula &f, const std::vector<std::string> &orig_order) {
    // Seed Γ with a fresh Any symbol per free variable (check_syntax order:
    // fold_left prepends, so later free vars get lower symbol indices; the
    // absolute indices do not affect the result, only clash/relations do).
    vars_.clear();
    for (const auto &v : free_vars(f))
      vars_.insert(vars_.begin(), {v, new_symbol(tcl::any)});
    type_check_formula(f);
    std::vector<std::pair<std::string, tcst>> out;
    for (const auto &name : orig_order) {
      type_val t = assoc(name);
      out.emplace_back(name, t.is_cst ? t.cst : tcst::t_float);
    }
    return out;
  }

  // Inferred parameter types of each LET/LETPAST predicate, keyed by the AST
  // node (populated during check()). The translator needs these because a
  // bound predicate's column types are not in the signature.
  const std::map<const parser::fo_let *, std::vector<tcst>> &
  let_param_types() const {
    return let_param_types_;
  }

private:
  // ---- type relations (rewriting.ml) ------------------------------------
  // t1 |<=| t2 : t1 is at least as specific as t2.
  static bool leq(const type_val &t1, const type_val &t2) {
    if (t1.is_cst)
      return true;  // TCst _ , _ -> true
    if (!t2.is_cst) {
      if (t1.cls == tcl::num && t2.cls == tcl::num)
        return t1.idx <= t2.idx;
      if (t1.cls == tcl::any && t2.cls == tcl::any)
        return t1.idx <= t2.idx;
      if (t1.cls == tcl::num)
        return true;  // TSymb(TNum,_), TSymb(_,_)
      return false;   // TSymb(TAny,_), TSymb(TNum,_)
    }
    // t1 symbolic, t2 concrete:  TSymb(TNum,_), <cst> -> true; else false
    return false;  // matches "| TSymb _ , _ -> false"
  }

  static bool clash(const type_val &t1, const type_val &t2) {
    if (t1.is_cst && t2.is_cst)
      return t1.cst != t2.cst;
    auto num_str = [](const type_val &a, const type_val &b) {
      return !a.is_cst && a.cls == tcl::num && b.is_cst &&
             (b.cst == tcst::t_str || b.cst == tcst::t_regexp);
    };
    return num_str(t1, t2) || num_str(t2, t1);
  }

  static type_val more_spec(const type_val &t1, const type_val &t2) {
    return leq(t1, t2) ? t1 : t2;
  }

  // ---- fresh symbol ------------------------------------------------------
  int max_symbol_index() const {
    int m = 0;
    auto scan = [&](const type_val &t) {
      if (!t.is_cst)
        m = std::max(m, t.idx);
    };
    for (const auto &[k, tys] : sch_)
      for (const auto &t : tys)
        scan(t);
    for (const auto &[v, t] : vars_)
      scan(t);
    return m;
  }
  type_val new_symbol(tcl cl) { return type_val::symb(cl, max_symbol_index() + 1); }

  static int max_symbol_in_vars(
    const std::vector<std::pair<std::string, type_val>> &vs) {
    int m = 0;
    for (const auto &[v, t] : vs)
      if (!t.is_cst)
        m = std::max(m, t.idx);
    return m;
  }
  int max_symbol_in(
    const std::vector<std::pair<pred_key, std::vector<type_val>>> &sch,
    const std::vector<std::pair<std::string, type_val>> &vs) const {
    int m = 0;
    for (const auto &[k, tys] : sch)
      for (const auto &t : tys)
        if (!t.is_cst)
          m = std::max(m, t.idx);
    m = std::max(m, max_symbol_in_vars(vs));
    return m;
  }

  // ---- environment -------------------------------------------------------
  bool mem_assoc(const std::string &v) const {
    for (const auto &[n, t] : vars_)
      if (n == v)
        return true;
    return false;
  }
  type_val assoc(const std::string &v) const {
    for (const auto &[n, t] : vars_)
      if (n == v)
        return t;
    throw type_error{"internal: variable not in Γ: " + v};
  }

  [[noreturn]] void raise_clash(const parser::term *t, const type_val &expected,
                                const type_val &actual) {
    std::string ts = t ? term_string(*t) : "<term>";
    throw type_error{"[Rewriting.type_check_term] Type check error on term " +
                     ts + ": expected type " + string_of_type(expected) +
                     ", actual type " + string_of_type(actual)};
  }

  // propagate_constraints: replace the less-specific type with the more-
  // specific one across the whole environment (Δ and Γ).
  void propagate(const type_val &t1, const type_val &t2) {
    type_val oldt = leq(t1, t2) ? t2 : t1;
    type_val newt = leq(t1, t2) ? t1 : t2;
    if (oldt == newt)
      return;
    for (auto &[k, tys] : sch_)
      for (auto &t : tys)
        if (t == oldt)
          t = newt;
    for (auto &[v, t] : vars_)
      if (t == oldt)
        t = newt;
  }

  void check_and_propagate(const type_val &expected, const type_val &actual,
                           const parser::term *t) {
    if (clash(expected, actual))
      raise_clash(t, expected, actual);
    propagate(expected, actual);
  }

  // ---- terms -------------------------------------------------------------
  static tcst cst_type(const parser::constant &c) {
    if (std::holds_alternative<parser::cst_int>(c))
      return tcst::t_int;
    if (std::holds_alternative<parser::cst_float>(c))
      return tcst::t_float;
    if (std::holds_alternative<parser::cst_str>(c))
      return tcst::t_str;
    return tcst::t_regexp;
  }

  // Returns the inferred type of `t`, given the expected type `typ`.
  type_val type_check_term(const type_val &typ, const parser::term &t) {
    using namespace parser;
    return std::visit(
      [&](const auto &v) -> type_val {
        using T = std::remove_cvref_t<decltype(v)>;
        if constexpr (std::is_same_v<T, term_var>) {
          if (mem_assoc(v.name)) {
            type_val vtyp = assoc(v.name);
            check_and_propagate(typ, vtyp, &t);
            return assoc(v.name);
          }
          vars_.insert(vars_.begin(), {v.name, typ});
          return typ;
        } else if constexpr (std::is_same_v<T, term_cst>) {
          type_val ctyp = type_val::of_cst(cst_type(v.value));
          check_and_propagate(typ, ctyp, &t);
          return ctyp;
        } else if constexpr (std::is_same_v<T, term_unary>) {
          return type_check_unary(v, typ, t);
        } else {  // term_binary
          return type_check_binary(v, typ, t);
        }
      },
      t.node);
  }

  // Fixed-signature conversions (f2i: arg Float -> Int, etc.).
  type_val fixed_conv(tcst result, tcst arg, const type_val &typ,
                      const parser::term &whole, const parser::term &inner) {
    check_and_propagate(type_val::of_cst(result), typ, &whole);
    type_val t_typ = type_check_term(type_val::of_cst(arg), inner);
    check_and_propagate(type_val::of_cst(arg), t_typ, &inner);
    return type_val::of_cst(result);
  }

  type_val type_check_unary(const parser::term_unary &u, const type_val &typ,
                            const parser::term &whole) {
    using parser::term_unop;
    switch (u.op) {
      case term_unop::f2i: return fixed_conv(tcst::t_int, tcst::t_float, typ, whole, *u.arg);
      case term_unop::i2f: return fixed_conv(tcst::t_float, tcst::t_int, typ, whole, *u.arg);
      case term_unop::i2s: return fixed_conv(tcst::t_str, tcst::t_int, typ, whole, *u.arg);
      case term_unop::s2i: return fixed_conv(tcst::t_int, tcst::t_str, typ, whole, *u.arg);
      case term_unop::f2s: return fixed_conv(tcst::t_str, tcst::t_float, typ, whole, *u.arg);
      case term_unop::s2f: return fixed_conv(tcst::t_float, tcst::t_str, typ, whole, *u.arg);
      case term_unop::format_date: return fixed_conv(tcst::t_str, tcst::t_float, typ, whole, *u.arg);
      case term_unop::year: return fixed_conv(tcst::t_int, tcst::t_float, typ, whole, *u.arg);
      case term_unop::month: return fixed_conv(tcst::t_int, tcst::t_float, typ, whole, *u.arg);
      case term_unop::day_of_month: return fixed_conv(tcst::t_int, tcst::t_float, typ, whole, *u.arg);
      case term_unop::r2s: return fixed_conv(tcst::t_str, tcst::t_regexp, typ, whole, *u.arg);
      case term_unop::s2r: return fixed_conv(tcst::t_regexp, tcst::t_str, typ, whole, *u.arg);
      case term_unop::uminus: {
        type_val exp = new_symbol(tcl::num);
        check_and_propagate(exp, typ, &whole);
        type_val t_typ = type_check_term(exp, *u.arg);
        check_and_propagate(exp, t_typ, u.arg.get());
        return more_spec(t_typ, exp);
      }
    }
    throw type_error{"internal: bad unary op"};
  }

  type_val type_check_binary(const parser::term_binary &b, const type_val &typ,
                             const parser::term &whole) {
    using parser::term_binop;
    if (b.op == term_binop::mod) {
      type_val exp = type_val::of_cst(tcst::t_int);
      check_and_propagate(exp, typ, &whole);
      type_val t1 = type_check_term(exp, *b.l);
      check_and_propagate(exp, t1, b.l.get());
      type_val t2 = type_check_term(exp, *b.r);
      check_and_propagate(exp, t2, b.r.get());
      return exp;
    }
    // Plus/Minus/Mult/Div: numeric, both operands unify to the result type.
    type_val exp = new_symbol(tcl::num);
    check_and_propagate(exp, typ, &whole);
    type_val t1 = type_check_term(exp, *b.l);
    check_and_propagate(exp, t1, b.l.get());
    exp = more_spec(t1, exp);
    type_val t2 = type_check_term(exp, *b.r);
    check_and_propagate(exp, t2, b.r.get());
    exp = more_spec(t2, exp);
    return exp;
  }

  // ---- formulas ----------------------------------------------------------
  void type_check_formula(const parser::formula &f) {
    using namespace parser;
    std::visit(
      [&](const auto &v) {
        using T = std::remove_cvref_t<decltype(v)>;
        if constexpr (std::is_same_v<T, fo_cmp>) {
          if (v.op == cmp_op::substring) {
            type_val s = type_val::of_cst(tcst::t_str);
            type_val t1 = type_check_term(s, v.l);
            check_and_propagate(s, t1, &v.l);
            type_val t2 = type_check_term(s, v.r);
            check_and_propagate(s, t2, &v.r);
            return;
          }
          // Equal / Less / LessEq: both sides unify to a fresh Any.
          type_val exp = new_symbol(tcl::any);
          type_val t1 = type_check_term(exp, v.l);
          check_and_propagate(exp, t1, &v.l);
          exp = more_spec(t1, exp);
          type_val t2 = type_check_term(exp, v.r);
          check_and_propagate(exp, t2, &v.r);
          check_and_propagate(t1, t2, &v.r);
        } else if constexpr (std::is_same_v<T, fo_matches>) {
          type_val s = type_val::of_cst(tcst::t_str);
          type_val t1 = type_check_term(s, v.l);
          check_and_propagate(s, t1, &v.l);
          type_val re = type_val::of_cst(tcst::t_regexp);
          type_val t2 = type_check_term(re, v.r);
          check_and_propagate(re, t2, &v.r);
          for (const auto &o : v.opts)
            if (o) {
              type_val tg = type_check_term(s, *o);
              check_and_propagate(s, tg, &*o);
            }
        } else if constexpr (std::is_same_v<T, fo_pred>) {
          type_check_pred(v);
        } else if constexpr (std::is_same_v<T, fo_neg>) {
          type_check_formula(*v.arg);
        } else if constexpr (std::is_same_v<T, fo_temporal_un>) {
          type_check_formula(*v.body);
        } else if constexpr (std::is_same_v<T, fo_prop>) {
          type_check_formula(*v.l);
          type_check_formula(*v.r);
        } else if constexpr (std::is_same_v<T, fo_temporal_bin>) {
          type_check_formula(*v.l);
          type_check_formula(*v.r);
        } else if constexpr (std::is_same_v<T, fo_quant>) {
          type_check_quant(v.vars, *v.body);
        } else if constexpr (std::is_same_v<T, fo_agg>) {
          type_check_aggreg(v);
        } else if constexpr (std::is_same_v<T, fo_let>) {
          type_check_let(v);
        } else {
          throw type_error{"typing: unsupported construct (regex)"};
        }
      },
      f.node);
  }

  void type_check_pred(const parser::fo_pred &p) {
    pred_key key{p.name, p.args.size()};
    std::vector<type_val> *slot = nullptr;
    for (auto &[k, tys] : sch_)
      if (k == key) {
        slot = &tys;
        break;
      }
    if (!slot)
      throw type_error{"[Rewriting.check_syntax] unknown predicate " + p.name +
                       "/" + std::to_string(p.args.size()) + " in input formula"};
    if (slot->size() != p.args.size())
      throw type_error{"[Rewriting.check_syntax] wrong arity for predicate " +
                       p.name};
    for (std::size_t i = 0; i < p.args.size(); ++i) {
      // Re-read the slot each iteration: propagate may have updated it.
      type_val exp = find_slot(key, i);
      type_val ti = type_check_term(exp, p.args[i]);
      check_and_propagate(exp, ti, &p.args[i]);
    }
  }

  type_val find_slot(const pred_key &key, std::size_t i) {
    for (auto &[k, tys] : sch_)
      if (k == key)
        return tys[i];
    throw type_error{"internal: predicate slot missing"};
  }

  // LET/LETPAST typing (rewriting.ml Let/LetPast): f1 is typed with only the
  // parameters in scope (they are exactly its free variables); the inferred
  // parameter types become the bound predicate's signature, under which f2 is
  // typed. Records the parameter types (keyed by the AST node) for the
  // translator, which needs the bound predicate's concrete column types.
  void type_check_let(const parser::fo_let &l) {
    if (l.kind == parser::let_kind::frz)
      throw type_error{"typing: FRZ is not supported"};
    std::vector<std::string> params;
    for (const auto &a : l.head.args) {
      const auto *var = std::get_if<parser::term_var>(&a.node);
      if (!var)
        throw type_error{"LET parameters must be variables"};
      params.push_back(var->name);
    }
    pred_key key{l.head.name, params.size()};
    const bool past = (l.kind == parser::let_kind::let_past);

    // f1 scope: a fresh Any per parameter, each distinct.
    std::vector<std::pair<std::string, type_val>> pvars;
    for (const auto &p : params) {
      int m = max_symbol_in(sch_, pvars);
      pvars.insert(pvars.begin(), {p, type_val::symb(tcl::any, m + 1)});
    }
    auto param_sig = [&]() {
      std::vector<type_val> s;
      for (const auto &p : params)
        for (const auto &vt : pvars)
          if (vt.first == p) { s.push_back(vt.second); break; }
      return s;
    };

    // Shadow any existing schema entry for the bound predicate.
    std::optional<std::vector<type_val>> shadowed;
    auto sit = std::find_if(sch_.begin(), sch_.end(),
                            [&](auto &e) { return e.first == key; });
    auto set_sig = [&](std::vector<type_val> sig) {
      if (sit != sch_.end())
        sit->second = std::move(sig);
      else {
        sch_.push_back({key, std::move(sig)});
        sit = std::prev(sch_.end());
      }
    };
    if (sit != sch_.end())
      shadowed = sit->second;

    if (past)
      set_sig(param_sig());  // predicate in scope inside f1 (recursive)

    auto outer_vars = vars_;
    vars_ = pvars;
    type_check_formula(*l.bound);

    // Parameter types after f1 (default unresolved to Float, as check_syntax).
    std::vector<tcst> ptypes;
    for (const auto &p : params) {
      type_val t = assoc(p);
      ptypes.push_back(t.is_cst ? t.cst : tcst::t_float);
    }
    let_param_types_[&l] = ptypes;
    std::vector<type_val> concrete;
    for (tcst c : ptypes)
      concrete.push_back(type_val::of_cst(c));
    set_sig(concrete);  // signature for f2 (and, for LETPAST, refines f1's)

    vars_ = outer_vars;
    type_check_formula(*l.body);

    // Restore the schema entry.
    sit = std::find_if(sch_.begin(), sch_.end(),
                       [&](auto &e) { return e.first == key; });
    if (shadowed)
      sit->second = *shadowed;
    else if (sit != sch_.end())
      sch_.erase(sit);
  }

  void type_check_quant(const std::vector<std::string> &bound,
                        const parser::formula &body) {
    // Shadow bound vars with fresh Any; restore afterwards.
    auto is_bound = [&](const std::string &n) {
      return std::find(bound.begin(), bound.end(), n) != bound.end();
    };
    std::vector<std::pair<std::string, type_val>> shadowed, reduced;
    for (const auto &vt : vars_) {
      if (is_bound(vt.first))
        shadowed.push_back(vt);
      else
        reduced.push_back(vt);
    }
    vars_ = reduced;
    for (const auto &b : bound)
      vars_.insert(vars_.begin(), {b, new_symbol(tcl::any)});
    type_check_formula(body);
    // unshadowed (non-bound) results + the original shadowed bindings
    std::vector<std::pair<std::string, type_val>> result;
    for (const auto &vt : vars_)
      if (!is_bound(vt.first))
        result.push_back(vt);
    for (const auto &vt : shadowed)
      result.push_back(vt);
    vars_ = std::move(result);
  }

  void type_check_aggreg(const parser::fo_agg &a) {
    using parser::agg_op;
    // zs = free vars of body not in group-by; shadow them.
    auto body_fv = free_vars(*a.body);
    auto in_gs = [&](const std::string &n) {
      return std::find(a.group_by.begin(), a.group_by.end(), n) !=
             a.group_by.end();
    };
    std::vector<std::string> zs;
    for (const auto &v : body_fv)
      if (!in_gs(v))
        zs.push_back(v);
    auto is_z = [&](const std::string &n) {
      return std::find(zs.begin(), zs.end(), n) != zs.end();
    };
    std::vector<std::pair<std::string, type_val>> shadowed, reduced;
    for (const auto &vt : vars_) {
      if (is_z(vt.first))
        shadowed.push_back(vt);
      else
        reduced.push_back(vt);
    }
    // vars' = reduced + one fresh Any per z. Each fresh symbol must be
    // distinct: compute its index over sch_ + shadowed + accumulator-so-far
    // (matching OCaml's fold with `new_type_symbol TAny sch (shadowed @ vrs)`).
    auto vars_prime = reduced;
    for (const auto &z : zs) {
      int m = max_symbol_in(sch_, shadowed);
      m = std::max(m, max_symbol_in_vars(vars_prime));
      vars_prime.insert(vars_prime.begin(), {z, type_val::symb(tcl::any, m + 1)});
    }

    type_val exp_any = [&]() {
      vars_ = vars_prime;
      return new_symbol(tcl::any);
    }();
    type_val exp_num = [&]() {
      vars_ = vars_prime;
      return new_symbol(tcl::num);
    }();

    type_val exp1, exp2;
    switch (a.op) {
      case agg_op::min:
      case agg_op::max: exp1 = exp_any; exp2 = exp_any; break;
      case agg_op::cnt: exp1 = type_val::of_cst(tcst::t_int); exp2 = exp_any; break;
      case agg_op::sum: exp1 = exp_num; exp2 = exp_num; break;
      case agg_op::avg:
      case agg_op::med: exp1 = type_val::of_cst(tcst::t_float); exp2 = exp_num; break;
    }

    // type_check_aggregation exp1 exp2
    vars_ = vars_prime;
    parser::term x_term{parser::term_var{a.agg_var}};
    type_check_term(exp2, x_term);   // (s1,v1) = check Var x
    type_check_formula(*a.body);     // (s2,v2) = check body
    // x's inferred type in v2 (BEFORE the restore — x is a z-var, shadowed).
    type_val x_typ = assoc(a.agg_var);
    // reduced_vars updated with gs types; restore = shadowed ++ reduced_updated
    std::vector<std::pair<std::string, type_val>> reduced_updated;
    for (const auto &vt : vars_) {
      bool in_reduced = false;
      for (const auto &r : reduced)
        if (r.first == vt.first) { in_reduced = true; break; }
      if (in_reduced)
        reduced_updated.push_back(vt);
    }
    vars_.clear();
    for (const auto &vt : shadowed)
      vars_.push_back(vt);
    for (const auto &vt : reduced_updated)
      vars_.push_back(vt);
    // reintroduce result var r; unify with x's type if the expected types match
    parser::term r_term{parser::term_var{a.res_var}};
    type_check_term(exp1, r_term);   // (s3,v3) = check Var r
    if (exp1 == exp2) {
      type_val r_typ = assoc(a.res_var);
      parser::term rt{parser::term_var{a.res_var}};
      check_and_propagate(x_typ, r_typ, &rt);
    }
  }

  static std::string term_string(const parser::term &t);

  std::map<const parser::fo_let *, std::vector<tcst>> let_param_types_;
  std::vector<std::pair<pred_key, std::vector<type_val>>> sch_;
  std::vector<std::pair<std::string, type_val>> vars_;
};

// Minimal term printer for error messages (MonPoly-ish).
inline std::string type_checker::term_string(const parser::term &t) {
  using namespace parser;
  return std::visit(
    [&](const auto &v) -> std::string {
      using T = std::remove_cvref_t<decltype(v)>;
      if constexpr (std::is_same_v<T, term_var>)
        return v.name;
      else if constexpr (std::is_same_v<T, term_cst>) {
        if (const auto *i = std::get_if<cst_int>(&v.value))
          return i->value.dec;
        if (const auto *s = std::get_if<cst_str>(&v.value))
          return "\"" + s->value + "\"";
        return "<cst>";
      } else if constexpr (std::is_same_v<T, term_unary>)
        return "f(" + term_string(*v.arg) + ")";
      else
        return "(" + term_string(*v.l) + " op " + term_string(*v.r) + ")";
    },
    t.node);
}

}  // namespace staticmon::compile
