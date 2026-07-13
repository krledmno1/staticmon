#pragma once
// clang-format off

#include <boost/mp11.hpp>
#include <staticmon/common/mp_helpers.h>
#include <staticmon/operators/operators.h>
#include <staticmon/common/table.h>
#include <string_view>

using namespace boost::mp11;
// formula_csts.h defines string constants as `"..."sv`; bring the string_view
// literal operator into scope.
using namespace std::literals;

#include <staticmon/input_formula/formula_csts.h>
#include <staticmon/input_formula/formula_in.h>
// clang-format on
