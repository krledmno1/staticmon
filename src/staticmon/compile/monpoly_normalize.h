#pragma once
// Reproduces MonPoly's formula normalization (src/rewriting.ml) over
// parser::formula, so that `staticmon-headers -verbose` prints the same
// "analyzed formula" as `monpoly -verified -verbose`. MonPoly analyses the
// normalized formula rf = normalize_negation(normalize_syntax(f)):
//   normalize_syntax f  = elim_syntactic_sugar (simplify_terms f)
//   normalize_negation f = elim_double_negation (push_negation f)
// The `-verified` branches (Misc.verified = true) are taken, since staticmon's
// oracle is `monpoly -verified`. When rf differs from the parsed f, MonPoly also
// prints the original as "The input formula is:".

#include <algorithm>
#include <optional>
#include <staticmon/compile/free_vars.h>
#include <staticmon/compile/monitorable.h>
#include <staticmon/parser/formula_ast.h>
#include <string>
#include <variant>
#include <vector>

namespace staticmon::compile {

namespace norm_detail {
using namespace staticmon::parser;

// ---- deep clone (the AST holds unique_ptrs) ---------------------------
inline term clone_term(const term &t);
inline formula clone_formula(const formula &f);
inline regex_ptr clone_regex(const regex &r);

inline term clone_term(const term &t) {
  return std::visit(
    [](const auto &v) -> term {
      using T = std::remove_cvref_t<decltype(v)>;
      if constexpr (std::is_same_v<T, term_var> || std::is_same_v<T, term_cst>)
        return term{v};
      else if constexpr (std::is_same_v<T, term_unary>)
        return term{term_unary{v.op, mk_term(clone_term(*v.arg))}};
      else
        return term{
          term_binary{v.op, mk_term(clone_term(*v.l)), mk_term(clone_term(*v.r))}};
    },
    t.node);
}

inline fo_pred clone_pred(const fo_pred &p) {
  fo_pred r{p.name, {}};
  for (const auto &a : p.args)
    r.args.push_back(clone_term(a));
  return r;
}

inline regex_ptr clone_regex(const regex &r) {
  return std::visit(
    [](const auto &v) -> regex_ptr {
      using T = std::remove_cvref_t<decltype(v)>;
      if constexpr (std::is_same_v<T, re_skip>)
        return mk_regex(regex{re_skip{v.n}});
      else if constexpr (std::is_same_v<T, re_test>)
        return mk_regex(regex{re_test{mk_formula(clone_formula(*v.f))}});
      else if constexpr (std::is_same_v<T, re_concat>)
        return mk_regex(regex{re_concat{clone_regex(*v.l), clone_regex(*v.r)}});
      else if constexpr (std::is_same_v<T, re_plus>)
        return mk_regex(regex{re_plus{clone_regex(*v.l), clone_regex(*v.r)}});
      else
        return mk_regex(regex{re_star{clone_regex(*v.arg)}});
    },
    r.node);
}

inline formula clone_formula(const formula &f) {
  return std::visit(
    [](const auto &h) -> formula {
      using T = std::remove_cvref_t<decltype(h)>;
      if constexpr (std::is_same_v<T, fo_pred>) {
        return formula{clone_pred(h)};
      } else if constexpr (std::is_same_v<T, fo_cmp>) {
        return formula{fo_cmp{h.op, clone_term(h.l), clone_term(h.r)}};
      } else if constexpr (std::is_same_v<T, fo_matches>) {
        fo_matches m{clone_term(h.l), clone_term(h.r), {}};
        for (const auto &o : h.opts)
          m.opts.push_back(o ? std::optional<term>(clone_term(*o))
                             : std::nullopt);
        return formula{std::move(m)};
      } else if constexpr (std::is_same_v<T, fo_let>) {
        return formula{fo_let{h.kind, clone_pred(h.head),
                              mk_formula(clone_formula(*h.bound)),
                              mk_formula(clone_formula(*h.body))}};
      } else if constexpr (std::is_same_v<T, fo_neg>) {
        return formula{fo_neg{mk_formula(clone_formula(*h.arg))}};
      } else if constexpr (std::is_same_v<T, fo_prop>) {
        return formula{fo_prop{h.op, mk_formula(clone_formula(*h.l)),
                               mk_formula(clone_formula(*h.r))}};
      } else if constexpr (std::is_same_v<T, fo_quant>) {
        return formula{
          fo_quant{h.universal, h.vars, mk_formula(clone_formula(*h.body))}};
      } else if constexpr (std::is_same_v<T, fo_agg>) {
        return formula{fo_agg{h.res_var, h.op, h.agg_var, h.group_by,
                              mk_formula(clone_formula(*h.body))}};
      } else if constexpr (std::is_same_v<T, fo_temporal_un>) {
        return formula{fo_temporal_un{h.op, h.intv,
                                      mk_formula(clone_formula(*h.body))}};
      } else if constexpr (std::is_same_v<T, fo_temporal_bin>) {
        return formula{fo_temporal_bin{h.op, h.intv,
                                       mk_formula(clone_formula(*h.l)),
                                       mk_formula(clone_formula(*h.r))}};
      } else {  // fo_regex
        return formula{fo_regex{h.future, h.intv, clone_regex(*h.re)}};
      }
    },
    f.node);
}

// ---- small builders ---------------------------------------------------
inline formula mk_neg(formula f) { return formula{fo_neg{mk_formula(std::move(f))}}; }
inline formula mk_prop(prop_binop op, formula l, formula r) {
  return formula{fo_prop{op, mk_formula(std::move(l)), mk_formula(std::move(r))}};
}
inline formula mk_tbin(temporal_binop op, const interval &iv, formula l,
                       formula r) {
  return formula{
    fo_temporal_bin{op, iv, mk_formula(std::move(l)), mk_formula(std::move(r))}};
}
inline formula mk_tun(temporal_unop op, const interval &iv, formula b) {
  return formula{fo_temporal_un{op, iv, mk_formula(std::move(b))}};
}
// MonPoly's verified sugar for ALWAYS/PAST_ALWAYS uses TRUE = NOT ("x" = "x").
inline formula mk_true_sentinel() {
  return mk_neg(formula{fo_cmp{cmp_op::equal, term{term_cst{cst_str{"x"}}},
                               term{term_cst{cst_str{"x"}}}}});
}

// ---- simplify_terms: fold ground (variable-free) subterms -------------
inline bool parse_i64(const std::string &s, long long &out) {
  try {
    std::size_t pos = 0;
    out = std::stoll(s, &pos);
    return pos == s.size();
  } catch (...) {
    return false;  // overflow / not an int -> leave unfolded
  }
}

inline bool term_is_ground(const term &t) {
  return std::visit(
    [](const auto &v) -> bool {
      using T = std::remove_cvref_t<decltype(v)>;
      if constexpr (std::is_same_v<T, term_var>)
        return false;
      else if constexpr (std::is_same_v<T, term_cst>)
        return true;
      else if constexpr (std::is_same_v<T, term_unary>)
        return term_is_ground(*v.arg);
      else
        return term_is_ground(*v.l) && term_is_ground(*v.r);
    },
    t.node);
}

// Evaluate a ground term to a constant when it is a well-typed int/float
// arithmetic tree (the cases eval_gterm handles without conversions). Returns
// nullopt otherwise, leaving the term as monpoly would after a type error would
// have been rejected earlier. Conversions (f2i/…) and date ops are left folded
// only when trivial; they are vanishingly rare in ground position.
inline std::optional<constant> eval_ground(const term &t);

inline std::optional<constant> eval_ground(const term &t) {
  return std::visit(
    [&](const auto &v) -> std::optional<constant> {
      using T = std::remove_cvref_t<decltype(v)>;
      if constexpr (std::is_same_v<T, term_cst>) {
        return v.value;
      } else if constexpr (std::is_same_v<T, term_unary>) {
        auto a = eval_ground(*v.arg);
        if (!a)
          return std::nullopt;
        if (v.op == term_unop::uminus) {
          if (auto *i = std::get_if<cst_int>(&*a))
            return constant{cst_int{big_int::normalize("-" + i->value.dec)}};
          if (auto *f = std::get_if<cst_float>(&*a))
            return constant{cst_float{-f->value}};
        }
        return std::nullopt;  // conversions: leave unfolded
      } else if constexpr (std::is_same_v<T, term_binary>) {
        auto a = eval_ground(*v.l), b = eval_ground(*v.r);
        if (!a || !b)
          return std::nullopt;
        // Float op Float (monpoly requires matching types).
        auto *fx = std::get_if<cst_float>(&*a);
        auto *fy = std::get_if<cst_float>(&*b);
        if (fx && fy) {
          double x = fx->value, y = fy->value, z = 0;
          switch (v.op) {
            case term_binop::plus: z = x + y; break;
            case term_binop::minus: z = x - y; break;
            case term_binop::mult: z = x * y; break;
            case term_binop::div: z = x / y; break;
            case term_binop::mod: return std::nullopt;  // mod: ints only
          }
          return constant{cst_float{z}};
        }
        // Int op Int. zarith / and mod truncate toward zero, matching C++
        // int64. Fold when both fit in int64 (all realistic formulas); leave
        // huge/overflowing values unfolded rather than mis-fold.
        auto *ix = std::get_if<cst_int>(&*a);
        auto *iy = std::get_if<cst_int>(&*b);
        if (ix && iy) {
          long long x, y;
          if (!parse_i64(ix->value.dec, x) || !parse_i64(iy->value.dec, y))
            return std::nullopt;
          long long z = 0;
          switch (v.op) {
            case term_binop::plus:
              if (__builtin_add_overflow(x, y, &z)) return std::nullopt;
              break;
            case term_binop::minus:
              if (__builtin_sub_overflow(x, y, &z)) return std::nullopt;
              break;
            case term_binop::mult:
              if (__builtin_mul_overflow(x, y, &z)) return std::nullopt;
              break;
            case term_binop::div:
              if (y == 0) return std::nullopt;
              z = x / y;
              break;
            case term_binop::mod:
              if (y == 0) return std::nullopt;
              z = x % y;
              break;
          }
          return constant{cst_int{big_int::normalize(std::to_string(z))}};
        }
        return std::nullopt;
      } else {
        return std::nullopt;
      }
    },
    t.node);
}

inline term simplify_term(const term &t) {
  if (term_is_ground(t))
    if (auto c = eval_ground(t))
      return term{term_cst{*c}};
  // recurse into non-ground compound terms so ground subterms fold
  return std::visit(
    [&](const auto &v) -> term {
      using T = std::remove_cvref_t<decltype(v)>;
      if constexpr (std::is_same_v<T, term_var> || std::is_same_v<T, term_cst>)
        return clone_term(t);
      else if constexpr (std::is_same_v<T, term_unary>)
        return term{term_unary{v.op, mk_term(simplify_term(*v.arg))}};
      else
        return term{term_binary{v.op, mk_term(simplify_term(*v.l)),
                                mk_term(simplify_term(*v.r))}};
    },
    t.node);
}

inline regex_ptr map_regex(const regex &r,
                           formula (*mapf)(const formula &));

inline formula simplify_terms(const formula &f);

inline formula simplify_terms(const formula &f) {
  return std::visit(
    [&](const auto &h) -> formula {
      using T = std::remove_cvref_t<decltype(h)>;
      if constexpr (std::is_same_v<T, fo_pred>) {
        fo_pred p{h.name, {}};
        for (const auto &a : h.args)
          p.args.push_back(simplify_term(a));
        return formula{std::move(p)};
      } else if constexpr (std::is_same_v<T, fo_cmp>) {
        return formula{fo_cmp{h.op, simplify_term(h.l), simplify_term(h.r)}};
      } else if constexpr (std::is_same_v<T, fo_matches>) {
        fo_matches m{simplify_term(h.l), simplify_term(h.r), {}};
        for (const auto &o : h.opts)
          m.opts.push_back(o ? std::optional<term>(simplify_term(*o))
                             : std::nullopt);
        return formula{std::move(m)};
      } else if constexpr (std::is_same_v<T, fo_let>) {
        return formula{fo_let{h.kind, clone_pred(h.head),
                              mk_formula(simplify_terms(*h.bound)),
                              mk_formula(simplify_terms(*h.body))}};
      } else if constexpr (std::is_same_v<T, fo_neg>) {
        return mk_neg(simplify_terms(*h.arg));
      } else if constexpr (std::is_same_v<T, fo_prop>) {
        return mk_prop(h.op, simplify_terms(*h.l), simplify_terms(*h.r));
      } else if constexpr (std::is_same_v<T, fo_quant>) {
        return formula{
          fo_quant{h.universal, h.vars, mk_formula(simplify_terms(*h.body))}};
      } else if constexpr (std::is_same_v<T, fo_agg>) {
        return formula{fo_agg{h.res_var, h.op, h.agg_var, h.group_by,
                              mk_formula(simplify_terms(*h.body))}};
      } else if constexpr (std::is_same_v<T, fo_temporal_un>) {
        return mk_tun(h.op, h.intv, simplify_terms(*h.body));
      } else if constexpr (std::is_same_v<T, fo_temporal_bin>) {
        return mk_tbin(h.op, h.intv, simplify_terms(*h.l), simplify_terms(*h.r));
      } else {  // fo_regex
        return formula{fo_regex{h.future, h.intv, map_regex(*h.re, +[](const formula &g) { return simplify_terms(g); })}};
      }
    },
    f.node);
}

inline regex_ptr map_regex(const regex &r, formula (*mapf)(const formula &)) {
  return std::visit(
    [&](const auto &v) -> regex_ptr {
      using T = std::remove_cvref_t<decltype(v)>;
      if constexpr (std::is_same_v<T, re_skip>)
        return mk_regex(regex{re_skip{v.n}});
      else if constexpr (std::is_same_v<T, re_test>)
        return mk_regex(regex{re_test{mk_formula(mapf(*v.f))}});
      else if constexpr (std::is_same_v<T, re_concat>)
        return mk_regex(regex{re_concat{map_regex(*v.l, mapf), map_regex(*v.r, mapf)}});
      else if constexpr (std::is_same_v<T, re_plus>)
        return mk_regex(regex{re_plus{map_regex(*v.l, mapf), map_regex(*v.r, mapf)}});
      else
        return mk_regex(regex{re_star{map_regex(*v.arg, mapf)}});
    },
    r.node);
}

// ---- elim_syntactic_sugar (rewriting.ml:156) --------------------------
inline formula elim_sugar(const formula &f);

inline formula elim_sugar(const formula &f) {
  return std::visit(
    [&](const auto &h) -> formula {
      using T = std::remove_cvref_t<decltype(h)>;
      if constexpr (std::is_same_v<T, fo_cmp> || std::is_same_v<T, fo_pred> ||
                    std::is_same_v<T, fo_matches>) {
        return clone_formula(f);
      } else if constexpr (std::is_same_v<T, fo_let>) {
        return formula{fo_let{h.kind, clone_pred(h.head),
                              mk_formula(elim_sugar(*h.bound)),
                              mk_formula(elim_sugar(*h.body))}};
      } else if constexpr (std::is_same_v<T, fo_neg>) {
        return mk_neg(elim_sugar(*h.arg));
      } else if constexpr (std::is_same_v<T, fo_quant>) {
        if (!h.universal) {
          // Exists: drop bound vars not free in the elaborated body.
          formula nf = elim_sugar(*h.body);
          auto fv = free_vars(nf);
          std::vector<std::string> keep;
          for (const auto &v : h.vars)
            if (std::find(fv.begin(), fv.end(), v) != fv.end())
              keep.push_back(v);
          if (keep.empty())
            return nf;
          return formula{fo_quant{false, std::move(keep), mk_formula(std::move(nf))}};
        }
        // ForAll (v,f) -> elim (Neg (Exists (v, Neg f)))
        formula inner =
          formula{fo_quant{false, h.vars,
                           mk_formula(mk_neg(clone_formula(*h.body)))}};
        return elim_sugar(mk_neg(std::move(inner)));
      } else if constexpr (std::is_same_v<T, fo_agg>) {
        return formula{fo_agg{h.res_var, h.op, h.agg_var, h.group_by,
                              mk_formula(elim_sugar(*h.body))}};
      } else if constexpr (std::is_same_v<T, fo_prop>) {
        if (h.op == prop_binop::and_)
          return mk_prop(prop_binop::and_, elim_sugar(*h.l), elim_sugar(*h.r));
        if (h.op == prop_binop::or_)
          return mk_prop(prop_binop::or_, elim_sugar(*h.l), elim_sugar(*h.r));
        if (h.op == prop_binop::implies)
          // Implies (f1,f2) -> Or (elim (Neg f1), elim f2)
          return mk_prop(prop_binop::or_, elim_sugar(mk_neg(clone_formula(*h.l))),
                         elim_sugar(*h.r));
        // Equiv (f1,f2) -> And (Or (elim (Neg f1), elim f2),
        //                       Or (elim f1, elim (Neg f2)))
        return mk_prop(
          prop_binop::and_,
          mk_prop(prop_binop::or_, elim_sugar(mk_neg(clone_formula(*h.l))),
                  elim_sugar(*h.r)),
          mk_prop(prop_binop::or_, elim_sugar(*h.l),
                  elim_sugar(mk_neg(clone_formula(*h.r)))));
      } else if constexpr (std::is_same_v<T, fo_temporal_un>) {
        if (h.op == temporal_unop::always)
          // verified: Always -> Release (TRUE, elim f)
          return mk_tbin(temporal_binop::release, h.intv, mk_true_sentinel(),
                         elim_sugar(*h.body));
        if (h.op == temporal_unop::past_always)
          // verified: PastAlways -> Trigger (TRUE, elim f)
          return mk_tbin(temporal_binop::trigger, h.intv, mk_true_sentinel(),
                         elim_sugar(*h.body));
        return mk_tun(h.op, h.intv, elim_sugar(*h.body));
      } else if constexpr (std::is_same_v<T, fo_temporal_bin>) {
        return mk_tbin(h.op, h.intv, elim_sugar(*h.l), elim_sugar(*h.r));
      } else {  // fo_regex
        return formula{fo_regex{h.future, h.intv, map_regex(*h.re, +[](const formula &g) { return elim_sugar(g); })}};
      }
    },
    f.node);
}

// ---- push_negation (rewriting.ml:214), verified branches --------------
inline formula push_neg(const formula &f);

// push(Neg inner) with inner already destructured
inline formula push_of_neg(const formula &inner) {
  return std::visit(
    [&](const auto &g) -> formula {
      using T = std::remove_cvref_t<decltype(g)>;
      if constexpr (std::is_same_v<T, fo_prop>) {
        if (g.op == prop_binop::and_)  // Neg(And) -> Or(push(Neg),push(Neg))
          return mk_prop(prop_binop::or_, push_of_neg(*g.l), push_of_neg(*g.r));
        if (g.op == prop_binop::or_)  // Neg(Or) -> And(push(Neg),push(Neg))
          return mk_prop(prop_binop::and_, push_of_neg(*g.l), push_of_neg(*g.r));
        // Implies/Equiv are gone after elim_sugar; fall back to Neg(push).
        return mk_neg(push_neg(formula{fo_prop{g.op, mk_formula(clone_formula(*g.l)),
                                               mk_formula(clone_formula(*g.r))}}));
      } else if constexpr (std::is_same_v<T, fo_temporal_un>) {
        switch (g.op) {
          case temporal_unop::eventually:  // Neg(Ev(f)) -> Neg(Ev(push f))
            return mk_neg(mk_tun(temporal_unop::eventually, g.intv, push_neg(*g.body)));
          case temporal_unop::once:  // Neg(Once(f)) -> Neg(Once(push f))
            return mk_neg(mk_tun(temporal_unop::once, g.intv, push_neg(*g.body)));
          case temporal_unop::always:  // Neg(Always(f)) -> Ev(push(Neg f))
            return mk_tun(temporal_unop::eventually, g.intv, push_of_neg(*g.body));
          case temporal_unop::past_always:  // Neg(PastAlways) -> Once(push(Neg f))
            return mk_tun(temporal_unop::once, g.intv, push_of_neg(*g.body));
          default:  // Prev/Next: Neg f -> Neg(push f)
            return mk_neg(push_neg(formula{clone_formula(inner)}));
        }
      } else if constexpr (std::is_same_v<T, fo_temporal_bin>) {
        switch (g.op) {
          case temporal_binop::since:  // verified: Neg(Since) -> Trigger(push(Neg),push(Neg))
            return mk_tbin(temporal_binop::trigger, g.intv, push_of_neg(*g.l), push_of_neg(*g.r));
          case temporal_binop::until:  // verified: Neg(Until) -> Release(push(Neg),push(Neg))
            return mk_tbin(temporal_binop::release, g.intv, push_of_neg(*g.l), push_of_neg(*g.r));
          case temporal_binop::trigger:  // Neg(Trigger) -> Since(push(Neg),push(Neg))
            return mk_tbin(temporal_binop::since, g.intv, push_of_neg(*g.l), push_of_neg(*g.r));
          case temporal_binop::release:  // Neg(Release) -> Until(push(Neg),push(Neg))
            return mk_tbin(temporal_binop::until, g.intv, push_of_neg(*g.l), push_of_neg(*g.r));
        }
        return mk_neg(push_neg(formula{clone_formula(inner)}));
      } else if constexpr (std::is_same_v<T, fo_let>) {
        // Neg(Let(p,f1,f2)) -> Let(p,f1, push(Neg f2))
        return formula{fo_let{g.kind, clone_pred(g.head),
                              mk_formula(clone_formula(*g.bound)),
                              mk_formula(push_of_neg(*g.body))}};
      } else {
        // Neg f -> Neg (push f)   (Pred/cmp/Exists/Aggreg/Neg/… )
        return mk_neg(push_neg(inner));
      }
    },
    inner.node);
}

inline formula push_neg(const formula &f) {
  return std::visit(
    [&](const auto &h) -> formula {
      using T = std::remove_cvref_t<decltype(h)>;
      if constexpr (std::is_same_v<T, fo_neg>) {
        return push_of_neg(*h.arg);
      } else if constexpr (std::is_same_v<T, fo_let>) {
        // Let(p,f1,f2) -> Let(p,f1, push f2)   (bound not pushed)
        return formula{fo_let{h.kind, clone_pred(h.head),
                              mk_formula(clone_formula(*h.bound)),
                              mk_formula(push_neg(*h.body))}};
      } else if constexpr (std::is_same_v<T, fo_cmp> ||
                           std::is_same_v<T, fo_pred> ||
                           std::is_same_v<T, fo_matches>) {
        return clone_formula(f);
      } else if constexpr (std::is_same_v<T, fo_prop>) {
        return mk_prop(h.op, push_neg(*h.l), push_neg(*h.r));
      } else if constexpr (std::is_same_v<T, fo_quant>) {
        return formula{
          fo_quant{h.universal, h.vars, mk_formula(push_neg(*h.body))}};
      } else if constexpr (std::is_same_v<T, fo_agg>) {
        return formula{fo_agg{h.res_var, h.op, h.agg_var, h.group_by,
                              mk_formula(push_neg(*h.body))}};
      } else if constexpr (std::is_same_v<T, fo_temporal_un>) {
        return mk_tun(h.op, h.intv, push_neg(*h.body));
      } else if constexpr (std::is_same_v<T, fo_temporal_bin>) {
        return mk_tbin(h.op, h.intv, push_neg(*h.l), push_neg(*h.r));
      } else {  // fo_regex
        return formula{fo_regex{h.future, h.intv, map_regex(*h.re, +[](const formula &g) { return push_neg(g); })}};
      }
    },
    f.node);
}

// ---- elim_double_negation (rewriting.ml) ------------------------------
inline formula elim_dneg(const formula &f);

inline formula elim_dneg(const formula &f) {
  return std::visit(
    [&](const auto &h) -> formula {
      using T = std::remove_cvref_t<decltype(h)>;
      if constexpr (std::is_same_v<T, fo_neg>) {
        if (auto *inner = std::get_if<fo_neg>(&h.arg->node))
          return elim_dneg(*inner->arg);  // Neg(Neg f) -> elim f
        return mk_neg(elim_dneg(*h.arg));
      } else if constexpr (std::is_same_v<T, fo_cmp> ||
                           std::is_same_v<T, fo_pred> ||
                           std::is_same_v<T, fo_matches>) {
        return clone_formula(f);
      } else if constexpr (std::is_same_v<T, fo_let>) {
        return formula{fo_let{h.kind, clone_pred(h.head),
                              mk_formula(elim_dneg(*h.bound)),
                              mk_formula(elim_dneg(*h.body))}};
      } else if constexpr (std::is_same_v<T, fo_prop>) {
        return mk_prop(h.op, elim_dneg(*h.l), elim_dneg(*h.r));
      } else if constexpr (std::is_same_v<T, fo_quant>) {
        return formula{
          fo_quant{h.universal, h.vars, mk_formula(elim_dneg(*h.body))}};
      } else if constexpr (std::is_same_v<T, fo_agg>) {
        return formula{fo_agg{h.res_var, h.op, h.agg_var, h.group_by,
                              mk_formula(elim_dneg(*h.body))}};
      } else if constexpr (std::is_same_v<T, fo_temporal_un>) {
        return mk_tun(h.op, h.intv, elim_dneg(*h.body));
      } else if constexpr (std::is_same_v<T, fo_temporal_bin>) {
        return mk_tbin(h.op, h.intv, elim_dneg(*h.l), elim_dneg(*h.r));
      } else {  // fo_regex
        return formula{fo_regex{h.future, h.intv, map_regex(*h.re, +[](const formula &g) { return elim_dneg(g); })}};
      }
    },
    f.node);
}

}  // namespace norm_detail

// The formula monpoly -verified analyses (rewriting.ml:1852). It normalizes
// syntax, then pushes negation -- but only keeps the pushed form when that does
// not lose monitorability:
//   nsf = elim_syntactic_sugar (simplify_terms f)
//   nf  = elim_double_negation (push_negation nsf)
//   rf  = (monitorable nsf && not monitorable nf) ? nsf : nf
// (MonPoly additionally rr-rewrites rf when it is still non-monitorable; that
// pass is the rr-gap staticmon does not implement, and only applies to formulas
// staticmon rejects anyway, so it is not reproduced here.)
inline parser::formula normalize_formula(const parser::formula &f) {
  parser::formula nsf =
    norm_detail::elim_sugar(norm_detail::simplify_terms(f));
  parser::formula nf = norm_detail::elim_dneg(norm_detail::push_neg(nsf));
  if (is_monitorable(nsf).ok && !is_monitorable(nf).ok)
    return nsf;
  return nf;
}

}  // namespace staticmon::compile
