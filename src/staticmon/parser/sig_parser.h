#pragma once
// Signature (.sig) parser, functionally equivalent to monpoly's
// Log_parser.parse_signature over Log_lexer (docs/monpoly-grammar.md §4).
//
//   signature := ( ident ( arg , ... ) )*
//   arg       := type | name:type[:ignored...]      (split on ':')
//   type      := int | string | float | regexp
//
// Notes replicated from monpoly:
//   - the log lexer's identifier charset is (letter|digit|_ [ ] / : - . !)+
//     (different from the formula lexer); quoted and r-quoted strings both
//     yield their inner content (escapes verbatim, quotes/r stripped)
//   - the result keeps predicates in reverse declaration order and appends
//     the built-in base schema tp(i:int), ts(t:int), tpts(i:int,t:int)
//     (Db.add_predicate prepends to the base accumulator)
//   - duplicate predicate names (including redefining tp/ts/tpts) fail
//   - an empty argument token ("" from a quoted string) fails (Misc.nsplit
//     returns [] for the empty string)

#include <algorithm>
#include <optional>
#include <staticmon/parser/formula_ast.h>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace staticmon::parser {

enum class sig_type { t_int, t_str, t_float, t_regexp };

struct sig_attr {
  std::string name;  // may be empty (bare "int" style args)
  sig_type type;
};

struct sig_pred {
  std::string name;
  std::vector<sig_attr> attrs;
};

// Reverse declaration order; base schema (tp, ts, tpts) at the end.
using signature = std::vector<sig_pred>;

struct sig_error {
  std::size_t pos;
  std::string message;
};

namespace sig_detail {

  enum class tok { str, lpa, rpa, com, other, eof };

  struct sig_token {
    tok type;
    std::size_t pos;
    std::string text;
  };

  inline bool ident_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '[' || c == ']' ||
           c == '/' || c == ':' || c == '-' || c == '.' || c == '!';
  }

  class sig_lexer {
  public:
    explicit sig_lexer(std::string_view in) : in_(in) {}

    sig_token next() {
      for (;;) {
        while (pos_ < in_.size() &&
               (in_[pos_] == ' ' || in_[pos_] == '\t' || in_[pos_] == '\n' ||
                in_[pos_] == '\r'))
          ++pos_;
        if (pos_ < in_.size() && in_[pos_] == '#') {
          while (pos_ < in_.size() && in_[pos_] != '\n' && in_[pos_] != '\r')
            ++pos_;
          continue;
        }
        break;
      }
      if (pos_ >= in_.size())
        return {tok::eof, pos_, {}};
      std::size_t start = pos_;
      char c = in_[pos_];
      switch (c) {
        case '(': ++pos_; return {tok::lpa, start, "("};
        case ')': ++pos_; return {tok::rpa, start, ")"};
        case ',': ++pos_; return {tok::com, start, ","};
        // tokens the signature grammar never accepts; lexed, then rejected
        case '@':
        case '>':
        case '<':
        case '{':
        case '}':
        case ';': ++pos_; return {tok::other, start, std::string(1, c)};
        default: break;
      }
      // longest match among: identifier, quoted string, r-quoted string
      std::size_t id_len = 0;
      while (start + id_len < in_.size() && ident_char(in_[start + id_len]))
        ++id_len;
      auto quoted = [&](std::size_t at) -> std::size_t {
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
      };
      std::size_t q_len = quoted(start);
      std::size_t rq_len = 0;
      if (c == 'r')
        if (std::size_t q = quoted(start + 1); q > 0)
          rq_len = q + 1;
      // pick the longest (ocamllex); rule order breaks ties in mll order:
      // string, quoted, r-quoted — identical lengths cannot collide here
      // except r-quoted vs identifier starting with 'r' (r-quoted wins ties
      // is unreachable: '"' is not an identifier char in the log lexer).
      std::size_t best = std::max({id_len, q_len, rq_len});
      if (best == 0) {
        ++pos_;  // Log_lexer: catch-all ERR token
        return {tok::other, start, std::string(1, c)};
      }
      pos_ = start + best;
      if (best == rq_len)
        return {tok::str, start, std::string(in_.substr(start + 2, best - 3))};
      if (best == q_len)
        return {tok::str, start, std::string(in_.substr(start + 1, best - 2))};
      return {tok::str, start, std::string(in_.substr(start, best))};
    }

  private:
    std::string_view in_;
    std::size_t pos_ = 0;
  };

}  // namespace sig_detail

class sig_parser {
public:
  static std::variant<signature, sig_error> parse(std::string_view input) {
    sig_parser p(input);
    return p.run();
  }

private:
  explicit sig_parser(std::string_view in) : lex_(in) { advance(); }

  void advance() { cur_ = lex_.next(); }

  std::variant<signature, sig_error> run() {
    // base schema is the initial accumulator; every predicate is prepended
    signature schema = {
      {"tp", {{"i", sig_type::t_int}}},
      {"ts", {{"t", sig_type::t_int}}},
      {"tpts", {{"i", sig_type::t_int}, {"t", sig_type::t_int}}},
    };
    for (;;) {
      if (cur_.type == sig_detail::tok::eof)
        return schema;
      if (cur_.type != sig_detail::tok::str)
        return sig_error{cur_.pos, "expected a predicate name"};
      sig_pred pred;
      pred.name = std::move(cur_.text);
      advance();
      auto args = parse_tuple();
      if (auto *err = std::get_if<sig_error>(&args))
        return *err;
      for (auto &raw : std::get<std::vector<std::string>>(args)) {
        auto attr = convert_type(raw);
        if (auto *err = std::get_if<sig_error>(&attr))
          return *err;
        pred.attrs.push_back(std::get<sig_attr>(std::move(attr)));
      }
      for (const auto &existing : schema)
        if (existing.name == pred.name)
          return sig_error{cur_.pos, "predicate defined more than once"};
      schema.insert(schema.begin(), std::move(pred));
    }
  }

  std::variant<std::vector<std::string>, sig_error> parse_tuple() {
    if (cur_.type != sig_detail::tok::lpa)
      return sig_error{cur_.pos, "expected '('"};
    advance();
    std::vector<std::string> out;
    if (cur_.type == sig_detail::tok::rpa) {
      advance();
      return out;
    }
    if (cur_.type != sig_detail::tok::str)
      return sig_error{cur_.pos, "expected a string or ')'"};
    out.push_back(std::move(cur_.text));
    advance();
    for (;;) {
      if (cur_.type == sig_detail::tok::com) {
        advance();
        if (cur_.type != sig_detail::tok::str)
          return sig_error{cur_.pos, "expected a string"};
        out.push_back(std::move(cur_.text));
        advance();
      } else if (cur_.type == sig_detail::tok::rpa) {
        advance();
        return out;
      } else {
        return sig_error{cur_.pos, "expected ',' or ')'"};
      }
    }
  }

  // Misc.nsplit on ':' then get_type; "" splits to nothing (failure),
  // "name:type:rest" ignores rest.
  std::variant<sig_attr, sig_error> convert_type(const std::string &raw) {
    if (raw.empty())
      return sig_error{cur_.pos, "empty attribute"};
    std::vector<std::string> parts;
    std::size_t start = 0;
    for (;;) {
      std::size_t colon = raw.find(':', start);
      if (colon == std::string::npos) {
        parts.push_back(raw.substr(start));
        break;
      }
      parts.push_back(raw.substr(start, colon - start));
      start = colon + 1;
    }
    std::string name = parts.size() == 1 ? std::string() : parts[0];
    const std::string &ty = parts.size() == 1 ? parts[0] : parts[1];
    if (ty == "int")
      return sig_attr{std::move(name), sig_type::t_int};
    if (ty == "string")
      return sig_attr{std::move(name), sig_type::t_str};
    if (ty == "float")
      return sig_attr{std::move(name), sig_type::t_float};
    if (ty == "regexp")
      return sig_attr{std::move(name), sig_type::t_regexp};
    return sig_error{cur_.pos, "unknown type"};
  }

  sig_detail::sig_lexer lex_;
  sig_detail::sig_token cur_;
};

}  // namespace staticmon::parser
