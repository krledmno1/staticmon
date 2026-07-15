// Native replacement for MonPoly's -explicitmon backend: parse a signature and a
// formula, then emit formula_in.h + formula_csts.h. Stages: parse (done) ->
// desugar -> translate -> codegen. Typing and monitorability checks are not
// yet wired in (see docs/explicitmon-pipeline.md); callers should feed
// monitorable, well-typed formulas for now.
//
// Usage:
//   staticmon-headers -sig S.sig -formula F.mfotl [-prefix DIR]
// With -prefix, writes DIR/formula_in.h and DIR/formula_csts.h; otherwise
// prints formula_in.h, a line "//---CSTS---", then formula_csts.h to stdout.

#include <fstream>
#include <iostream>
#include <sstream>
#include <staticmon/compile/desugar.h>
#include <staticmon/compile/emit_headers.h>
#include <staticmon/compile/monitorable.h>
#include <staticmon/compile/translate.h>
#include <staticmon/compile/typing.h>
#include <staticmon/parser/formula_parser.h>
#include <staticmon/parser/sig_parser.h>
#include <string>

using namespace staticmon;

static std::string read_file(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

static compile::val_type conv_type(parser::sig_type t) {
  switch (t) {
    case parser::sig_type::t_int: return compile::val_type::t_int;
    case parser::sig_type::t_str: return compile::val_type::t_str;
    case parser::sig_type::t_float: return compile::val_type::t_float;
    case parser::sig_type::t_regexp:
      throw std::runtime_error("regexp predicate types unsupported");
  }
  throw std::runtime_error("bad sig type");
}

static compile::tcst conv_tcst(parser::sig_type t) {
  switch (t) {
    case parser::sig_type::t_int: return compile::tcst::t_int;
    case parser::sig_type::t_str: return compile::tcst::t_str;
    case parser::sig_type::t_float: return compile::tcst::t_float;
    case parser::sig_type::t_regexp: return compile::tcst::t_regexp;
  }
  return compile::tcst::t_int;
}

static const char *tcst_name(compile::tcst t) {
  switch (t) {
    case compile::tcst::t_int: return "int";
    case compile::tcst::t_str: return "string";
    case compile::tcst::t_float: return "float";
    case compile::tcst::t_regexp: return "regexp";
  }
  return "?";
}

int main(int argc, char **argv) {
  std::string sig_path, formula_path, prefix;
  bool mode_check = false, mode_sigout = false, mode_verbose = false;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() { return (i + 1 < argc) ? argv[++i] : ""; };
    if (a == "-sig")
      sig_path = next();
    else if (a == "-formula")
      formula_path = next();
    else if (a == "-prefix" || a == "-explicitmon_prefix")
      prefix = next();
    else if (a == "-check")
      mode_check = true;
    else if (a == "-sigout")
      mode_sigout = true;
    else if (a == "-verbose")
      mode_verbose = true;
    else if (a == "-explicitmon")
      continue;
  }
  if (sig_path.empty() || formula_path.empty()) {
    std::cerr << "usage: staticmon-headers -sig S -formula F "
                 "[-prefix DIR | -check | -sigout | -verbose]\n";
    return 2;
  }

  auto sig_res = parser::sig_parser::parse(read_file(sig_path));
  if (auto *err = std::get_if<parser::sig_error>(&sig_res)) {
    std::cerr << "signature error: " << err->message << "\n";
    return 1;
  }
  const auto &sig = std::get<parser::signature>(sig_res);

  // schema_map (codegen: concrete val_type per slot) + typing signature.
  compile::schema_map schema;
  std::vector<std::pair<compile::type_checker::pred_key, std::vector<compile::tcst>>>
    type_sig;
  for (const auto &p : sig) {
    std::vector<compile::val_type> tys;
    std::vector<compile::tcst> ttys;
    bool has_regexp = false;
    for (const auto &a : p.attrs) {
      ttys.push_back(conv_tcst(a.type));
      if (a.type == parser::sig_type::t_regexp)
        has_regexp = true;
      else
        tys.push_back(conv_type(a.type));
    }
    if (!has_regexp)
      schema[p.name] = std::move(tys);
    type_sig.push_back({{p.name, p.attrs.size()}, std::move(ttys)});
  }

  std::string formula_text = read_file(formula_path);
  auto f_res = parser::formula_parser::parse(formula_text);
  if (auto *err = std::get_if<parser::parse_error>(&f_res)) {
    std::cerr << "formula parse error: " << err->message << " at " << err->pos
              << "\n";
    return 1;
  }
  const auto &formula = std::get<parser::formula>(f_res);
  auto orig_free = compile::free_vars(formula);

  // -verbose: the header monpoly -verbose prints before monitoring. Emitted
  // here (before typing/monitorability), so it is produced for any parsable
  // formula, as monpoly does. staticmon has no formula pretty-printer, so we
  // echo the input formula (trimmed) rather than a re-serialized AST; the free
  // variables are printed in the same order as -sigout.
  if (mode_verbose) {
    auto l = formula_text.find_first_not_of(" \t\r\n");
    auto r = formula_text.find_last_not_of(" \t\r\n");
    std::string trimmed =
      l == std::string::npos ? "" : formula_text.substr(l, r - l + 1);
    std::cout << "The analyzed formula is:\n  " << trimmed << "\n";
    std::cout << "The sequence of free variables is: (";
    for (std::size_t i = 0; i < orig_free.size(); ++i)
      std::cout << (i ? "," : "") << orig_free[i];
    std::cout << ")\n";
    return 0;
  }

  // Order matches monpoly's check_formula: check_wff, then type inference.
  // Well-formedness (intervals / bounded future / LET params).
  if (auto err = compile::check_wff(formula)) {
    std::cerr << "Fatal error: exception Failure(\"" << *err << "\")\n";
    return 1;
  }

  // Desugar first: type inference and translation both run on the desugared
  // formula, so LET-node pointers (which key the inferred parameter types)
  // are consistent between the two stages. Desugaring preserves variable
  // types, so -sigout parity is unaffected.
  auto desugared = compile::desugar(formula);

  // Stage 2: type inference (rejects ill-typed like monpoly).
  compile::type_checker tc(type_sig);
  std::vector<std::pair<std::string, compile::tcst>> var_types;
  try {
    var_types = tc.check(desugared, orig_free);
  } catch (const compile::type_error &e) {
    std::cerr << "Fatal error: exception Failure(\"" << e.message << "\")\n";
    return 1;
  }

  if (mode_sigout) {
    for (std::size_t i = 0; i < var_types.size(); ++i)
      std::cout << (i ? ", " : "") << var_types[i].first << ":"
                << tcst_name(var_types[i].second);
    std::cout << "\n";
    return 0;
  }

  // Stage 3: monitorability.
  auto mon = compile::is_monitorable(desugared);

  if (mode_check) {
    if (mon.ok)
      std::cout << "The analyzed formula is monitorable.\n";
    else
      std::cout << "The analyzed formula is NOT monitorable, because of a "
                   "subformula.\n"
                << mon.reason << "\n";
    return 0;
  }
  if (!mon.ok) {
    std::cerr << "The formula is NOT monitorable: " << mon.reason << "\n";
    return 1;
  }

  // LET/LETPAST parameter types (tcst -> val_type) for the translator.
  compile::let_param_types_map let_types;
  for (const auto &[node, tys] : tc.let_param_types()) {
    std::vector<compile::val_type> vts;
    for (compile::tcst t : tys) {
      switch (t) {
        case compile::tcst::t_int: vts.push_back(compile::val_type::t_int); break;
        case compile::tcst::t_str: vts.push_back(compile::val_type::t_str); break;
        case compile::tcst::t_float: vts.push_back(compile::val_type::t_float); break;
        case compile::tcst::t_regexp:
          std::cerr << "translation error: LET parameter of type regexp is "
                       "unsupported\n";
          return 1;
      }
    }
    let_types[node] = std::move(vts);
  }

  // Stages 4-5: translate + codegen.
  try {
    compile::translator tr(std::move(schema), std::move(let_types));
    auto translated = tr.run(desugared, orig_free);
    compile::header_emitter emitter;
    auto headers = emitter.emit(translated);
    if (!prefix.empty()) {
      std::ofstream(prefix + "/formula_in.h") << headers.formula_in;
      std::ofstream(prefix + "/formula_csts.h") << headers.formula_csts;
    } else {
      std::cout << headers.formula_in << "//---CSTS---\n"
                << headers.formula_csts;
    }
  } catch (const compile::translate_error &e) {
    std::cerr << "translation error: " << e.message << "\n";
    return 1;
  }
  return 0;
}
