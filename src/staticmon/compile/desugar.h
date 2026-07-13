#pragma once
// Stage 3a (desugaring): MonPoly's Rewriting.elim_syntactic_sugar over the
// parser AST (see docs/explicitmon-pipeline.md §4). Eliminates Implies, Equiv,
// ForAll, Always, PastAlways into the core fragment and prunes vacuous Exists.
// Matches monpoly-exp with verified=false (the explicitmon setting): Always ->
// Neg(Eventually(Neg .)), PastAlways -> Neg(Once(Neg .)).
//
// Operates by value (deep clone) because Equiv duplicates its operands.

#include <staticmon/compile/free_vars.h>
#include <staticmon/parser/formula_ast.h>
#include <algorithm>
#include <memory>
#include <variant>

namespace staticmon::compile {

// ---- deep clone ----------------------------------------------------------
inline parser::term clone_term(const parser::term &t) {
  using namespace parser;
  return std::visit(
    [&](const auto &v) -> term {
      using T = std::remove_cvref_t<decltype(v)>;
      if constexpr (std::is_same_v<T, term_var> || std::is_same_v<T, term_cst>)
        return term{v};
      else if constexpr (std::is_same_v<T, term_unary>)
        return term{term_unary{v.op, mk_term(clone_term(*v.arg))}};
      else
        return term{term_binary{v.op, mk_term(clone_term(*v.l)),
                                mk_term(clone_term(*v.r))}};
    },
    t.node);
}

parser::formula clone_formula(const parser::formula &f);

inline parser::fo_pred clone_pred(const parser::fo_pred &p) {
  std::vector<parser::term> args;
  args.reserve(p.args.size());
  for (const auto &a : p.args)
    args.push_back(clone_term(a));
  return parser::fo_pred{p.name, std::move(args)};
}

inline parser::regex clone_regex(const parser::regex &r) {
  using namespace parser;
  return std::visit(
    [&](const auto &v) -> regex {
      using T = std::remove_cvref_t<decltype(v)>;
      if constexpr (std::is_same_v<T, re_skip>)
        return regex{v};
      else if constexpr (std::is_same_v<T, re_test>)
        return regex{re_test{mk_formula(clone_formula(*v.f))}};
      else if constexpr (std::is_same_v<T, re_concat>)
        return regex{re_concat{mk_regex(clone_regex(*v.l)),
                               mk_regex(clone_regex(*v.r))}};
      else if constexpr (std::is_same_v<T, re_plus>)
        return regex{re_plus{mk_regex(clone_regex(*v.l)),
                             mk_regex(clone_regex(*v.r))}};
      else
        return regex{re_star{mk_regex(clone_regex(*v.arg))}};
    },
    r.node);
}

inline parser::formula clone_formula(const parser::formula &f) {
  using namespace parser;
  return std::visit(
    [&](const auto &v) -> formula {
      using T = std::remove_cvref_t<decltype(v)>;
      if constexpr (std::is_same_v<T, fo_cmp>)
        return formula{fo_cmp{v.op, clone_term(v.l), clone_term(v.r)}};
      else if constexpr (std::is_same_v<T, fo_matches>) {
        std::vector<std::optional<term>> opts;
        for (const auto &o : v.opts)
          opts.push_back(o ? std::optional<term>(clone_term(*o))
                           : std::nullopt);
        return formula{fo_matches{clone_term(v.l), clone_term(v.r),
                                  std::move(opts)}};
      } else if constexpr (std::is_same_v<T, fo_pred>) {
        std::vector<term> args;
        for (const auto &a : v.args)
          args.push_back(clone_term(a));
        return formula{fo_pred{v.name, std::move(args)}};
      } else if constexpr (std::is_same_v<T, fo_let>)
        return formula{fo_let{v.kind, clone_pred(v.head),
                              mk_formula(clone_formula(*v.bound)),
                              mk_formula(clone_formula(*v.body))}};
      else if constexpr (std::is_same_v<T, fo_neg>)
        return formula{fo_neg{mk_formula(clone_formula(*v.arg))}};
      else if constexpr (std::is_same_v<T, fo_prop>)
        return formula{fo_prop{v.op, mk_formula(clone_formula(*v.l)),
                               mk_formula(clone_formula(*v.r))}};
      else if constexpr (std::is_same_v<T, fo_quant>)
        return formula{fo_quant{v.universal, v.vars,
                                mk_formula(clone_formula(*v.body))}};
      else if constexpr (std::is_same_v<T, fo_agg>)
        return formula{fo_agg{v.res_var, v.op, v.agg_var, v.group_by,
                              mk_formula(clone_formula(*v.body))}};
      else if constexpr (std::is_same_v<T, fo_temporal_un>)
        return formula{fo_temporal_un{v.op, v.intv,
                                      mk_formula(clone_formula(*v.body))}};
      else if constexpr (std::is_same_v<T, fo_temporal_bin>)
        return formula{fo_temporal_bin{v.op, v.intv,
                                       mk_formula(clone_formula(*v.l)),
                                       mk_formula(clone_formula(*v.r))}};
      else
        return formula{fo_regex{v.future, v.intv,
                                mk_regex(clone_regex(*v.re))}};
    },
    f.node);
}

// ---- desugar -------------------------------------------------------------
namespace desugar_detail {
  using namespace parser;

  inline formula mk_neg(formula f) {
    return formula{fo_neg{mk_formula(std::move(f))}};
  }
}  // namespace desugar_detail

inline parser::formula desugar(const parser::formula &f) {
  using namespace parser;
  return std::visit(
    [&](const auto &v) -> formula {
      using T = std::remove_cvref_t<decltype(v)>;
      if constexpr (std::is_same_v<T, fo_cmp> ||
                    std::is_same_v<T, fo_matches> ||
                    std::is_same_v<T, fo_pred>) {
        return clone_formula(f);
      } else if constexpr (std::is_same_v<T, fo_let>) {
        return formula{fo_let{v.kind,
                              clone_pred(v.head),
                              mk_formula(desugar(*v.bound)),
                              mk_formula(desugar(*v.body))}};
      } else if constexpr (std::is_same_v<T, fo_neg>) {
        return desugar_detail::mk_neg(desugar(*v.arg));
      } else if constexpr (std::is_same_v<T, fo_prop>) {
        switch (v.op) {
          case prop_binop::and_:
            return formula{fo_prop{prop_binop::and_, mk_formula(desugar(*v.l)),
                                   mk_formula(desugar(*v.r))}};
          case prop_binop::or_:
            return formula{fo_prop{prop_binop::or_, mk_formula(desugar(*v.l)),
                                   mk_formula(desugar(*v.r))}};
          case prop_binop::implies:
            // Implies(f1,f2) -> Or(Neg f1, f2)
            return formula{fo_prop{
              prop_binop::or_,
              mk_formula(desugar_detail::mk_neg(desugar(*v.l))),
              mk_formula(desugar(*v.r))}};
          case prop_binop::equiv: {
            // Equiv(f1,f2) -> And(Or(Neg f1, f2), Or(f1, Neg f2))
            formula left{fo_prop{
              prop_binop::or_,
              mk_formula(desugar_detail::mk_neg(desugar(*v.l))),
              mk_formula(desugar(*v.r))}};
            formula right{fo_prop{
              prop_binop::or_, mk_formula(desugar(*v.l)),
              mk_formula(desugar_detail::mk_neg(desugar(*v.r)))}};
            return formula{fo_prop{prop_binop::and_,
                                   mk_formula(std::move(left)),
                                   mk_formula(std::move(right))}};
          }
        }
        return clone_formula(f);
      } else if constexpr (std::is_same_v<T, fo_quant>) {
        if (!v.universal) {
          // Exists: prune vars not free in the desugared body; drop if empty.
          formula body = desugar(*v.body);
          auto fv = free_vars(body);
          std::vector<std::string> kept;
          for (const auto &x : v.vars)
            if (std::find(fv.begin(), fv.end(), x) != fv.end())
              kept.push_back(x);
          if (kept.empty())
            return body;
          return formula{fo_quant{false, std::move(kept),
                                  mk_formula(std::move(body))}};
        }
        // ForAll(v,f) -> Neg(Exists(v, Neg f))
        formula inner{fo_quant{false, v.vars,
                               mk_formula(desugar_detail::mk_neg(
                                 desugar(*v.body)))}};
        return desugar(desugar_detail::mk_neg(std::move(inner)));
      } else if constexpr (std::is_same_v<T, fo_agg>) {
        return formula{fo_agg{v.res_var, v.op, v.agg_var, v.group_by,
                              mk_formula(desugar(*v.body))}};
      } else if constexpr (std::is_same_v<T, fo_temporal_un>) {
        switch (v.op) {
          case temporal_unop::always:
            // Always(i,f) -> Neg(Eventually(i, Neg f))
            return desugar_detail::mk_neg(formula{fo_temporal_un{
              temporal_unop::eventually, v.intv,
              mk_formula(desugar_detail::mk_neg(desugar(*v.body)))}});
          case temporal_unop::past_always:
            // PastAlways(i,f) -> Neg(Once(i, Neg f))
            return desugar_detail::mk_neg(formula{fo_temporal_un{
              temporal_unop::once, v.intv,
              mk_formula(desugar_detail::mk_neg(desugar(*v.body)))}});
          default:
            return formula{fo_temporal_un{v.op, v.intv,
                                          mk_formula(desugar(*v.body))}};
        }
      } else if constexpr (std::is_same_v<T, fo_temporal_bin>) {
        return formula{fo_temporal_bin{v.op, v.intv, mk_formula(desugar(*v.l)),
                                       mk_formula(desugar(*v.r))}};
      } else {  // fo_regex — elim over regex tests (structure preserved)
        return clone_formula(f);
      }
    },
    f.node);
}

}  // namespace staticmon::compile
