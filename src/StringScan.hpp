#pragma once
#include <cstdint>
#include <optional>
#include <string_view>

// Small string-scanning helpers used by the request parser. These are internal
// to the library and are not part of the installed public headers.

std::string_view::size_type findAnyOf(std::string_view haystack,
                                      std::string_view needles,
                                      std::string_view::size_type pos = 0);

bool advanceOver(std::string_view& str, std::string_view target);

// Reads a non-negative decimal index from the front of `str`, advancing past
// it. Nullopt, with `str` untouched, where `str` does not start with a digit
// or the index does not fit 32 bits.
std::optional<uint32_t> advanceOverIndex(std::string_view& str);
