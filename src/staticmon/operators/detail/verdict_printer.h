#pragma once
#include <algorithm>
#include <boost/mp11.hpp>
#include <cstdint>
#include <fmt/format.h>
#include <fmt/os.h>
#include <fmt/ranges.h>
#include <optional>
#include <staticmon/common/table.h>
#include <staticmon/common/util.h>
#include <staticmon/operators/detail/operator_types.h>
#include <string>

template<typename T>
class output_row_fmt {
  friend struct fmt::formatter<output_row_fmt<T>>;

public:
  explicit output_row_fmt(const T &t) : t(t){};

private:
  const T &t;
};

template<typename... Args>
struct fmt::formatter<output_row_fmt<std::tuple<Args...>>, char>
    : trivial_parser {
  template<typename FormatContext>
  auto format
    [[maybe_unused]] (const output_row_fmt<std::tuple<Args...>> &arg_wrapper,
                      FormatContext &ctx) -> decltype(auto) {
    const auto &arg = arg_wrapper.t;
    return boost::mp11::tuple_apply(
      [&ctx](auto &&...vals) {
        return fmt::format_to(
          ctx.out(), "({})",
          fmt::join(std::forward_as_tuple(output_row_fmt(vals)...), ","));
      },
      arg);
  }
};

template<>
struct fmt::formatter<output_row_fmt<std::int64_t>, char> : trivial_parser {
  template<typename FormatContext>
  auto format [[maybe_unused]] (const output_row_fmt<std::int64_t> &arg_wrapper,
                                FormatContext &ctx) const -> decltype(auto) {
    return fmt::format_to(ctx.out(), "{}", arg_wrapper.t);
  }
};

template<>
struct fmt::formatter<output_row_fmt<double>, char> : trivial_parser {
  template<typename FormatContext>
  auto format [[maybe_unused]] (const output_row_fmt<double> &arg_wrapper,
                                FormatContext &ctx) const -> decltype(auto) {
    return fmt::format_to(ctx.out(), "{:.5e}", arg_wrapper.t);
  }
};

template<>
struct fmt::formatter<output_row_fmt<std::string>, char> : trivial_parser {
  template<typename FormatContext>
  auto format [[maybe_unused]] (const output_row_fmt<std::string> &arg_wrapper,
                                FormatContext &ctx) const -> decltype(auto) {
    return fmt::format_to(ctx.out(), "\"{}\"", arg_wrapper.t);
  }
};

class verdict_printer {
public:
  explicit verdict_printer(std::optional<std::string> file_name) {
    if (file_name)
      ofile_.emplace(fmt::output_file(std::move(*file_name)));
  }

  template<typename... Args>
  void print_verdict(std::size_t ts, std::size_t tp,
                     std::vector<std::tuple<Args...>> &tbl) {
    if (tbl.empty())
      return;
    std::sort(tbl.begin(), tbl.end());
    print("@{} (time point {}):", ts, tp);
    if (tbl.size() == 1 && (sizeof...(Args) == 0)) {
      print(" true\n");
    } else {
      for (const auto &row : tbl)
        print(" {}", output_row_fmt(row));
      print("\n");
    }
  }

private:
  template<typename... Args>
  void print(fmt::format_string<Args...> fmt, Args &&...args) {
    if (ofile_)
      ofile_->print(fmt, std::forward<Args>(args)...);
    else
      fmt::print(fmt, std::forward<Args>(args)...);
  }
  std::optional<fmt::ostream> ofile_;
};
