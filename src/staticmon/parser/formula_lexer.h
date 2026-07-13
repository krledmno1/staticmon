#pragma once
// Tokenizer for MFOTL formulas, replicating monpoly's formula_lexer.mll
// (see docs/monpoly-grammar.md §1). ocamllex semantics: longest match wins;
// equal lengths are broken by rule order in the .mll file. The quirks below
// are deliberate and covered by differential tests:
//   - identifiers may start with a digit and contain - / : ' "
//   - `-5` lexes as a negative integer, `x-5` as one identifier
//   - `123a` is a time unit, `123ab` an identifier
//   - quoted strings keep escape sequences verbatim
//   - `(*...*)` comments (non-nested), `#` line comments

#include <cstdint>
#include <optional>
#include <staticmon/parser/formula_ast.h>
#include <string>
#include <string_view>
#include <vector>

namespace staticmon::parser {

enum class token_type {
  // symbols / keywords
  PLUS,
  MINUS,
  DOT,
  STAR,
  SLASH,
  LPA,
  RPA,
  LSB,
  RSB,
  FREX,
  PREX,
  BAR,
  COM,
  SC,
  QM,
  LD,
  LARROW,
  LESSEQ,
  LESS,
  GTREQ,
  GTR,
  EQ,
  SUBSTRING,
  MATCHES,
  R2S,
  S2R,
  MOD,
  F2I,
  I2F,
  I2S,
  S2I,
  F2S,
  S2F,
  DAY_OF_MONTH,
  MONTH,
  YEAR,
  FORMAT_DATE,
  FALSE,
  TRUE,
  LET,
  LETPAST,
  FRZ,
  IN,
  NOT,
  AND,
  OR,
  IMPL,
  EQUIV,
  EX,
  FA,
  PREV,
  NEXT,
  EVENTUALLY,
  ONCE,
  ALWAYS,
  PAST_ALWAYS,
  SINCE,
  TRIGGER,
  UNTIL,
  RELEASE,
  CNT,
  MIN,
  MAX,
  SUM,
  AVG,
  MED,
  // valued
  TU,       // time unit: value_int = count, value_char = unit letter
  INT,      // value_str = canonical decimal (arbitrary precision)
  RAT,      // value_float
  STR_CST,  // value_str = content between quotes, escapes verbatim
  REGEXP_CST,  // value_str = content between quotes (after the leading r)
  STR,         // identifier, value_str
  END_OF_INPUT,
};

struct token {
  token_type type;
  std::size_t pos;  // byte offset of the lexeme start (for error messages)
  std::string value_str;
  big_int value_int;  // TU count / INT value
  char value_char = 0;
  double value_float = 0.0;
};

struct lex_error {
  std::size_t pos;
  std::string message;
};

namespace detail {

  inline bool is_digit(char c) { return c >= '0' && c <= '9'; }
  inline bool is_letter(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
  }
  // formula_lexer.mll: string = (letter|digit|_) (letter|digit|_|-|/|:|'|")*
  inline bool is_ident_start(char c) {
    return is_letter(c) || is_digit(c) || c == '_';
  }
  inline bool is_ident_cont(char c) {
    return is_ident_start(c) || c == '-' || c == '/' || c == ':' ||
           c == '\'' || c == '"';
  }

  // Fixed lexemes in .mll rule order (earlier entry wins length ties).
  // Multi-byte lexemes must come in this table in the same order as the
  // ocamllex rules; the scan below still applies longest-match first.
  struct fixed_tok {
    std::string_view lexeme;
    token_type type;
  };

  inline constexpr fixed_tok fixed_table[] = {
    {"+", token_type::PLUS},
    {"-", token_type::MINUS},
    {".", token_type::DOT},
    {"*", token_type::STAR},
    {"/", token_type::SLASH},
    {"(", token_type::LPA},
    {")", token_type::RPA},
    {"[", token_type::LSB},
    {"]", token_type::RSB},
    {"|>", token_type::FREX},
    {"\xE2\x96\xB7", token_type::FREX},  // ▷
    {"FORWARD", token_type::FREX},
    {"MATCHF", token_type::FREX},
    {"<|", token_type::PREX},
    {"\xE2\x97\x81", token_type::PREX},  // ◁
    {"BACKWARD", token_type::PREX},
    {"MATCHP", token_type::PREX},
    {"|", token_type::BAR},
    {",", token_type::COM},
    {";", token_type::SC},
    {"?", token_type::QM},
    {"_", token_type::LD},
    {"<-", token_type::LARROW},
    {"<=", token_type::LESSEQ},
    {"SUBSTRING", token_type::SUBSTRING},
    {"MATCHES", token_type::MATCHES},
    {"r2s", token_type::R2S},
    {"s2r", token_type::S2R},
    {"<", token_type::LESS},
    {">=", token_type::GTREQ},
    {">", token_type::GTR},
    {"=", token_type::EQ},
    {"MOD", token_type::MOD},
    {"f2i", token_type::F2I},
    {"i2f", token_type::I2F},
    {"i2s", token_type::I2S},
    {"s2i", token_type::S2I},
    {"f2s", token_type::F2S},
    {"s2f", token_type::S2F},
    {"DAY_OF_MONTH", token_type::DAY_OF_MONTH},
    {"MONTH", token_type::MONTH},
    {"YEAR", token_type::YEAR},
    {"FORMAT_DATE", token_type::FORMAT_DATE},
    {"FALSE", token_type::FALSE},
    {"TRUE", token_type::TRUE},
    {"LET", token_type::LET},
    {"LETPAST", token_type::LETPAST},
    {"FRZ", token_type::FRZ},
    {"IN", token_type::IN},
    {"NOT", token_type::NOT},
    {"AND", token_type::AND},
    {"OR", token_type::OR},
    {"IMPLIES", token_type::IMPL},
    {"EQUIV", token_type::EQUIV},
    {"EXISTS", token_type::EX},
    {"FORALL", token_type::FA},
    {"PREV", token_type::PREV},
    {"PREVIOUS", token_type::PREV},
    {"NEXT", token_type::NEXT},
    {"EVENTUALLY", token_type::EVENTUALLY},
    {"SOMETIMES", token_type::EVENTUALLY},
    {"ONCE", token_type::ONCE},
    {"ALWAYS", token_type::ALWAYS},
    {"PAST_ALWAYS", token_type::PAST_ALWAYS},
    {"HISTORICALLY", token_type::PAST_ALWAYS},
    {"SINCE", token_type::SINCE},
    {"TRIGGER", token_type::TRIGGER},
    {"UNTIL", token_type::UNTIL},
    {"RELEASE", token_type::RELEASE},
    {"CNT", token_type::CNT},
    {"MIN", token_type::MIN},
    {"MAX", token_type::MAX},
    {"SUM", token_type::SUM},
    {"AVG", token_type::AVG},
    {"MED", token_type::MED},
  };

  // Rule order of the valued lexeme classes in formula_lexer.mll; the fixed
  // table above corresponds to rule indices [0, n_fixed). Larger index =
  // later rule = loses length ties.
  enum class valued_class : int {
    tu = 0,      // line 145
    int_ = 1,    // line 146
    rat = 2,     // line 147
    str_cst = 3, // line 148
    regexp = 4,  // line 149
    ident = 5,   // line 150
  };

}  // namespace detail

class formula_lexer {
public:
  explicit formula_lexer(std::string_view input) : in_(input) {}

  // Tokenizes the whole input. Returns tokens ending with END_OF_INPUT, or a
  // lex_error (monpoly: unmatched input / unterminated comment raise).
  std::variant<std::vector<token>, lex_error> tokenize() {
    std::vector<token> out;
    for (;;) {
      if (!skip_ws_and_comments())
        return lex_error{pos_, "comment not ended"};
      if (pos_ >= in_.size()) {
        out.push_back(token{token_type::END_OF_INPUT, pos_, {}, {}, 0, 0.0});
        return out;
      }
      auto t = next_token();
      if (!t)
        return lex_error{pos_, "no matching token"};
      out.push_back(std::move(*t));
    }
  }

private:
  // Returns false on unterminated (* comment (a lexing failure in monpoly).
  bool skip_ws_and_comments() {
    for (;;) {
      while (pos_ < in_.size() && (in_[pos_] == ' ' || in_[pos_] == '\t' ||
                                   in_[pos_] == '\n' || in_[pos_] == '\r'))
        ++pos_;
      if (pos_ + 1 < in_.size() && in_[pos_] == '(' && in_[pos_ + 1] == '*') {
        std::size_t end = in_.find("*)", pos_ + 2);
        if (end == std::string_view::npos)
          return false;
        pos_ = end + 2;
        continue;
      }
      if (pos_ < in_.size() && in_[pos_] == '#') {
        std::size_t end = in_.find_first_of("\n\r", pos_ + 1);
        pos_ = (end == std::string_view::npos) ? in_.size() : end;
        continue;
      }
      return true;
    }
  }

  // Length of a quoted string starting at `at` (including both quotes), or 0.
  // quoted_string = '"' ([^ '"' '\\'] | '\\' _)* '"'
  std::size_t quoted_len(std::size_t at) const {
    if (at >= in_.size() || in_[at] != '"')
      return 0;
    std::size_t i = at + 1;
    while (i < in_.size()) {
      if (in_[i] == '"')
        return i - at + 1;
      if (in_[i] == '\\') {
        if (i + 1 >= in_.size())
          return 0;
        i += 2;
      } else {
        ++i;
      }
    }
    return 0;
  }

  std::optional<token> next_token() {
    using namespace detail;
    std::size_t best_len = 0;
    int best_rule = INT32_MAX;  // lower = earlier rule = wins ties
    token result{};
    result.pos = pos_;
    std::string_view rest = in_.substr(pos_);

    auto consider = [&](std::size_t len, int rule, token tok) {
      if (len == 0)
        return;
      if (len > best_len || (len == best_len && rule < best_rule)) {
        best_len = len;
        best_rule = rule;
        tok.pos = pos_;
        result = std::move(tok);
      }
    };

    // Fixed tokens (rules before the valued classes).
    for (std::size_t r = 0; r < std::size(fixed_table); ++r) {
      const auto &ft = fixed_table[r];
      if (rest.substr(0, ft.lexeme.size()) == ft.lexeme)
        consider(ft.lexeme.size(), static_cast<int>(r),
                 token{ft.type, 0, {}, {}, 0, 0.0});
    }

    constexpr int n_fixed = static_cast<int>(std::size(fixed_table));
    auto valued_rule = [&](valued_class c) {
      return n_fixed + static_cast<int>(c);
    };

    // TU: digit+ letter
    {
      std::size_t i = 0;
      while (i < rest.size() && is_digit(rest[i]))
        ++i;
      if (i > 0 && i < rest.size() && is_letter(rest[i]))
        consider(i + 1, valued_rule(valued_class::tu),
                 token{token_type::TU, 0, std::string(rest.substr(0, i)),
                       big_int::normalize(rest.substr(0, i)), rest[i], 0.0});
    }

    // INT: -? digit+ ; RAT: -? digit+ . digit*
    {
      std::size_t i = 0;
      if (i < rest.size() && rest[i] == '-')
        ++i;
      std::size_t digits_start = i;
      while (i < rest.size() && is_digit(rest[i]))
        ++i;
      if (i > digits_start) {
        consider(i, valued_rule(valued_class::int_),
                 token{token_type::INT, 0, std::string(rest.substr(0, i)),
                       big_int::normalize(rest.substr(0, i)), 0, 0.0});
        if (i < rest.size() && rest[i] == '.') {
          std::size_t j = i + 1;
          while (j < rest.size() && is_digit(rest[j]))
            ++j;
          std::string text(rest.substr(0, j));
          consider(j, valued_rule(valued_class::rat),
                   token{token_type::RAT, 0, text, {}, 0, std::stod(text)});
        }
      }
    }

    // STR_CST: quoted string
    if (std::size_t ql = quoted_len(0 + pos_); ql > 0)
      consider(ql, valued_rule(valued_class::str_cst),
               token{token_type::STR_CST, 0,
                     std::string(rest.substr(1, ql - 2)), {}, 0, 0.0});

    // REGEXP_CST: 'r' quoted string
    if (!rest.empty() && rest[0] == 'r') {
      if (std::size_t ql = quoted_len(pos_ + 1); ql > 0)
        consider(ql + 1, valued_rule(valued_class::regexp),
                 token{token_type::REGEXP_CST, 0,
                       std::string(rest.substr(2, ql - 2)), {}, 0, 0.0});
    }

    // STR: identifier
    if (!rest.empty() && is_ident_start(rest[0])) {
      std::size_t i = 1;
      while (i < rest.size() && is_ident_cont(rest[i]))
        ++i;
      consider(i, valued_rule(valued_class::ident),
               token{token_type::STR, 0, std::string(rest.substr(0, i)),
                     {},
                     0,
                     0.0});
    }

    if (best_len == 0)
      return std::nullopt;
    pos_ += best_len;
    return result;
  }

  std::string_view in_;
  std::size_t pos_ = 0;
};

}  // namespace staticmon::parser
