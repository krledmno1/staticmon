#pragma once
// Stage 3 (monitorability + well-formedness): a faithful port of
// Rewriting.is_monitorable, check_intervals and check_bounds
// (monpoly-develop/src/rewriting.ml:295-500, 849-975). Run on the desugared
// formula (post elim_syntactic_sugar), matching the explicitmon pipeline's
// gate. This is the *unverified* Rewriting.is_monitorable fragment (the one
// -explicitmon uses); it does NOT include the extra rewriting (rr) monpoly's
// default -check applies before this check, so it can be more conservative
// than `monpoly -check` on formulas that rr makes monitorable. See STATUS.md.

#include <algorithm>
#include <optional>
#include <staticmon/compile/free_vars.h>
#include <staticmon/parser/formula_ast.h>
#include <string>
#include <variant>
#include <vector>

namespace staticmon::compile {

struct monitorability {
  bool ok;
  std::string reason;  // empty iff ok
};

namespace mon_detail {
  using namespace parser;

  inline bool subset(const std::vector<std::string> &a,
                     const std::vector<std::string> &b) {
    for (const auto &x : a)
      if (std::find(b.begin(), b.end(), x) == b.end())
        return false;
    return true;
  }

  inline bool is_cmp(const formula &f) {
    return std::holds_alternative<fo_cmp>(f.node);
  }

  inline bool is_and_relop(const formula &f) {
    if (const auto *c = std::get_if<fo_cmp>(&f.node)) {
      (void) c;
      return true;  // Equal / Less / LessEq / Substring
    }
    if (std::holds_alternative<fo_matches>(f.node))
      return true;
    if (const auto *n = std::get_if<fo_neg>(&f.node)) {
      const auto &i = n->arg->node;
      return std::holds_alternative<fo_cmp>(i) ||
             std::holds_alternative<fo_matches>(i);
    }
    return false;
  }

  inline bool is_special_case(const std::vector<std::string> &fv1,
                              const formula &f) {
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
      return subset(free_vars(f), fv1);  // Less / LessEq / Substring
    }
    if (const auto *m = std::get_if<fo_matches>(&f.node)) {
      if (!subset(term_vars(m->l), fv1) || !subset(term_vars(m->r), fv1))
        return false;
      for (const auto &o : m->opts) {
        if (!o || std::holds_alternative<term_var>(o->node))
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

  // Reason strings (abbreviated from rewriting.ml msg_*).
  constexpr const char *MSG_EQUAL =
    "Only equalities of the form t1 = t2 where at least one of t1, t2 is a "
    "variable and the other is a constant are monitorable";
  constexpr const char *MSG_LESS = "Comparisons are not monitorable alone";
  constexpr const char *MSG_NOT_EQUAL = "Only x != x style inequalities are "
                                        "monitorable";
  constexpr const char *MSG_PRED = "Predicate arguments must be variables or "
                                   "constants";
  constexpr const char *MSG_NOT = "NOT psi must have no free variables (except "
                                  "guarded by AND / SINCE / UNTIL)";
  constexpr const char *MSG_ANDRELOP = "The right conjunct's free variables "
                                       "are not restricted by the left";
  constexpr const char *MSG_SUBSET = "The subformula's free variables are not "
                                     "a subset of the guard's";
  constexpr const char *MSG_OR = "Both disjuncts must have the same free "
                                 "variables";

  monitorability is_monitorable(const formula &f);

  inline monitorability ok() { return {true, ""}; }
  inline monitorability no(const char *m) { return {false, m}; }

  inline monitorability is_monitorable(const formula &f) {
    return std::visit(
      [&](const auto &v) -> monitorability {
        using T = std::remove_cvref_t<decltype(v)>;
        if constexpr (std::is_same_v<T, fo_cmp>) {
          if (v.op == cmp_op::equal) {
            bool lv = std::holds_alternative<term_var>(v.l.node);
            bool lc = std::holds_alternative<term_cst>(v.l.node);
            bool rv = std::holds_alternative<term_var>(v.r.node);
            bool rc = std::holds_alternative<term_cst>(v.r.node);
            if ((lv && rc) || (lc && rv) || (lc && rc))
              return ok();
            return no(MSG_EQUAL);
          }
          return no(MSG_LESS);  // Less / LessEq / Substring
        } else if constexpr (std::is_same_v<T, fo_matches>) {
          return no(MSG_LESS);
        } else if constexpr (std::is_same_v<T, fo_neg>) {
          // Neg(Equal ..) special cases, else general Neg.
          if (const auto *c = std::get_if<fo_cmp>(&v.arg->node);
              c && c->op == cmp_op::equal) {
            const auto *x = std::get_if<term_var>(&c->l.node);
            const auto *y = std::get_if<term_var>(&c->r.node);
            bool lc = std::holds_alternative<term_cst>(c->l.node);
            bool rc = std::holds_alternative<term_cst>(c->r.node);
            if (x && y && x->name == y->name)
              return ok();
            if (lc && rc)
              return ok();
            return no(MSG_NOT_EQUAL);
          }
          if (free_vars(*v.arg).empty())
            return is_monitorable(*v.arg);
          return no(MSG_NOT);
        } else if constexpr (std::is_same_v<T, fo_pred>) {
          for (const auto &a : v.args) {
            if (!std::holds_alternative<term_var>(a.node) &&
                !std::holds_alternative<term_cst>(a.node))
              return no(MSG_PRED);
          }
          return ok();
        } else if constexpr (std::is_same_v<T, fo_prop>) {
          if (v.op == prop_binop::and_) {
            auto m1 = is_monitorable(*v.l);
            if (!m1.ok)
              return m1;
            auto fv1 = free_vars(*v.l);
            const auto &f2 = *v.r;
            if (is_and_relop(f2)) {
              if (is_special_case(fv1, f2))
                return ok();
              return no(MSG_ANDRELOP);
            }
            if (const auto *n = std::get_if<fo_neg>(&f2.node)) {
              auto fv2 = free_vars(f2);
              if (!subset(fv2, fv1))
                return no(MSG_SUBSET);
              return is_monitorable(*n->arg);
            }
            return is_monitorable(f2);
          }
          if (v.op == prop_binop::or_) {
            auto fv1 = free_vars(*v.l);
            auto fv2 = free_vars(*v.r);
            if (!subset(fv1, fv2) || !subset(fv2, fv1))
              return no(MSG_OR);
            auto m1 = is_monitorable(*v.l);
            if (!m1.ok)
              return m1;
            return is_monitorable(*v.r);
          }
          // Implies/Equiv should have been desugared.
          return no("non-desugared propositional operator");
        } else if constexpr (std::is_same_v<T, fo_quant>) {
          if (v.universal)
            return no("FORALL should have been desugared");
          return is_monitorable(*v.body);
        } else if constexpr (std::is_same_v<T, fo_agg>) {
          return is_monitorable(*v.body);
        } else if constexpr (std::is_same_v<T, fo_temporal_un>) {
          using K = temporal_unop;
          if (v.op == K::prev || v.op == K::next || v.op == K::eventually ||
              v.op == K::once)
            return is_monitorable(*v.body);
          return no("ALWAYS/PAST_ALWAYS should have been desugared");
        } else if constexpr (std::is_same_v<T, fo_temporal_bin>) {
          // Since / Until (Trigger/Release not in the explicitmon fragment).
          if (v.op != temporal_binop::since && v.op != temporal_binop::until)
            return no("TRIGGER/RELEASE not monitorable (unverified)");
          auto m2 = is_monitorable(*v.r);
          if (!m2.ok)
            return m2;
          auto fv1 = free_vars(*v.l);
          auto fv2 = free_vars(*v.r);
          if (!subset(fv1, fv2))
            return no(MSG_SUBSET);
          const formula *f1p = &*v.l;
          if (const auto *n = std::get_if<fo_neg>(&v.l->node))
            f1p = &*n->arg;
          return is_monitorable(*f1p);
        } else if constexpr (std::is_same_v<T, fo_let>) {
          auto m1 = is_monitorable(*v.bound);
          if (!m1.ok)
            return m1;
          return is_monitorable(*v.body);
        } else {
          return no("regex not monitorable (unverified)");
        }
      },
      f.node);
  }

  // ---- well-formedness: intervals and bounded future --------------------
  inline bool bi_geq_zero(const big_int &b) { return b.dec.empty() || b.dec[0] != '-'; }

  inline bool bi_less(const big_int &a, const big_int &b) {
    bool an = !a.dec.empty() && a.dec[0] == '-';
    bool bn = !b.dec.empty() && b.dec[0] == '-';
    if (an != bn)
      return an;  // negative < non-negative
    // same sign: compare magnitudes
    std::string am = an ? a.dec.substr(1) : a.dec;
    std::string bm = bn ? b.dec.substr(1) : b.dec;
    bool mag_less;
    if (am.size() != bm.size())
      mag_less = am.size() < bm.size();
    else
      mag_less = am < bm;
    bool mag_eq = (am == bm);
    if (an)  // both negative: larger magnitude is smaller
      return !mag_less && !mag_eq;
    return mag_less;
  }
  inline bool bi_leq(const big_int &a, const big_int &b) {
    return !bi_less(b, a);
  }

  inline bool check_interval(const interval &intv) {
    auto bound_ok = [](const interval_bound &b) {
      if (const auto *o = std::get_if<bnd_open>(&b))
        return bi_geq_zero(o->value);
      if (const auto *c = std::get_if<bnd_closed>(&b))
        return bi_geq_zero(c->value);
      return true;  // inf
    };
    if (!bound_ok(intv.lower) || !bound_ok(intv.upper))
      return false;
    // check_lb_ub
    if (std::holds_alternative<bnd_inf>(intv.lower))
      return false;
    if (std::holds_alternative<bnd_inf>(intv.upper))
      return true;  // lower is finite (checked)
    const big_int &a = std::holds_alternative<bnd_closed>(intv.lower)
                         ? std::get<bnd_closed>(intv.lower).value
                         : std::get<bnd_open>(intv.lower).value;
    const big_int &b = std::holds_alternative<bnd_closed>(intv.upper)
                         ? std::get<bnd_closed>(intv.upper).value
                         : std::get<bnd_open>(intv.upper).value;
    bool both_closed = std::holds_alternative<bnd_closed>(intv.lower) &&
                       std::holds_alternative<bnd_closed>(intv.upper);
    return both_closed ? bi_leq(a, b) : bi_less(a, b);
  }

  inline bool check_intervals(const formula &f) {
    return std::visit(
      [&](const auto &v) -> bool {
        using T = std::remove_cvref_t<decltype(v)>;
        if constexpr (std::is_same_v<T, fo_cmp> ||
                      std::is_same_v<T, fo_matches> ||
                      std::is_same_v<T, fo_pred>)
          return true;
        else if constexpr (std::is_same_v<T, fo_neg>)
          return check_intervals(*v.arg);
        else if constexpr (std::is_same_v<T, fo_quant>)
          return check_intervals(*v.body);
        else if constexpr (std::is_same_v<T, fo_agg>)
          return check_intervals(*v.body);
        else if constexpr (std::is_same_v<T, fo_prop>)
          return check_intervals(*v.l) && check_intervals(*v.r);
        else if constexpr (std::is_same_v<T, fo_temporal_un>)
          return check_interval(v.intv) && check_intervals(*v.body);
        else if constexpr (std::is_same_v<T, fo_temporal_bin>)
          return check_interval(v.intv) && check_intervals(*v.l) &&
                 check_intervals(*v.r);
        else if constexpr (std::is_same_v<T, fo_let>)
          return check_intervals(*v.bound) && check_intervals(*v.body);
        else
          return true;  // regex: not in fragment
      },
      f.node);
  }

  inline bool check_bounds(const formula &f) {
    auto ub_finite = [](const interval &i) {
      return !std::holds_alternative<bnd_inf>(i.upper);
    };
    return std::visit(
      [&](const auto &v) -> bool {
        using T = std::remove_cvref_t<decltype(v)>;
        if constexpr (std::is_same_v<T, fo_cmp> ||
                      std::is_same_v<T, fo_matches> ||
                      std::is_same_v<T, fo_pred>)
          return true;
        else if constexpr (std::is_same_v<T, fo_neg>)
          return check_bounds(*v.arg);
        else if constexpr (std::is_same_v<T, fo_quant>)
          return check_bounds(*v.body);
        else if constexpr (std::is_same_v<T, fo_agg>)
          return check_bounds(*v.body);
        else if constexpr (std::is_same_v<T, fo_prop>)
          return check_bounds(*v.l) && check_bounds(*v.r);
        else if constexpr (std::is_same_v<T, fo_temporal_un>) {
          // Prev/Next/Once/PastAlways: unbounded ok. Eventually/Always:
          // upper must be finite (bounded future). (Post-desugar Always is
          // gone; Eventually remains.)
          if (v.op == temporal_unop::eventually ||
              v.op == temporal_unop::always)
            return ub_finite(v.intv) && check_bounds(*v.body);
          return check_bounds(*v.body);
        } else if constexpr (std::is_same_v<T, fo_temporal_bin>) {
          if (v.op == temporal_binop::until || v.op == temporal_binop::release)
            return ub_finite(v.intv) && check_bounds(*v.l) &&
                   check_bounds(*v.r);
          return check_bounds(*v.l) && check_bounds(*v.r);
        } else if constexpr (std::is_same_v<T, fo_let>) {
          return check_bounds(*v.bound) && check_bounds(*v.body);
        } else {
          return true;
        }
      },
      f.node);
  }

  // check_let (rewriting.ml): LET/LETPAST parameters must be distinct
  // variables and coincide exactly with the free variables of the definition.
  inline std::optional<std::string> check_let(const formula &f) {
    if (const auto *l = std::get_if<fo_let>(&f.node)) {
      std::vector<std::string> params;
      for (const auto &a : l->head.args) {
        if (!std::holds_alternative<term_var>(a.node))
          return "[Rewriting.check_let] LET parameters must be variables";
        params.push_back(std::get<term_var>(a.node).name);
      }
      auto fv1 = free_vars(*l->bound);
      if (fv1.size() != params.size() || !subset(fv1, params) ||
          !subset(params, fv1))
        return "[Rewriting.check_let] LET parameters must coincide with the "
               "free variables of the definition";
      if (auto e = check_let(*l->bound))
        return e;
      return check_let(*l->body);
    }
    // recurse structurally
    return std::visit(
      [&](const auto &v) -> std::optional<std::string> {
        using T = std::remove_cvref_t<decltype(v)>;
        if constexpr (std::is_same_v<T, fo_neg>)
          return check_let(*v.arg);
        else if constexpr (std::is_same_v<T, fo_quant> ||
                           std::is_same_v<T, fo_agg>)
          return check_let(*v.body);
        else if constexpr (std::is_same_v<T, fo_temporal_un>)
          return check_let(*v.body);
        else if constexpr (std::is_same_v<T, fo_prop> ||
                           std::is_same_v<T, fo_temporal_bin>) {
          if (auto e = check_let(*v.l))
            return e;
          return check_let(*v.r);
        } else {
          return std::nullopt;
        }
      },
      f.node);
  }
}  // namespace mon_detail

using mon_detail::check_bounds;
using mon_detail::check_interval;
using mon_detail::check_intervals;
using mon_detail::check_let;

inline monitorability is_monitorable(const parser::formula &f) {
  return mon_detail::is_monitorable(f);
}

inline std::optional<std::string> check_wff(const parser::formula &f) {
  if (auto e = check_let(f))
    return e;
  if (!check_intervals(f))
    return "[Rewriting.check_wff] The formula contains a negative or empty "
           "interval";
  if (!check_bounds(f))
    return "[Rewriting.check_wff] The formula contains an unbounded future "
           "temporal operator. It is hence not monitorable.";
  return std::nullopt;
}

}  // namespace staticmon::compile
