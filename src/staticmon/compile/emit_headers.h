#pragma once
// Stage 5 (codegen): render an exformula IR to the two header files
// formula_in.h and formula_csts.h, mirroring
// monpoly-exp/src/explicitmon.ml's cpp_of_exformula / print_* functions
// (see docs/explicitmon-pipeline.md §3). Output is semantically identical to
// monpoly-exp's; whitespace/line-wrapping differ (the header-diff harness
// normalizes whitespace), so we format for readability rather than byte
// parity with OCaml's Format boxes.

#include <staticmon/compile/exformula.h>
#include <staticmon/parser/formula_ast.h>
#include <string>
#include <variant>
#include <vector>

namespace staticmon::compile {

struct emitted_headers {
  std::string formula_in;    // contents of formula_in.h
  std::string formula_csts;  // contents of formula_csts.h
};

class header_emitter {
public:
  emitted_headers emit(const translated &t) {
    scsts_.clear();
    fcsts_.clear();
    std::string in;
    in += "using input_formula =\n  ";
    in += render_formula(*t.formula);
    in += ";\n";
    in += "using free_variables =\n  ";
    in += render_var_list(t.free_variables);
    in += ";\n";
    in += "inline static const pred_map_t input_predicates =\n  ";
    in += render_pred_map(t.predicates);
    in += ";\n";

    std::string csts;
    for (const auto &[name, val] : scsts_) {
      csts += "struct " + name + " {\n";
      csts += "  using value_type = std::string_view;\n";
      csts += "  static constexpr std::string_view value = \"" + val + "\"sv;\n";
      csts += "};\n";
    }
    for (const auto &[name, val] : fcsts_) {
      csts += "struct " + name + " {\n";
      csts += "  using value_type = double;\n";
      csts += "  static constexpr double value = " + val + ";\n";
      csts += "};\n";
    }
    return {std::move(in), std::move(csts)};
  }

private:
  static const char *ctype(val_type t) {
    switch (t) {
      case val_type::t_int: return "std::int64_t";
      case val_type::t_str: return "std::string";
      case val_type::t_float: return "double";
    }
    return "?";
  }
  static const char *arg_type(val_type t) {
    switch (t) {
      case val_type::t_int: return "INT_TYPE";
      case val_type::t_str: return "STRING_TYPE";
      case val_type::t_float: return "FLOAT_TYPE";
    }
    return "?";
  }
  static const char *agg_op_name(agg_op op) {
    switch (op) {
      case agg_op::cnt: return "cnt_agg_op";
      case agg_op::min: return "min_agg_op";
      case agg_op::max: return "max_agg_op";
      case agg_op::sum: return "sum_agg_op";
      case agg_op::avg: return "avg_agg_op";
      case agg_op::med: return "med_agg_op";
    }
    return "?";
  }
  static const char *cst_type_name(cst_type t) {
    switch (t) {
      case cst_type::eq: return "cst_eq";
      case cst_type::less: return "cst_less";
      case cst_type::less_eq: return "cst_less_eq";
    }
    return "?";
  }

  // A constant used as a *value* (mp_int64_t / string_cst_N / float_cst_N).
  std::string render_cst(const constant &c) {
    return std::visit(
      [&](const auto &v) -> std::string {
        using T = std::remove_cvref_t<decltype(v)>;
        if constexpr (std::is_same_v<T, parser::cst_int>) {
          return "mp_int64_t<" + v.value.dec + ">";
        } else if constexpr (std::is_same_v<T, parser::cst_float>) {
          std::string name = "float_cst_" + std::to_string(++cst_counter_);
          fcsts_.emplace_back(name, ocaml_float_string(v.value));
          return name;
        } else if constexpr (std::is_same_v<T, parser::cst_str>) {
          std::string name = "string_cst_" + std::to_string(++cst_counter_);
          scsts_.emplace_back(name, v.value);
          return name;
        } else {
          return "/*regexp-unsupported*/";
        }
      },
      c);
  }

  std::string render_term(const ex_term &t) {
    return std::visit(
      [&](const auto &v) -> std::string {
        using T = std::remove_cvref_t<decltype(v)>;
        if constexpr (std::is_same_v<T, ex_tvar>) {
          return "tvar<" + std::to_string(v.id) + ">";
        } else if constexpr (std::is_same_v<T, ex_tcst>) {
          return "tcst<" + render_cst(v.value) + ">";
        } else if constexpr (std::is_same_v<T, ex_tunary>) {
          const char *n = v.op == ex_term_unop::f2i    ? "tf2i"
                          : v.op == ex_term_unop::i2f   ? "ti2f"
                                                        : "tuminus";
          return std::string(n) + "<" + render_term(*v.arg) + ">";
        } else {
          const auto &b = v;
          const char *n = b.op == ex_term_binop::plus    ? "tplus"
                          : b.op == ex_term_binop::minus  ? "tminus"
                          : b.op == ex_term_binop::mult    ? "tmult"
                          : b.op == ex_term_binop::div     ? "tdiv"
                                                           : "tmod";
          return std::string(n) + "<" + render_term(*b.l) + ", " +
                 render_term(*b.r) + ">";
        }
      },
      t.node);
  }

  std::string render_predarg(const ex_predarg &a) {
    return std::visit(
      [&](const auto &v) -> std::string {
        using T = std::remove_cvref_t<decltype(v)>;
        if constexpr (std::is_same_v<T, ex_pvar>) {
          return "pvar<" + std::string(ctype(v.type)) + ", " +
                 std::to_string(v.id) + ">";
        } else {
          return "pcst<" + render_cst(v.value) + ">";
        }
      },
      a);
  }

  std::string render_bound(const ex_bound &b) {
    if (std::holds_alternative<ex_inf>(b))
      return "inf_bound";
    return "mp_size_t<" + std::get<ex_bnd>(b).value.dec + ">";
  }

  std::string render_var_list(const std::vector<var_id> &vars) {
    if (vars.empty())
      return "mp_list<>";
    std::string s = "mp_list_c<std::size_t";
    for (var_id v : vars)
      s += ", " + std::to_string(v);
    s += ">";
    return s;
  }

  std::string render_agg_info(const ex_aggreg_info &info) {
    return std::to_string(info.res_var) + ", " + agg_op_name(info.op) + ", " +
           std::to_string(info.agg_var) + ", " + render_var_list(info.group_by);
  }

  std::string render_sop(const simple_op &s) {
    return std::visit(
      [&](const auto &v) -> std::string {
        using T = std::remove_cvref_t<decltype(v)>;
        if constexpr (std::is_same_v<T, sop_and_assign>) {
          return "mandassign<" + std::to_string(v.res_var) + ", " +
                 render_term(v.term) + ">";
        } else if constexpr (std::is_same_v<T, sop_and_rel>) {
          return "mandrel<" + std::string(v.negated ? "true" : "false") + ", " +
                 cst_type_name(v.op) + ", " + render_term(v.l) + ", " +
                 render_term(v.r) + ">";
        } else {
          std::string s = "mexists<";
          for (std::size_t i = 0; i < v.vars.size(); ++i)
            s += (i ? ", " : "") + std::to_string(v.vars[i]);
          s += ">";
          return s;
        }
      },
      s);
  }

  std::string render_formula(const exformula &f) {
    return std::visit(
      [&](const auto &v) -> std::string {
        using T = std::remove_cvref_t<decltype(v)>;
        if constexpr (std::is_same_v<T, ex_predicate>) {
          std::string s = "mpredicate<" + std::to_string(v.id);
          for (const auto &a : v.args)
            s += ", " + render_predarg(a);
          return s + ">";
        } else if constexpr (std::is_same_v<T, ex_builtin_tp>) {
          return "mtp<" + render_predarg(v.arg) + ">";
        } else if constexpr (std::is_same_v<T, ex_builtin_ts>) {
          return "mts<" + render_predarg(v.arg) + ">";
        } else if constexpr (std::is_same_v<T, ex_builtin_tpts>) {
          return "mtpts<" + render_predarg(v.arg1) + ", " +
                 render_predarg(v.arg2) + ">";
        } else if constexpr (std::is_same_v<T, ex_and>) {
          return "mand<" +
                 std::string(v.jt == join_type::anti_join ? "true" : "false") +
                 ", " + render_formula(*v.l) + ", " + render_formula(*v.r) + ">";
        } else if constexpr (std::is_same_v<T, ex_or>) {
          return "mor<" + render_formula(*v.l) + ", " + render_formula(*v.r) +
                 ">";
        } else if constexpr (std::is_same_v<T, ex_neg>) {
          return "mneg<" + render_formula(*v.arg) + ">";
        } else if constexpr (std::is_same_v<T, ex_eq>) {
          return "mequal<" + render_term(v.l) + ", " + render_term(v.r) + ">";
        } else if constexpr (std::is_same_v<T, ex_empty_rel>) {
          return "memptyrel";
        } else if constexpr (std::is_same_v<T, ex_temporal_un>) {
          const char *n = v.k == ex_temporal_un::kind::prev   ? "mprev"
                          : v.k == ex_temporal_un::kind::next  ? "mnext"
                          : v.k == ex_temporal_un::kind::once  ? "monce"
                                                               : "meventually";
          return std::string(n) + "<" + render_bound(v.intv.lower) + ", " +
                 render_bound(v.intv.upper) + ", " + render_formula(*v.arg) +
                 ">";
        } else if constexpr (std::is_same_v<T, ex_since>) {
          const char *n = v.future ? "muntil" : "msince";
          return std::string(n) + "<" +
                 std::string(v.negated ? "true" : "false") + ", " +
                 render_bound(v.intv.lower) + ", " + render_bound(v.intv.upper) +
                 ", " + render_formula(*v.l) + ", " + render_formula(*v.r) + ">";
        } else if constexpr (std::is_same_v<T, ex_once_agg>) {
          return "monceagg<" + render_agg_info(v.info) + ", " +
                 render_bound(v.intv.lower) + ", " + render_bound(v.intv.upper) +
                 ", " + render_formula(*v.arg) + ">";
        } else if constexpr (std::is_same_v<T, ex_since_agg>) {
          return "msinceagg<" + render_agg_info(v.info) + ", " +
                 std::string(v.negated ? "true" : "false") + ", " +
                 render_bound(v.intv.lower) + ", " + render_bound(v.intv.upper) +
                 ", " + render_formula(*v.l) + ", " + render_formula(*v.r) + ">";
        } else if constexpr (std::is_same_v<T, ex_aggregation>) {
          return "maggregation<" + render_agg_info(v.info) + ", " +
                 render_formula(*v.arg) + ">";
        } else if constexpr (std::is_same_v<T, ex_fused>) {
          std::string s = "mfusedsimpleop<simpleops<";
          for (std::size_t i = 0; i < v.sops.size(); ++i)
            s += (i ? ", " : "") + render_sop(v.sops[i]);
          s += ">, " + render_formula(*v.arg) + ">";
          return s;
        } else {  // ex_let
          return std::string(v.past ? "mletpast<" : "mlet<") +
                 std::to_string(v.id) + ", " + render_var_list(v.pred_layout) +
                 ", " + render_formula(*v.bound) + ", " +
                 render_formula(*v.body) + ">";
        }
      },
      f.node);
  }

  std::string render_pred_map(const std::vector<pred_info> &preds) {
    std::string s = "{";
    for (std::size_t i = 0; i < preds.size(); ++i) {
      const auto &p = preds[i];
      if (i)
        s += ", ";
      s += "{\"" + p.name + "\", {" + std::to_string(p.id) + ", {";
      for (std::size_t j = 0; j < p.arg_types.size(); ++j)
        s += (j ? ", " : "") + std::string(arg_type(p.arg_types[j]));
      s += "}}}";
    }
    s += "}";
    return s;
  }

  // OCaml Float.to_string: shortest round-trip decimal, always with a '.'.
  static std::string ocaml_float_string(double v);

  std::size_t cst_counter_ = 0;
  std::vector<std::pair<std::string, std::string>> scsts_;
  std::vector<std::pair<std::string, std::string>> fcsts_;
};

inline std::string header_emitter::ocaml_float_string(double v) {
  // Match OCaml's Float.to_string closely enough for the C++ compiler: a
  // decimal literal with a fractional part. %.17g round-trips doubles; ensure
  // a '.' or exponent is present so it is a floating literal.
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.17g", v);
  std::string s(buf);
  if (s.find('.') == std::string::npos && s.find('e') == std::string::npos &&
      s.find("inf") == std::string::npos && s.find("nan") == std::string::npos)
    s += ".";
  return s;
}

}  // namespace staticmon::compile
