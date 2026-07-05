#pragma once
#include <optional>
#include <string_view>

// Small string-scanning helpers used by the request parser. These are internal
// to the library and are not part of the installed public headers.

std::string_view::size_type findAnyOf(std::string_view haystack,
                                      std::string_view needles,
                                      std::string_view::size_type pos = 0);

bool advanceOver(std::string_view& str, std::string_view target);

std::optional<int> advanceOverInt(std::string_view& str);
