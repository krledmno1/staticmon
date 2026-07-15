#pragma once
// Pretty-printer for parser::formula that reproduces MonPoly's
// MFOTL.string_of_formula / Predicate.string_of_term byte-for-byte (monpoly
// src/MFOTL.ml:658, src/predicate.ml:356). Used by `staticmon-headers -verbose`
// so its "analyzed formula" line matches `monpoly -verbose`. The parenthesizing
// rules (top/par flags) and operator spellings are copied from that source; see
// the reference lines noted at each case.

#include <cstdio>
#include <staticmon/parser/formula_ast.h>
#include <string>
#include <variant>

namespace staticmon::parser {

// Predicate.string_of_cst (predicate.ml:359).
inline std::string print_cst(const constant &c) {
  return std::visit(
    [](const auto &v) -> std::string {
      using T = std::remove_cvref_t<decltype(v)>;
      if constexpr (std::is_same_v<T, cst_int>) {
        return v.value.dec;  // Z.to_string: canonical decimal
      } else if constexpr (std::is_same_v<T, cst_float>) {
        // Printf.sprintf "%g" f -- OCaml's %g is C's %g.
        char buf[64];
        std::snprintf(buf, sizeof buf, "%g", v.value);
        return buf;
      } else if constexpr (std::is_same_v<T, cst_str>) {
        // format_string: "" -> "\"\""; already-quoted -> as-is; else wrap.
        const std::string &s = v.value;
        if (s.empty())
          return "\"\"";
        if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
          return s;
        return "\"" + s + "\"";
      } else {  // cst_regexp -> "r" ^ format_string p
        const std::string &s = v.pattern;
        std::string q = s.empty() ? "\"\""
                        : (s.size() >= 2 && s.front() == '"' && s.back() == '"')
                          ? s
                          : "\"" + s + "\"";
        return "r" + q;
      }
    },
    c);
}

// Predicate.string_of_term (predicate.ml:378). `b` is true at the top of a term
// and inside a function-call argument; a compound term gets parentheses only
// when it appears as an operand (b == false).
inline std::string print_term_rec(bool b, const term &t);

inline std::string print_term_node(bool &no_paren, const term &t) {
  return std::visit(
    [&](const auto &v) -> std::string {
      using T = std::remove_cvref_t<decltype(v)>;
      if constexpr (std::is_same_v<T, term_var>) {
        no_paren = true;
        return v.name;
      } else if constexpr (std::is_same_v<T, term_cst>) {
        no_paren = true;
        return print_cst(v.value);
      } else if constexpr (std::is_same_v<T, term_unary>) {
        no_paren = false;
        if (v.op == term_unop::uminus)
          return "-" + print_term_rec(false, *v.arg);
        const char *fn = "";
        switch (v.op) {
          case term_unop::f2i: fn = "f2i"; break;
          case term_unop::i2f: fn = "i2f"; break;
          case term_unop::i2s: fn = "i2s"; break;
          case term_unop::s2i: fn = "s2i"; break;
          case term_unop::f2s: fn = "f2s"; break;
          case term_unop::s2f: fn = "s2f"; break;
          case term_unop::r2s: fn = "r2s"; break;
          case term_unop::s2r: fn = "s2r"; break;
          case term_unop::year: fn = "YEAR"; break;
          case term_unop::month: fn = "MONTH"; break;
          case term_unop::day_of_month: fn = "DAY_OF_MONTH"; break;
          case term_unop::format_date: fn = "FORMAT_DATE"; break;
          case term_unop::uminus: break;  // handled above
        }
        return std::string(fn) + "(" + print_term_rec(true, *v.arg) + ")";
      } else {  // term_binary
        no_paren = false;
        const char *op = "";
        switch (v.op) {
          case term_binop::plus: op = " + "; break;
          case term_binop::minus: op = " - "; break;
          case term_binop::mult: op = " * "; break;
          case term_binop::div: op = " / "; break;
          case term_binop::mod: op = " mod "; break;  // monpoly prints lowercase
        }
        return print_term_rec(false, *v.l) + op + print_term_rec(false, *v.r);
      }
    },
    t.node);
}

inline std::string print_term_rec(bool b, const term &t) {
  bool no_paren = false;
  std::string s = print_term_node(no_paren, t);
  return (b || no_paren) ? s : "(" + s + ")";
}

inline std::string print_term(const term &t) { return print_term_rec(true, t); }

// MFOTL.string_of_interval (MFOTL.ml:598).
inline std::string print_interval(const interval &iv) {
  auto lo = std::visit(
    [](const auto &b) -> std::string {
      using T = std::remove_cvref_t<decltype(b)>;
      if constexpr (std::is_same_v<T, bnd_open>)
        return "(" + b.value.dec + ",";
      else if constexpr (std::is_same_v<T, bnd_closed>)
        return "[" + b.value.dec + ",";
      else
        return "(*,";
    },
    iv.lower);
  auto hi = std::visit(
    [](const auto &b) -> std::string {
      using T = std::remove_cvref_t<decltype(b)>;
      if constexpr (std::is_same_v<T, bnd_open>)
        return b.value.dec + ")";
      else if constexpr (std::is_same_v<T, bnd_closed>)
        return b.value.dec + "]";
      else
        return "*)";
    },
    iv.upper);
  return lo + hi;
}

// Predicate.string_of_predicate (predicate.ml:415): name ^ "(a,b,c)".
inline std::string print_predicate(const fo_pred &p) {
  std::string s = p.name + "(";
  for (std::size_t i = 0; i < p.args.size(); ++i)
    s += (i ? "," : "") + print_term(p.args[i]);
  return s + ")";
}

inline const char *agg_op_name(agg_op op) {
  switch (op) {
    case agg_op::cnt: return "CNT";
    case agg_op::min: return "MIN";
    case agg_op::max: return "MAX";
    case agg_op::sum: return "SUM";
    case agg_op::avg: return "AVG";
    case agg_op::med: return "MED";
  }
  return "?";
}

inline std::string print_regex_rec(bool top, bool par, const regex &r,
                                   const std::string &padding);

// MFOTL.string_of_formula's string_f_rec (MFOTL.ml:661). `top` marks the root
// (no outer parens), `par` records that the parent already brackets binary
// children. Atoms print bare; unary operators are wrapped only when par (and not
// top); binary operators always take exactly one bracket pair unless at top.
inline std::string print_formula_rec(bool top, bool par, const formula &f,
                                     const std::string &padding) {
  auto join_vars = [](const std::vector<std::string> &vs) {
    std::string s;
    for (std::size_t i = 0; i < vs.size(); ++i)
      s += (i ? ", " : "") + vs[i];
    return s;
  };

  return std::visit(
    [&](const auto &h) -> std::string {
      using T = std::remove_cvref_t<decltype(h)>;

      // ---- atoms: no parentheses ----
      if constexpr (std::is_same_v<T, fo_cmp>) {
        const char *op = h.op == cmp_op::equal      ? " = "
                         : h.op == cmp_op::less      ? " < "
                         : h.op == cmp_op::less_eq   ? " <= "
                                                     : " SUBSTRING ";
        return print_term(h.l) + op + print_term(h.r);
      } else if constexpr (std::is_same_v<T, fo_matches>) {
        std::string s = print_term(h.l) + " MATCHES " + print_term(h.r);
        if (!h.opts.empty()) {
          s += " (";
          for (std::size_t i = 0; i < h.opts.size(); ++i)
            s += (i ? ", " : "") +
                 (h.opts[i] ? print_term(*h.opts[i]) : std::string("_"));
          s += ")";
        }
        return s;
      } else if constexpr (std::is_same_v<T, fo_pred>) {
        return print_predicate(h);
      } else {
        // ---- non-atoms ----
        std::string pre1 = (par && !top) ? "(" : "";
        std::string suf1 = (par && !top) ? ")" : "";
        std::string body;

        if constexpr (std::is_same_v<T, fo_neg>) {
          body = "NOT " + print_formula_rec(false, false, *h.arg, padding);
        } else if constexpr (std::is_same_v<T, fo_quant>) {
          body = std::string(h.universal ? "FORALL " : "EXISTS ") +
                 join_vars(h.vars) + ". " +
                 print_formula_rec(false, false, *h.body, padding);
        } else if constexpr (std::is_same_v<T, fo_agg>) {
          body = h.res_var + " <- " + agg_op_name(h.op) + " " + h.agg_var;
          if (!h.group_by.empty()) {
            body += "; ";
            for (std::size_t i = 0; i < h.group_by.size(); ++i)
              body += (i ? "," : "") + h.group_by[i];
          }
          body += " " + print_formula_rec(false, false, *h.body, padding);
        } else if constexpr (std::is_same_v<T, fo_temporal_un>) {
          const char *nm = "";
          switch (h.op) {
            case temporal_unop::prev: nm = "PREVIOUS"; break;
            case temporal_unop::next: nm = "NEXT"; break;
            case temporal_unop::eventually: nm = "EVENTUALLY"; break;
            case temporal_unop::once: nm = "ONCE"; break;
            case temporal_unop::always: nm = "ALWAYS"; break;
            case temporal_unop::past_always: nm = "PAST_ALWAYS"; break;
          }
          body = std::string(nm) + print_interval(h.intv) + " " +
                 print_formula_rec(false, false, *h.body, padding);
        } else if constexpr (std::is_same_v<T, fo_regex>) {
          body = std::string(h.future ? "|>" : "<|") + print_interval(h.intv) +
                 print_regex_rec(false, false, *h.re, padding);
        } else {
          // ---- binary operators: one extra bracket pair unless top ----
          std::string pre2 = (!par && !top) ? "(" : "";
          std::string suf2 = (!par && !top) ? ")" : "";
          std::string b;
          if constexpr (std::is_same_v<T, fo_prop>) {
            // AND brackets both sides; OR/IMPLIES/EQUIV only the left.
            bool rpar = h.op == prop_binop::and_;
            const char *op = h.op == prop_binop::and_       ? " AND "
                             : h.op == prop_binop::or_       ? " OR "
                             : h.op == prop_binop::implies   ? " IMPLIES "
                                                             : " EQUIV ";
            b = print_formula_rec(false, true, *h.l, padding) + op +
                print_formula_rec(false, rpar, *h.r, padding);
          } else if constexpr (std::is_same_v<T, fo_let>) {
            const char *kw = h.kind == let_kind::let       ? "LET"
                             : h.kind == let_kind::let_past ? "LETPAST"
                                                            : "FRZ";
            b = std::string(kw) + " " +
                print_predicate(h.head) + " = " +
                print_formula_rec(false, true, *h.bound, padding) + "\n" +
                padding + "IN " +
                print_formula_rec(false, false, *h.body, padding);
          } else if constexpr (std::is_same_v<T, fo_temporal_bin>) {
            const char *nm = "";
            switch (h.op) {
              case temporal_binop::since: nm = " SINCE"; break;
              case temporal_binop::trigger: nm = " TRIGGER"; break;
              case temporal_binop::until: nm = " UNTIL"; break;
              case temporal_binop::release: nm = " RELEASE"; break;
            }
            b = print_formula_rec(false, true, *h.l, padding) + nm +
                print_interval(h.intv) + " " +
                print_formula_rec(false, false, *h.r, padding);
          }
          body = pre2 + b + suf2;
        }
        return pre1 + body + suf1;
      }
    },
    f.node);
}

inline std::string print_regex_rec(bool top, bool par, const regex &r,
                                   const std::string &padding) {
  return std::visit(
    [&](const auto &h) -> std::string {
      using T = std::remove_cvref_t<decltype(h)>;
      if constexpr (std::is_same_v<T, re_skip>) {
        return h.n == 1 ? "." : ".{" + std::to_string(h.n) + "}";
      } else {
        std::string pre1 = (par && !top) ? "(" : "";
        std::string suf1 = (par && !top) ? ")" : "";
        std::string body;
        if constexpr (std::is_same_v<T, re_test>) {
          body = print_formula_rec(false, false, *h.f, padding) + "?";
        } else if constexpr (std::is_same_v<T, re_star>) {
          body = print_regex_rec(false, false, *h.arg, padding) + "*";
        } else {
          std::string pre2 = (!par && !top) ? "(" : "";
          std::string suf2 = (!par && !top) ? ")" : "";
          std::string b;
          if constexpr (std::is_same_v<T, re_concat>)
            b = print_regex_rec(false, true, *h.l, padding) + " " +
                print_regex_rec(false, false, *h.r, padding);
          else  // re_plus
            b = print_regex_rec(false, true, *h.l, padding) + " + " +
                print_regex_rec(false, false, *h.r, padding);
          body = pre2 + b + suf2;
        }
        return pre1 + body + suf1;
      }
    },
    r.node);
}

// Top-level printer. `prefix` mirrors MFOTL.string_of_formula's `str`: it is
// prepended and its last line sets the indentation used before LET's "IN".
inline std::string print_formula(const formula &f, const std::string &prefix) {
  auto nl = prefix.find_last_of('\n');
  std::string last = nl == std::string::npos ? prefix : prefix.substr(nl + 1);
  std::string padding(last.size(), ' ');
  return prefix + print_formula_rec(true, false, f, padding);
}

}  // namespace staticmon::parser
