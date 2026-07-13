#pragma once
// Prints the parser AST in the exact format of test/parser_oracle/oracle.ml
// so that differential comparison is plain string equality.
//   - strings: OCaml String.escaped inside double quotes
//   - floats: OCaml %h (C99 hex-float; "infinity"/"nan" spelled out)
//   - integers: canonical decimal (big_int is already normalized)

#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <staticmon/parser/formula_ast.h>
#include <string>
#include <variant>

namespace staticmon::parser {

namespace detail {

  inline void ocaml_escape_into(std::string &out, std::string_view s) {
    for (char sc : s) {
      auto c = static_cast<unsigned char>(sc);
      switch (c) {
        case '"':
          out += "\\\"";
          break;
        case '\\':
          out += "\\\\";
          break;
        case '\n':
          out += "\\n";
          break;
        case '\t':
          out += "\\t";
          break;
        case '\r':
          out += "\\r";
          break;
        case '\b':
          out += "\\b";
          break;
        default:
          if (c >= 32 && c <= 126) {
            out += static_cast<char>(c);
          } else {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\%03u", c);
            out += buf;
          }
      }
    }
  }

  // OCaml Printf %h: hexadecimal float, or "infinity"/"-infinity"/"nan".
  inline void ocaml_hex_float_into(std::string &out, double v) {
    if (std::isnan(v)) {
      out += "nan";
      return;
    }
    if (std::isinf(v)) {
      out += v < 0 ? "-infinity" : "infinity";
      return;
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%a", v);
    out += buf;
  }

}  // namespace detail

class sexp_printer {
public:
  std::string print_ok(const formula &f) {
    out_.clear();
    out_ += "(ok ";
    print(f);
    out_ += ")";
    return out_;
  }

  static std::string print_parse_error(std::string_view msg) {
    std::string out = "(parse_error \"";
    detail::ocaml_escape_into(out, msg);
    out += "\")";
    return out;
  }

private:
  void str(std::string_view s) {
    out_ += '"';
    detail::ocaml_escape_into(out_, s);
    out_ += '"';
  }

  void print(const constant &c) {
    std::visit(
      [&](const auto &v) {
        using T = std::remove_cvref_t<decltype(v)>;
        if constexpr (std::is_same_v<T, cst_int>) {
          out_ += "(Int ";
          out_ += v.value.dec;
          out_ += ")";
        } else if constexpr (std::is_same_v<T, cst_float>) {
          out_ += "(Float ";
          detail::ocaml_hex_float_into(out_, v.value);
          out_ += ")";
        } else if constexpr (std::is_same_v<T, cst_str>) {
          out_ += "(Str ";
          str(v.value);
          out_ += ")";
        } else {
          out_ += "(Regexp ";
          str(v.pattern);
          out_ += ")";
        }
      },
      c);
  }

  static constexpr const char *term_unop_name(term_unop op) {
    switch (op) {
      case term_unop::f2i: return "F2i";
      case term_unop::i2f: return "I2f";
      case term_unop::i2s: return "I2s";
      case term_unop::s2i: return "S2i";
      case term_unop::f2s: return "F2s";
      case term_unop::s2f: return "S2f";
      case term_unop::day_of_month: return "DayOfMonth";
      case term_unop::month: return "Month";
      case term_unop::year: return "Year";
      case term_unop::format_date: return "FormatDate";
      case term_unop::r2s: return "R2s";
      case term_unop::s2r: return "S2r";
      case term_unop::uminus: return "UMinus";
    }
    return "?";
  }

  static constexpr const char *term_binop_name(term_binop op) {
    switch (op) {
      case term_binop::plus: return "Plus";
      case term_binop::minus: return "Minus";
      case term_binop::mult: return "Mult";
      case term_binop::div: return "Div";
      case term_binop::mod: return "Mod";
    }
    return "?";
  }

  void print(const term &t) {
    std::visit(
      [&](const auto &v) {
        using T = std::remove_cvref_t<decltype(v)>;
        if constexpr (std::is_same_v<T, term_var>) {
          out_ += "(Var ";
          str(v.name);
          out_ += ")";
        } else if constexpr (std::is_same_v<T, term_cst>) {
          out_ += "(Cst ";
          print(v.value);
          out_ += ")";
        } else if constexpr (std::is_same_v<T, term_unary>) {
          out_ += "(";
          out_ += term_unop_name(v.op);
          out_ += " ";
          print(*v.arg);
          out_ += ")";
        } else {
          out_ += "(";
          out_ += term_binop_name(v.op);
          out_ += " ";
          print(*v.l);
          out_ += " ";
          print(*v.r);
          out_ += ")";
        }
      },
      t.node);
  }

  void print(const interval_bound &b) {
    std::visit(
      [&](const auto &v) {
        using T = std::remove_cvref_t<decltype(v)>;
        if constexpr (std::is_same_v<T, bnd_open>) {
          out_ += "(OBnd ";
          out_ += v.value.dec;
          out_ += ")";
        } else if constexpr (std::is_same_v<T, bnd_closed>) {
          out_ += "(CBnd ";
          out_ += v.value.dec;
          out_ += ")";
        } else {
          out_ += "Inf";
        }
      },
      b);
  }

  void print(const interval &i) {
    out_ += "(Interval ";
    print(i.lower);
    out_ += " ";
    print(i.upper);
    out_ += ")";
  }

  void var_list(const std::vector<std::string> &vs) {
    out_ += "(";
    for (std::size_t i = 0; i < vs.size(); ++i) {
      if (i > 0)
        out_ += " ";
      str(vs[i]);
    }
    out_ += ")";
  }

  void print(const fo_pred &p) {
    out_ += "(Pred ";
    str(p.name);
    out_ += " ";
    out_ += std::to_string(p.args.size());
    out_ += " (";
    for (std::size_t i = 0; i < p.args.size(); ++i) {
      if (i > 0)
        out_ += " ";
      print(p.args[i]);
    }
    out_ += "))";
  }

  static constexpr const char *agg_op_name(agg_op op) {
    switch (op) {
      case agg_op::cnt: return "Cnt";
      case agg_op::min: return "Min";
      case agg_op::max: return "Max";
      case agg_op::sum: return "Sum";
      case agg_op::avg: return "Avg";
      case agg_op::med: return "Med";
    }
    return "?";
  }

  void print(const regex &r) {
    std::visit(
      [&](const auto &v) {
        using T = std::remove_cvref_t<decltype(v)>;
        if constexpr (std::is_same_v<T, re_skip>) {
          out_ += "(Skip ";
          out_ += std::to_string(v.n);
          out_ += ")";
        } else if constexpr (std::is_same_v<T, re_test>) {
          out_ += "(Test ";
          print(*v.f);
          out_ += ")";
        } else if constexpr (std::is_same_v<T, re_concat>) {
          out_ += "(Concat ";
          print(*v.l);
          out_ += " ";
          print(*v.r);
          out_ += ")";
        } else if constexpr (std::is_same_v<T, re_plus>) {
          out_ += "(Plus ";
          print(*v.l);
          out_ += " ";
          print(*v.r);
          out_ += ")";
        } else {
          out_ += "(Star ";
          print(*v.arg);
          out_ += ")";
        }
      },
      r.node);
  }

  void print(const formula &f) {
    std::visit(
      [&](const auto &v) {
        using T = std::remove_cvref_t<decltype(v)>;
        if constexpr (std::is_same_v<T, fo_cmp>) {
          switch (v.op) {
            case cmp_op::equal: out_ += "(Equal "; break;
            case cmp_op::less: out_ += "(Less "; break;
            case cmp_op::less_eq: out_ += "(LessEq "; break;
            case cmp_op::substring: out_ += "(Substring "; break;
          }
          print(v.l);
          out_ += " ";
          print(v.r);
          out_ += ")";
        } else if constexpr (std::is_same_v<T, fo_matches>) {
          out_ += "(Matches ";
          print(v.l);
          out_ += " ";
          print(v.r);
          out_ += " (";
          for (std::size_t i = 0; i < v.opts.size(); ++i) {
            if (i > 0)
              out_ += " ";
            if (v.opts[i]) {
              out_ += "(Some ";
              print(*v.opts[i]);
              out_ += ")";
            } else {
              out_ += "None";
            }
          }
          out_ += "))";
        } else if constexpr (std::is_same_v<T, fo_pred>) {
          print(v);
        } else if constexpr (std::is_same_v<T, fo_let>) {
          switch (v.kind) {
            case let_kind::let: out_ += "(Let "; break;
            case let_kind::let_past: out_ += "(LetPast "; break;
            case let_kind::frz: out_ += "(Frz "; break;
          }
          print(v.head);
          out_ += " ";
          print(*v.bound);
          out_ += " ";
          print(*v.body);
          out_ += ")";
        } else if constexpr (std::is_same_v<T, fo_neg>) {
          out_ += "(Neg ";
          print(*v.arg);
          out_ += ")";
        } else if constexpr (std::is_same_v<T, fo_prop>) {
          switch (v.op) {
            case prop_binop::and_: out_ += "(And "; break;
            case prop_binop::or_: out_ += "(Or "; break;
            case prop_binop::implies: out_ += "(Implies "; break;
            case prop_binop::equiv: out_ += "(Equiv "; break;
          }
          print(*v.l);
          out_ += " ";
          print(*v.r);
          out_ += ")";
        } else if constexpr (std::is_same_v<T, fo_quant>) {
          out_ += v.universal ? "(ForAll " : "(Exists ";
          var_list(v.vars);
          out_ += " ";
          print(*v.body);
          out_ += ")";
        } else if constexpr (std::is_same_v<T, fo_agg>) {
          out_ += "(Aggreg ";
          str(v.res_var);
          out_ += " ";
          out_ += agg_op_name(v.op);
          out_ += " ";
          str(v.agg_var);
          out_ += " ";
          var_list(v.group_by);
          out_ += " ";
          print(*v.body);
          out_ += ")";
        } else if constexpr (std::is_same_v<T, fo_temporal_un>) {
          switch (v.op) {
            case temporal_unop::prev: out_ += "(Prev "; break;
            case temporal_unop::next: out_ += "(Next "; break;
            case temporal_unop::eventually: out_ += "(Eventually "; break;
            case temporal_unop::once: out_ += "(Once "; break;
            case temporal_unop::always: out_ += "(Always "; break;
            case temporal_unop::past_always: out_ += "(PastAlways "; break;
          }
          print(v.intv);
          out_ += " ";
          print(*v.body);
          out_ += ")";
        } else if constexpr (std::is_same_v<T, fo_temporal_bin>) {
          switch (v.op) {
            case temporal_binop::since: out_ += "(Since "; break;
            case temporal_binop::trigger: out_ += "(Trigger "; break;
            case temporal_binop::until: out_ += "(Until "; break;
            case temporal_binop::release: out_ += "(Release "; break;
          }
          print(v.intv);
          out_ += " ";
          print(*v.l);
          out_ += " ";
          print(*v.r);
          out_ += ")";
        } else if constexpr (std::is_same_v<T, fo_regex>) {
          out_ += v.future ? "(Frex " : "(Prex ";
          print(v.intv);
          out_ += " ";
          print(*v.re);
          out_ += ")";
        } else {
          static_assert(always_false_of<T>, "unhandled formula node");
        }
      },
      f.node);
  }

  template<typename>
  static constexpr bool always_false_of = false;

  std::string out_;
};

}  // namespace staticmon::parser
