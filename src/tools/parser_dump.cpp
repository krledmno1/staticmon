// CLI for differential-testing staticmon's standalone formula parser against
// the MonPoly oracle (test/parser_oracle). Same I/O protocol: formula files
// as arguments, or length-prefixed frames on stdin; one result line each:
//   (ok <sexp>)  |  (parse_error "...")
// Comparison matches (ok ...) lines exactly and parse_error lines by head.

#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <staticmon/parser/formula_parser.h>
#include <staticmon/parser/sexp_printer.h>
#include <staticmon/parser/sig_parser.h>
#include <string>

using namespace staticmon::parser;

// Same convention as the oracle: frames starting with the line "#SIG" are
// signatures (the marker is a comment in both syntaxes).
static constexpr std::string_view sig_marker = "#SIG\n";

static void print_schema(const signature &s) {
  std::string out = "(ok (Schema";
  for (const auto &p : s) {
    out += " (\"";
    detail::ocaml_escape_into(out, p.name);
    out += "\"";
    for (const auto &a : p.attrs) {
      out += " (\"";
      detail::ocaml_escape_into(out, a.name);
      out += "\" ";
      switch (a.type) {
        case sig_type::t_int: out += "TInt"; break;
        case sig_type::t_str: out += "TStr"; break;
        case sig_type::t_float: out += "TFloat"; break;
        case sig_type::t_regexp: out += "TRegexp"; break;
      }
      out += ")";
    }
    out += ")";
  }
  out += "))";
  std::cout << out << "\n";
}

static void run_one(const std::string &src) {
  if (src.size() >= sig_marker.size() &&
      std::string_view(src).substr(0, sig_marker.size()) == sig_marker) {
    auto res = sig_parser::parse(std::string_view(src).substr(sig_marker.size()));
    if (auto *s = std::get_if<signature>(&res)) {
      print_schema(*s);
    } else {
      auto &err = std::get<sig_error>(res);
      std::cout << sexp_printer::print_parse_error(
                     err.message + " at offset " + std::to_string(err.pos))
                << "\n";
    }
    return;
  }
  auto res = formula_parser::parse(src);
  if (auto *f = std::get_if<formula>(&res)) {
    sexp_printer printer;
    std::cout << printer.print_ok(*f) << "\n";
  } else {
    auto &err = std::get<parse_error>(res);
    std::cout << sexp_printer::print_parse_error(
                   err.message + " at offset " + std::to_string(err.pos))
              << "\n";
  }
}

int main(int argc, char **argv) {
  if (argc > 1) {
    for (int i = 1; i < argc; ++i) {
      std::ifstream in(argv[i], std::ios::binary);
      if (!in) {
        std::cout << "(driver_error \"cannot open file\")\n";
        continue;
      }
      std::ostringstream ss;
      ss << in.rdbuf();
      run_one(ss.str());
    }
    return 0;
  }
  // stdin frame protocol: decimal byte count on its own line, then the bytes.
  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.empty())
      continue;
    std::size_t n = std::stoul(line);
    std::string src(n, '\0');
    std::cin.read(src.data(), static_cast<std::streamsize>(n));
    if (std::cin.gcount() != static_cast<std::streamsize>(n))
      break;
    run_one(src);
  }
  return 0;
}
