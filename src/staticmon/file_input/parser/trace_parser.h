#pragma once
#include <absl/container/flat_hash_map.h>
#include <boost/serialization/strong_typedef.hpp>
#include <charconv>
#include <fmt/core.h>
#include <fmt/ranges.h>
#include <iterator>
#include <lexy/action/parse.hpp>
#include <lexy/action/scan.hpp>
#include <lexy/callback.hpp>
#include <lexy/dsl.hpp>
#include <lexy/input/string_input.hpp>
#include <lexy_ext/report_error.hpp>
#include <optional>
#include <staticmon/input_formula/formula.h>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace parse {
#define RULE static constexpr auto rule =
#define VALUE static constexpr auto value =

using parser_database = absl::flat_hash_map<pred_id_t, database_table>;

inline database monitor_db_from_parser_db(parser_database &&db) {
  database ret_db;
  ret_db.reserve(db.size());
  for (auto &entry : db) {
    if (entry.second.empty())
      continue;
    ret_db.emplace(entry.first, make_vector(std::move(entry.second)));
  }
  return ret_db;
}

namespace dsl = lexy::dsl;

struct parse_state {
  const pred_map_t &pred_map;
};

class trace_parser {
public:
  trace_parser() = default;
  timestamped_database parse_database(std::string_view db) {
    auto st = parse_state{input_predicates};
    auto input = lexy::string_input<lexy::ascii_encoding>(db);
    auto parsed_db =
      lexy::parse<trace_parser::ts_db_parse>(input, st, lexy_ext::report_error);
    if (!parsed_db || (parsed_db.error_count() > 0))
      throw std::runtime_error("failed to parse timestamped database");
    assert(parsed_db.has_value());
    return std::move(parsed_db).value();
  }

private:
  struct string_arg : lexy::token_production {
    using res_type = std::string;
    RULE dsl::quoted(dsl::code_point - dsl::ascii::control,
                     dsl::backslash_escape.capture(dsl::lit_c<'"'> /
                                                   dsl::lit_c<'\\'>));
    VALUE lexy::as_string<std::string>;
  };

  using arg_tup_ty = std::pair<pred_id_t, database_table>;

  struct unknown_pred {
    static constexpr auto name = "unknown predicate";
  };

  struct not_enough_args {
    static constexpr auto name =
      "predicate has wrong arity - not enough arguments";
  };

  struct too_many_args {
    static constexpr auto name =
      "predicate has wrong arity - too many arguments";
  };
  struct invalid_double {
    static constexpr auto name = "invalid double value";
  };
  struct invalid_integer {
    static constexpr auto name = "invalid integer value";
  };

  struct discard_arg : lexy::token_production {
    RULE dsl::peek(dsl::lit_c<'\"'>) >> dsl::p<string_arg> |
      dsl::else_
        >> dsl::while_(dsl::ascii::character - dsl::comma - dsl::lit_c<')'>);
    VALUE lexy::noop;  // discard: lexy >= 2025 needs a sink for list productions
  };

  struct discard_tuple {
    RULE dsl::parenthesized.opt_list(dsl::p<discard_arg>, dsl::sep(dsl::comma));
    VALUE lexy::noop;  // discard: lexy >= 2025 needs a sink for list productions
  };

  struct discard_tup_list {
    // Zero or more '(...)' tuples. The continuation must peek for a real '(':
    // peek_not(alpha/semicolon) also succeeds at the end of the time-point
    // chunk (nothing ahead), which made an unknown predicate's tuple at chunk
    // end demand yet another tuple -- classic MonPoly traces (no trailing ';',
    // events of undeclared predicates) end exactly like that.
    RULE dsl::opt(dsl::list(dsl::peek(dsl::lit_c<'('>) >> dsl::p<discard_tuple>));
    VALUE lexy::noop;  // discard: lexy >= 2025 needs a sink for list productions
  };

  struct pred_name {
    using res_type = std::string;
    RULE dsl::identifier(dsl::ascii::alpha_digit_underscore);
    VALUE lexy::as_string<res_type>;
  };

  struct named_arg_tup_list : lexy::scan_production<std::optional<arg_tup_ty>> {

    template<typename Context, typename Reader>
    static void parse_event(lexy::rule_scanner<Context, Reader> &scanner,
                            std::vector<event_data> &tup, arg_types ty) {
      if (ty == INT_TYPE || ty == FLOAT_TYPE) {
        // lexy >= 2025: capture() requires a token; wrap the while-loop.
        auto maybe_lexeme = scanner.capture(dsl::token(
          dsl::while_(dsl::ascii::digit / dsl::period / dsl::lit_c<'e'> /
                      dsl::lit_c<'E'> / dsl::lit_c<'-'>)));
        if (!scanner)
          return;
        auto lexeme = maybe_lexeme.value();
        if (ty == INT_TYPE) {
          const auto *fst = lexeme.data(), *lst = fst + lexeme.size();
          std::int64_t int_val;
          auto [new_ptr, ec] = std::from_chars(fst, lst, int_val);
          if (ec != std::errc()) {
            scanner.fatal_error(invalid_integer{},
                                scanner.position() - lexeme.size(),
                                scanner.position());
            return;
          }
          tup.emplace_back(int_val);
        } else {
          const auto *fst = lexeme.data(), *lst = fst + lexeme.size();
          double double_val;
          auto [new_ptr, ec] = std::from_chars(fst, lst, double_val);
          if (ec != std::errc()) {
            scanner.fatal_error(invalid_double{},
                                scanner.position() - lexeme.size(),
                                scanner.position());
            return;
          }
          tup.emplace_back(double_val);
        }
      } else {
        lexy::scan_result<std::string> string_val;
        scanner.parse(string_val, dsl::p<string_arg>);
        if (!scanner)
          return;
        tup.emplace_back(string_val.value());
      }
    }

    template<typename Context, typename Reader>
    static void parse_tuple(lexy::rule_scanner<Context, Reader> &scanner,
                            std::vector<event_data> &tup,
                            const std::vector<arg_types> &tys) {
      scanner.parse(dsl::lit_c<'('>);
      if (!scanner)
        return;
      if (scanner.branch(dsl::lit_c<')'>)) {
        if (!tys.empty())
          scanner.fatal_error(not_enough_args{}, scanner.begin(),
                              scanner.position());
        return;
      }

      auto it = tys.cbegin();
      do {
        if (!scanner)
          return;
        if (it == tys.cend()) {
          scanner.fatal_error(too_many_args{}, scanner.begin(),
                              scanner.position());
          return;
        }
        parse_event(scanner, tup, *it);
        if (!scanner)
          return;
        ++it;
      } while (scanner.branch(dsl::lit_c<','>));
      if (!scanner)
        return;
      if (it != tys.cend()) {
        scanner.fatal_error(not_enough_args{}, scanner.begin(),
                            scanner.position());
        return;
      }
      scanner.parse(dsl::lit_c<')'>);
    }

    template<typename Context, typename Reader>
    static scan_result scan(lexy::rule_scanner<Context, Reader> &scanner,
                            const parse_state &state) {
      lexy::scan_result<std::string> p_name_res;
      scanner.parse(p_name_res, dsl::p<pred_name>);
      if (!scanner)
        return lexy::scan_failed;
      // TODO: only const
      std::string p_name(p_name_res.value());
      auto p_it = state.pred_map.find(p_name);
      if (p_it == state.pred_map.end()) {
        scanner.parse(dsl::p<discard_tup_list>);
        if (!scanner)
          return lexy::scan_failed;
        return std::nullopt;
      }
      const auto &pinfo = p_it->second;
      const auto &ptypes = pinfo.second;
      std::size_t n_args = ptypes.size();
      std::size_t p_id = pinfo.first;
      // Parse this predicate's tuples. Each tuple starts with '(', so stop as
      // soon as the next token is not '(' -- at a new predicate name, ';', or
      // end of the time-point chunk (which may omit the trailing ';').
      database_table tup_list;
      while (scanner.branch(dsl::peek(dsl::lit_c<'('>))) {
        if (!scanner)
          return lexy::scan_failed;
        std::vector<event_data> tup;
        tup.reserve(n_args);
        parse_tuple(scanner, tup, ptypes);
        if (!scanner)
          return lexy::scan_failed;
        tup_list.emplace_back(std::move(tup));
      }
      if (!scanner)
        return lexy::scan_failed;
      return std::pair(p_id, std::move(tup_list));
    }
  };

  struct db_parse {
    RULE dsl::list(dsl::peek(dsl::ascii::alpha_digit_underscore) >>
                   dsl::p<named_arg_tup_list>);
    static constexpr auto fold_fn = [](parser_database &db,
                                       std::optional<arg_tup_ty> &&elem) {
      if (!elem)
        return;
      auto it = db.find(elem->first);
      if (it == db.end())
        db.insert(std::move(*elem));
      else
        it->second.insert(it->second.end(),
                          std::make_move_iterator(elem->second.begin()),
                          std::make_move_iterator(elem->second.end()));
    };
    VALUE
    lexy::fold_inplace<parser_database>(
      std::initializer_list<parser_database::init_type>{}, fold_fn);
  };

  struct ts_parse : lexy::token_production {
    RULE dsl::capture(dsl::digits<>);
    VALUE lexy::callback<std::size_t>([](auto lexeme) -> std::size_t {
      const auto *fst = lexeme.data(), *lst = fst + lexeme.size();
      std::size_t ts;
      auto [new_ptr, ec] = std::from_chars(fst, lst, ts);
      if (ec != std::errc() || new_ptr != lst)
        throw std::runtime_error("invalid integer");
      return ts;
    });
  };

  struct ts_db_parse {
    static constexpr auto whitespace = dsl::ascii::blank / dsl::ascii::newline;

    // `whitespace` above enables automatic whitespace skipping between tokens
    // (lexy >= 2025 removed the bare `dsl::whitespace` rule; it is now a
    // function, and the auto-skip makes an explicit rule redundant).
    // One time-point: `@ <ts> [<db>] [;]`. The trailing ';' is optional -- the
    // driver delimits time-points at top-level '@'/';', so a chunk may or may
    // not carry its ';'. The db is present iff a predicate name follows.
    RULE dsl::at_sign + dsl::p<ts_parse> +
      dsl::opt(dsl::peek(dsl::ascii::alpha_digit_underscore) >> dsl::p<db_parse>) +
      dsl::if_(dsl::semicolon) + dsl::eof;
    VALUE lexy::callback<timestamped_database>(
      [](std::size_t &&ts,
         std::optional<parser_database> &&db) -> timestamped_database {
        if (db)
          return std::make_pair(ts, monitor_db_from_parser_db(std::move(*db)));
        else
          return std::make_pair(ts, database());
      });
  };
#undef RULE
#undef VALUE
};
}// namespace parse
