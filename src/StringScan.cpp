#include "StringScan.hpp"

#include <charconv>
#include <system_error>

std::string_view::size_type findAnyOf(std::string_view haystack,
                                      std::string_view needles,
                                      std::string_view::size_type pos) {
  return haystack.find_first_of(needles, pos);
}

bool advanceOver(std::string_view& str, std::string_view target) {
  if (str.starts_with(target)) {
    str.remove_prefix(target.size());
    return true;
  }
  return false;
}

std::optional<int> advanceOverInt(std::string_view& str) {
  int index = 0;
  auto [ptr, error] =
      std::from_chars(str.data(), str.data() + str.size(), index);
  if (error == std::errc::invalid_argument
      or error == std::errc::result_out_of_range) {
    return std::nullopt;
  }
  str.remove_prefix(ptr - str.data());
  return index;
}
