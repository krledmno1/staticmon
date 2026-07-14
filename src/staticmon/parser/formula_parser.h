#pragma once
// Recursive-descent / Pratt parser for MFOTL formulas, functionally
// equivalent to monpoly's ocamlyacc grammar (docs/monpoly-grammar.md §2).
//
// The yacc precedence table maps to binding powers (higher = tighter):
//   2 AGGREG | 3 LET/FRZ/IN | 4 SINCE/TRIGGER/UNTIL/RELEASE (right)
//   5 temporal unaries | 6 EXISTS/FORALL | 7 EQUIV (left) | 8 IMPLIES (right)
//   9 OR (left) | 10 AND (left) | 12 NOT | 13 BASE (regex formula atoms)
// A prefix rule's body is parsed at the rule's own binding power, which
// reproduces yacc's shift/reduce resolution (e.g. `PREV a AND b` extends the
// body because AND(10) > PREV(5); `EXISTS x. a SINCE b` reduces the
// quantifier first because SINCE(4) < EX(6)).
//
// The `(term)` vs `(formula)` ambiguity (both `(x) = 5` and `(P(x))` are
// valid) is resolved by speculation with backtracking, mirroring what LALR
// state merging does. Term parsing is greedy (e.g. after `x = 4` a `*` is
// consumed into the term, as yacc's shift preference does).

#include <algorithm>
#include <cstdint>
#include <staticmon/parser/formula_ast.h>
#include <staticmon/parser/formula_lexer.h>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace staticmon::parser {

struct parse_error {
  std::size_t pos;
  std::string message;
};

class formula_parser {
public:
  // Parses a complete formula (trailing input is an error, like yacc's
  // implicit end-of-input in the augmented start rule).
  static std::variant<formula, parse_error> parse(std::string_view input) {
    formula_lexer lexer(input);
    auto lexed = lexer.tokenize();
    if (auto *err = std::get_if<lex_error>(&lexed))
      return parse_error{err->pos, err->message};
    formula_parser p(std::get<std::vector<token>>(std::move(lexed)));
    try {
      formula f = p.parse_formula(0);
      p.expect(token_type::END_OF_INPUT);
      return f;
    } catch (error_exc &e) {
      return std::move(e.err);
    }
  }

private:
  struct error_exc {
    parse_error err;
  };

  explicit formula_parser(std::vector<token> toks) : toks_(std::move(toks)) {}

  [[noreturn]] void fail(std::string msg) {
    throw error_exc{parse_error{peek().pos, std::move(msg)}};
  }

  const token &peek(std::size_t k = 0) const {
    std::size_t i = idx_ + k;
    return toks_[i < toks_.size() ? i : toks_.size() - 1];
  }
  token_type peek_ty(std::size_t k = 0) const { return peek(k).type; }
  token advance() {
    token t = peek();
    if (idx_ + 1 < toks_.size())
      ++idx_;
    return t;
  }
  bool accept(token_type t) {
    if (peek_ty() == t) {
      advance();
      return true;
    }
    return false;
  }
  token expect(token_type t) {
    if (peek_ty() != t)
      fail("unexpected token");
    return advance();
  }

  // ---- binding powers ---------------------------------------------------
  enum : int {
    bp_aggreg = 2,
    bp_let = 3,
    bp_temporal_bin = 4,
    bp_temporal_un = 5,
    bp_quant = 6,
    bp_equiv = 7,
    bp_impl = 8,
    bp_or = 9,
    bp_and = 10,
    bp_not = 12,
    bp_base = 13,
  };

  struct infix_info {
    int bp;
    bool right_assoc;
  };

  static std::optional<infix_info> formula_infix(token_type t) {
    switch (t) {
      case token_type::EQUIV: return infix_info{bp_equiv, false};
      case token_type::IMPL: return infix_info{bp_impl, true};
      case token_type::OR: return infix_info{bp_or, false};
      case token_type::AND: return infix_info{bp_and, false};
      case token_type::SINCE:
      case token_type::TRIGGER:
      case token_type::UNTIL:
      case token_type::RELEASE: return infix_info{bp_temporal_bin, true};
      default: return std::nullopt;
    }
  }

  // ---- formulas ---------------------------------------------------------
  formula parse_formula(int min_bp) {
    formula lhs = parse_formula_prefix();
    for (;;) {
      auto inf = formula_infix(peek_ty());
      if (!inf)
        break;
      if (inf->bp < min_bp || (inf->bp == min_bp && !inf->right_assoc))
        break;
      token op = advance();
      if (op.type == token_type::SINCE || op.type == token_type::TRIGGER ||
          op.type == token_type::UNTIL || op.type == token_type::RELEASE) {
        interval intv = parse_optional_interval();
        formula rhs = parse_formula(inf->bp);
        temporal_binop bop = op.type == token_type::SINCE ? temporal_binop::since
          : op.type == token_type::TRIGGER                ? temporal_binop::trigger
          : op.type == token_type::UNTIL                  ? temporal_binop::until
                                                          : temporal_binop::release;
        lhs = formula{fo_temporal_bin{bop, intv, mk_formula(std::move(lhs)),
                                      mk_formula(std::move(rhs))}};
      } else {
        formula rhs = parse_formula(inf->bp);
        prop_binop bop = op.type == token_type::EQUIV ? prop_binop::equiv
          : op.type == token_type::IMPL               ? prop_binop::implies
          : op.type == token_type::OR                 ? prop_binop::or_
                                                      : prop_binop::and_;
        lhs = formula{fo_prop{bop, mk_formula(std::move(lhs)),
                              mk_formula(std::move(rhs))}};
      }
    }
    return lhs;
  }

  formula parse_formula_prefix() {
    switch (peek_ty()) {
      case token_type::TRUE:
        advance();
        return equal_cst(0, 0);
      case token_type::FALSE:
        advance();
        return equal_cst(0, 1);
      case token_type::NOT: {
        advance();
        formula body = parse_formula(bp_not);
        return formula{fo_neg{mk_formula(std::move(body))}};
      }
      case token_type::EX:
      case token_type::FA: {
        bool universal = advance().type == token_type::FA;
        std::vector<std::string> vars = parse_var_list_allow_empty();
        expect(token_type::DOT);
        if (vars.empty())
          fail(universal ? "forall: no variables" : "exists: no variables");
        formula body = parse_formula(bp_quant);
        return formula{
          fo_quant{universal, std::move(vars), mk_formula(std::move(body))}};
      }
      case token_type::LET:
      case token_type::LETPAST:
      case token_type::FRZ: {
        token_type k = advance().type;
        formula head = parse_predicate();
        auto *pred = std::get_if<fo_pred>(&head.node);
        if (!pred)
          fail("expected predicate");  // add_ex produced Exists
        expect(token_type::EQ);
        formula bound = parse_formula(bp_let);
        expect(token_type::IN);
        formula body = parse_formula(bp_let);
        let_kind kind = k == token_type::LET ? let_kind::let
          : k == token_type::LETPAST         ? let_kind::let_past
                                             : let_kind::frz;
        return formula{fo_let{kind, std::move(*pred),
                              mk_formula(std::move(bound)),
                              mk_formula(std::move(body))}};
      }
      case token_type::PREV:
      case token_type::NEXT:
      case token_type::EVENTUALLY:
      case token_type::ONCE:
      case token_type::ALWAYS:
      case token_type::PAST_ALWAYS: {
        token_type k = advance().type;
        interval intv = parse_optional_interval();
        formula body = parse_formula(bp_temporal_un);
        temporal_unop op = k == token_type::PREV ? temporal_unop::prev
          : k == token_type::NEXT                ? temporal_unop::next
          : k == token_type::EVENTUALLY          ? temporal_unop::eventually
          : k == token_type::ONCE                ? temporal_unop::once
          : k == token_type::ALWAYS              ? temporal_unop::always
                                                 : temporal_unop::past_always;
        return formula{fo_temporal_un{op, intv, mk_formula(std::move(body))}};
      }
      case token_type::FREX:
      case token_type::PREX: {
        bool future = advance().type == token_type::FREX;
        interval intv = parse_optional_interval();
        regex re = parse_regex_alt(future);
        return formula{fo_regex{future, intv, mk_regex(std::move(re))}};
      }
      case token_type::STR: {
        if (peek_ty(1) == token_type::LARROW)
          return parse_aggregation();
        if (peek_ty(1) == token_type::LPA)
          return parse_predicate();
        return parse_term_comparison();
      }
      case token_type::LPA: {
        // Speculate: full term expression followed by a comparison operator
        // (covers `(x) = 5`, `(x)+1 < y`); otherwise a parenthesized formula.
        std::size_t save = idx_;
        std::size_t save_cnt = fresh_var_cnt_;
        try {
          return parse_term_comparison();
        } catch (error_exc &) {
          idx_ = save;
          fresh_var_cnt_ = save_cnt;
        }
        expect(token_type::LPA);
        formula f = parse_formula(0);
        expect(token_type::RPA);
        return f;
      }
      default:
        return parse_term_comparison();
    }
  }

  formula equal_cst(std::int64_t a, std::int64_t b) {
    term l{term_cst{cst_int{big_int::normalize(std::to_string(a))}}};
    term r{term_cst{cst_int{big_int::normalize(std::to_string(b))}}};
    return formula{fo_cmp{cmp_op::equal, std::move(l), std::move(r)}};
  }

  // term <cmp> term (nonassoc: a following comparison token is a syntax
  // error, surfaced by the caller/top level as unexpected token).
  formula parse_term_comparison() {
    term l = parse_term(0);
    switch (peek_ty()) {
      case token_type::EQ:
        advance();
        return formula{fo_cmp{cmp_op::equal, std::move(l), parse_term(0)}};
      case token_type::LESSEQ:
        advance();
        return formula{fo_cmp{cmp_op::less_eq, std::move(l), parse_term(0)}};
      case token_type::LESS:
        advance();
        return formula{fo_cmp{cmp_op::less, std::move(l), parse_term(0)}};
      case token_type::GTR: {
        advance();
        term r = parse_term(0);
        return formula{fo_cmp{cmp_op::less, std::move(r), std::move(l)}};
      }
      case token_type::GTREQ: {
        advance();
        term r = parse_term(0);
        return formula{fo_cmp{cmp_op::less_eq, std::move(r), std::move(l)}};
      }
      case token_type::SUBSTRING:
        advance();
        return formula{fo_cmp{cmp_op::substring, std::move(l), parse_term(0)}};
      case token_type::MATCHES: {
        advance();
        term r = parse_term(0);
        std::vector<std::optional<term>> opts;
        if (peek_ty() == token_type::LPA) {
          advance();
          if (!accept(token_type::RPA)) {
            for (;;) {
              if (accept(token_type::LD))
                opts.emplace_back(std::nullopt);
              else
                opts.emplace_back(parse_term(0));
              if (accept(token_type::RPA))
                break;
              expect(token_type::COM);
            }
          }
        }
        return formula{fo_matches{std::move(l), std::move(r), std::move(opts)}};
      }
      default:
        fail("expected comparison operator after term");
    }
  }

  // predicate := STR ( termlist ) with monpoly's add_ex: arguments that are
  // variables starting with '_' (including the fresh vars minted for the
  // anonymous `_`) are existentially quantified, in argument order, with
  // duplicates kept.
  formula parse_predicate() {
    token name = expect(token_type::STR);
    expect(token_type::LPA);
    std::vector<term> args;
    // termlist := termarg COM termlist | termarg | ε  — the ε tail makes a
    // single trailing comma legal: P(x,) parses as P(x).
    while (!accept(token_type::RPA)) {
      if (accept(token_type::LD)) {
        ++fresh_var_cnt_;
        args.push_back(term{term_var{"_" + std::to_string(fresh_var_cnt_)}});
      } else {
        args.push_back(parse_term(0));
      }
      if (!accept(token_type::COM)) {
        expect(token_type::RPA);
        break;
      }
    }
    std::vector<std::string> ex_vars;
    for (const auto &a : args)
      if (const auto *v = std::get_if<term_var>(&a.node))
        if (!v->name.empty() && v->name.front() == '_')
          ex_vars.push_back(v->name);
    formula pred{fo_pred{std::move(name.value_str), std::move(args)}};
    if (ex_vars.empty())
      return pred;
    return formula{
      fo_quant{false, std::move(ex_vars), mk_formula(std::move(pred))}};
  }

  // var <- agg var [; varlist] formula   (%prec AGGREG)
  formula parse_aggregation() {
    token res = expect(token_type::STR);
    expect(token_type::LARROW);
    agg_op op;
    switch (peek_ty()) {
      case token_type::CNT: op = agg_op::cnt; break;
      case token_type::MIN: op = agg_op::min; break;
      case token_type::MAX: op = agg_op::max; break;
      case token_type::SUM: op = agg_op::sum; break;
      case token_type::AVG: op = agg_op::avg; break;
      case token_type::MED: op = agg_op::med; break;
      default: fail("expected aggregation operator");
    }
    advance();
    token agg_var = expect(token_type::STR);
    std::vector<std::string> group_by;
    if (accept(token_type::SC)) {
      // Greedy: STR (COM STR)*; the last var not followed by COM ends the
      // list (yacc shifts vars into the group-by list; the formula follows).
      while (peek_ty() == token_type::STR) {
        group_by.push_back(advance().value_str);
        if (!accept(token_type::COM))
          break;
      }
    }
    formula body = parse_formula(bp_aggreg);
    // monpoly checks agg/group-by vars are free in the body at parse time;
    // free-variable computation is a semantic check we replicate later, not
    // part of AST construction. (Differential harness: category `ok` differs
    // only for formulas violating this check; see aggreg() in the .mly.)
    check_aggreg_vars(agg_var.value_str, group_by, body);
    return formula{fo_agg{std::move(res.value_str), op,
                          std::move(agg_var.value_str), std::move(group_by),
                          mk_formula(std::move(body))}};
  }

  // ---- free variables (for the parse-time aggregation check) ------------
  static void collect_term_vars(const term &t, std::vector<std::string> &out) {
    std::visit(
      [&](const auto &v) {
        using T = std::remove_cvref_t<decltype(v)>;
        if constexpr (std::is_same_v<T, term_var>)
          out.push_back(v.name);
        else if constexpr (std::is_same_v<T, term_unary>)
          collect_term_vars(*v.arg, out);
        else if constexpr (std::is_same_v<T, term_binary>) {
          collect_term_vars(*v.l, out);
          collect_term_vars(*v.r, out);
        }
      },
      t.node);
  }

  static void free_vars(const formula &f, std::vector<std::string> &out);

  static void free_vars_regex(const regex &r, std::vector<std::string> &out) {
    std::visit(
      [&](const auto &v) {
        using T = std::remove_cvref_t<decltype(v)>;
        if constexpr (std::is_same_v<T, re_test>)
          free_vars(*v.f, out);
        else if constexpr (std::is_same_v<T, re_concat> ||
                           std::is_same_v<T, re_plus>) {
          free_vars_regex(*v.l, out);
          free_vars_regex(*v.r, out);
        } else if constexpr (std::is_same_v<T, re_star>) {
          free_vars_regex(*v.arg, out);
        }
      },
      r.node);
  }

  void check_aggreg_vars(const std::string &agg_var,
                         const std::vector<std::string> &group_by,
                         const formula &body) {
    std::vector<std::string> fv;
    free_vars(body, fv);
    auto is_free = [&](const std::string &v) {
      return std::find(fv.begin(), fv.end(), v) != fv.end();
    };
    if (!is_free(agg_var))
      fail("aggregation variable not free in the aggregated formula");
    for (const auto &v : group_by)
      if (!is_free(v))
        fail("group-by variable not free in the aggregated formula");
  }

  // ---- terms -------------------------------------------------------------
  enum : int { bp_add = 17, bp_mul = 18, bp_uminus = 19 };

  static std::optional<std::pair<term_binop, int>> term_infix(token_type t) {
    switch (t) {
      case token_type::PLUS: return std::pair{term_binop::plus, (int)bp_add};
      case token_type::MINUS: return std::pair{term_binop::minus, (int)bp_add};
      case token_type::STAR: return std::pair{term_binop::mult, (int)bp_mul};
      case token_type::SLASH: return std::pair{term_binop::div, (int)bp_mul};
      case token_type::MOD: return std::pair{term_binop::mod, (int)bp_mul};
      default: return std::nullopt;
    }
  }

  static std::optional<term_unop> term_function(token_type t) {
    switch (t) {
      case token_type::F2I: return term_unop::f2i;
      case token_type::I2F: return term_unop::i2f;
      case token_type::I2S: return term_unop::i2s;
      case token_type::S2I: return term_unop::s2i;
      case token_type::F2S: return term_unop::f2s;
      case token_type::S2F: return term_unop::s2f;
      case token_type::DAY_OF_MONTH: return term_unop::day_of_month;
      case token_type::MONTH: return term_unop::month;
      case token_type::YEAR: return term_unop::year;
      case token_type::FORMAT_DATE: return term_unop::format_date;
      case token_type::R2S: return term_unop::r2s;
      case token_type::S2R: return term_unop::s2r;
      default: return std::nullopt;
    }
  }

  term parse_term(int min_bp) {
    term lhs = parse_term_atom();
    for (;;) {
      auto inf = term_infix(peek_ty());
      if (!inf || inf->second < min_bp ||
          (inf->second == min_bp /* all left-assoc */))
        break;
      advance();
      term rhs = parse_term(inf->second);
      lhs = term{term_binary{inf->first, mk_term(std::move(lhs)),
                             mk_term(std::move(rhs))}};
    }
    return lhs;
  }

  term parse_term_atom() {
    if (auto fn = term_function(peek_ty())) {
      advance();
      expect(token_type::LPA);
      term arg = parse_term(0);
      expect(token_type::RPA);
      return term{term_unary{*fn, mk_term(std::move(arg))}};
    }
    switch (peek_ty()) {
      case token_type::MINUS: {
        advance();
        term arg = parse_term(bp_uminus);
        return term{term_unary{term_unop::uminus, mk_term(std::move(arg))}};
      }
      case token_type::LPA: {
        advance();
        term t = parse_term(0);
        expect(token_type::RPA);
        return t;
      }
      case token_type::INT: {
        token t = advance();
        return term{term_cst{cst_int{std::move(t.value_int)}}};
      }
      case token_type::RAT: {
        token t = advance();
        return term{term_cst{cst_float{t.value_float}}};
      }
      case token_type::STR_CST: {
        token t = advance();
        return term{term_cst{cst_str{std::move(t.value_str)}}};
      }
      case token_type::REGEXP_CST: {
        token t = advance();
        return term{term_cst{cst_regexp{std::move(t.value_str)}}};
      }
      case token_type::STR: {
        token t = advance();
        return term{term_var{std::move(t.value_str)}};
      }
      default:
        fail("expected term");
    }
  }

  // ---- intervals ---------------------------------------------------------
  bool looks_like_interval() const {
    if (peek_ty() == token_type::LSB)
      return true;
    if (peek_ty() != token_type::LPA)
      return false;
    token_type u = peek_ty(1);
    return (u == token_type::INT || u == token_type::TU) &&
           peek_ty(2) == token_type::COM;
  }

  interval parse_optional_interval() {
    if (!looks_like_interval())
      return default_interval();
    bool lower_closed = advance().type == token_type::LSB;
    big_int lo = parse_units();
    expect(token_type::COM);
    interval_bound upper;
    if (peek_ty() == token_type::STAR) {
      advance();
      if (!accept(token_type::RPA) && !accept(token_type::RSB))
        fail("expected ) or ] after *");
      upper = bnd_inf{};
    } else {
      big_int hi = parse_units();
      if (accept(token_type::RPA))
        upper = bnd_open{std::move(hi)};
      else if (accept(token_type::RSB))
        upper = bnd_closed{std::move(hi)};
      else
        fail("expected ) or ] after interval bound");
    }
    interval_bound lower = lower_closed
                             ? interval_bound{bnd_closed{std::move(lo)}}
                             : interval_bound{bnd_open{std::move(lo)}};
    return interval{std::move(lower), std::move(upper)};
  }

  // units := TU | INT. Time units go through OCaml machine-int arithmetic in
  // monpoly (Scanf %d, then d*n with silent 63-bit wraparound) — replicated.
  big_int parse_units() {
    if (peek_ty() == token_type::INT)
      return advance().value_int;
    if (peek_ty() != token_type::TU)
      fail("expected interval bound");
    token t = advance();
    std::int64_t mult;
    switch (t.value_char) {
      case 'd': mult = 24 * 60 * 60; break;
      case 'h': mult = 60 * 60; break;
      case 'm': mult = 60; break;
      case 's': mult = 1; break;
      default: fail("unrecognized time unit");
    }
    // Scanf %d overflows to failure for counts beyond OCaml's int; the
    // multiplication then wraps in 63-bit arithmetic.
    errno = 0;
    char *end = nullptr;
    long long n = std::strtoll(t.value_str.c_str(), &end, 10);
    constexpr long long ocaml_max = (1LL << 62) - 1;
    if (errno != 0 || n > ocaml_max)
      fail("time unit count out of range");
    unsigned long long wrapped =
      static_cast<unsigned long long>(n) * static_cast<unsigned long long>(mult);
    // reinterpret modulo 2^63 as signed 63-bit (OCaml native int)
    std::int64_t v = static_cast<std::int64_t>(wrapped << 1) >> 1;
    return big_int::normalize(std::to_string(v));
  }

  // ---- quantifier variable lists ------------------------------------------
  std::vector<std::string> parse_var_list_allow_empty() {
    std::vector<std::string> vars;
    if (peek_ty() != token_type::STR)
      return vars;  // empty list; caller rejects after consuming DOT
    vars.push_back(advance().value_str);
    while (accept(token_type::COM))
      vars.push_back(expect(token_type::STR).value_str);
    return vars;
  }

  // ---- regexes -------------------------------------------------------------
  static bool can_start_formula(token_type t) {
    switch (t) {
      case token_type::LPA:
      case token_type::FALSE:
      case token_type::TRUE:
      case token_type::STR:
      case token_type::STR_CST:
      case token_type::REGEXP_CST:
      case token_type::INT:
      case token_type::RAT:
      case token_type::MINUS:
      case token_type::NOT:
      case token_type::EX:
      case token_type::FA:
      case token_type::LET:
      case token_type::LETPAST:
      case token_type::FRZ:
      case token_type::PREV:
      case token_type::NEXT:
      case token_type::EVENTUALLY:
      case token_type::ONCE:
      case token_type::ALWAYS:
      case token_type::PAST_ALWAYS:
      case token_type::FREX:
      case token_type::PREX:
      case token_type::F2I:
      case token_type::I2F:
      case token_type::I2S:
      case token_type::S2I:
      case token_type::F2S:
      case token_type::S2F:
      case token_type::DAY_OF_MONTH:
      case token_type::MONTH:
      case token_type::YEAR:
      case token_type::FORMAT_DATE:
      case token_type::R2S:
      case token_type::S2R:
        return true;
      default:
        return false;
    }
  }

  static bool can_start_regex_atom(token_type t) {
    return t == token_type::DOT || can_start_formula(t);
  }

  regex parse_regex_alt(bool future) {
    regex l = parse_regex_concat(future);
    if (accept(token_type::PLUS)) {
      regex r = parse_regex_alt(future);  // right-assoc
      return regex{re_plus{mk_regex(std::move(l)), mk_regex(std::move(r))}};
    }
    return l;
  }

  regex parse_regex_concat(bool future) {
    regex l = parse_regex_atom(future);
    if (can_start_regex_atom(peek_ty())) {
      regex r = parse_regex_concat(future);  // right-assoc, greedy (shift)
      return regex{re_concat{mk_regex(std::move(l)), mk_regex(std::move(r))}};
    }
    return l;
  }

  regex parse_regex_atom(bool future) {
    regex atom = parse_regex_atom_base(future);
    while (accept(token_type::STAR))
      atom = regex{re_star{mk_regex(std::move(atom))}};
    return atom;
  }

  regex parse_regex_atom_base(bool future) {
    if (accept(token_type::DOT))
      return regex{re_skip{1}};
    if (peek_ty() == token_type::LPA) {
      // Speculate a parenthesized regex; fall back to a formula atom (which
      // itself covers `(x) = 5`-style parenthesized terms).
      std::size_t save = idx_;
      std::size_t save_cnt = fresh_var_cnt_;
      try {
        advance();
        regex r = parse_regex_alt(future);
        expect(token_type::RPA);
        return r;
      } catch (error_exc &) {
        idx_ = save;
        fresh_var_cnt_ = save_cnt;
      }
    }
    formula f = parse_formula(bp_base);
    if (accept(token_type::QM))
      return regex{re_test{mk_formula(std::move(f))}};
    regex test{re_test{mk_formula(std::move(f))}};
    regex wild{re_skip{1}};
    if (future)
      return regex{re_concat{mk_regex(std::move(test)), mk_regex(std::move(wild))}};
    return regex{re_concat{mk_regex(std::move(wild)), mk_regex(std::move(test))}};
  }

  std::vector<token> toks_;
  std::size_t idx_ = 0;
  std::size_t fresh_var_cnt_ = 0;
};

inline void formula_parser::free_vars(const formula &f,
                                      std::vector<std::string> &out) {
  std::visit(
    [&](const auto &v) {
      using T = std::remove_cvref_t<decltype(v)>;
      if constexpr (std::is_same_v<T, fo_cmp>) {
        collect_term_vars(v.l, out);
        collect_term_vars(v.r, out);
      } else if constexpr (std::is_same_v<T, fo_matches>) {
        collect_term_vars(v.l, out);
        collect_term_vars(v.r, out);
        for (const auto &o : v.opts)
          if (o)
            collect_term_vars(*o, out);
      } else if constexpr (std::is_same_v<T, fo_pred>) {
        // monpoly pvars: only arguments that ARE variables (no descent into
        // compound terms), first occurrence only.
        for (const auto &a : v.args)
          if (const auto *var = std::get_if<term_var>(&a.node))
            if (std::find(out.begin(), out.end(), var->name) == out.end())
              out.push_back(var->name);
      } else if constexpr (std::is_same_v<T, fo_let>) {
        free_vars(*v.body, out);
      } else if constexpr (std::is_same_v<T, fo_neg>) {
        free_vars(*v.arg, out);
      } else if constexpr (std::is_same_v<T, fo_prop>) {
        free_vars(*v.l, out);
        free_vars(*v.r, out);
      } else if constexpr (std::is_same_v<T, fo_quant>) {
        std::vector<std::string> inner;
        free_vars(*v.body, inner);
        for (auto &x : inner)
          if (std::find(v.vars.begin(), v.vars.end(), x) == v.vars.end())
            out.push_back(std::move(x));
      } else if constexpr (std::is_same_v<T, fo_agg>) {
        // monpoly: fv(Aggreg) = res_var :: group_by
        out.push_back(v.res_var);
        for (const auto &g : v.group_by)
          out.push_back(g);
      } else if constexpr (std::is_same_v<T, fo_temporal_un>) {
        free_vars(*v.body, out);
      } else if constexpr (std::is_same_v<T, fo_temporal_bin>) {
        free_vars(*v.l, out);
        free_vars(*v.r, out);
      } else if constexpr (std::is_same_v<T, fo_regex>) {
        free_vars_regex(*v.re, out);
      }
    },
    f.node);
}

}  // namespace staticmon::parser
