#include "PayloadValue.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "flatbuffers/flatbuffers.h"
#include "flatbuffers/util.h"

using flatbuffers::GetTypeSize;
using flatbuffers::IsScalar;

namespace {

// The smallest magnitude a double rounds to infinity as a float32: halfway
// between the largest float32 and 2^128, a tie that rounds to the even
// neighbour, which is 2^128.
constexpr double kFloatRoundsToInfinity = 0x1.ffffffp127;

// How deeply maps and lists may nest in a payload, as the FlatBuffers parser
// bounds it, so that a hostile request cannot exhaust the stack.
constexpr int kMaxPayloadDepth = 64;

// Whether a flexbuffer integer is a value `Integer` holds.
template <typename Integer>
bool integerFits(flexbuffers::Reference value) {
  constexpr auto kMax =
      static_cast<uint64_t>(std::numeric_limits<Integer>::max());
  if (value.IsUInt()) {
    return value.AsUInt64() <= kMax;
  }
  if (not value.IsInt()) {
    return false;
  }
  const auto number = value.AsInt64();
  if (number < 0) {
    return std::numeric_limits<Integer>::is_signed
           and number
                   >= static_cast<int64_t>(std::numeric_limits<Integer>::min());
  }
  return static_cast<uint64_t>(number) <= kMax;
}

bool isDigit(char c) { return c >= '0' and c <= '9'; }

bool isHexDigit(char c) {
  return isDigit(c) or (c >= 'a' and c <= 'f') or (c >= 'A' and c <= 'F');
}

bool isIdentifierChar(char c) {
  return isDigit(c) or (c >= 'a' and c <= 'z') or (c >= 'A' and c <= 'Z')
         or c == '_';
}

void skipWhitespace(std::string_view& text) {
  text.remove_prefix(std::min(text.find_first_not_of(" \t\r\n"), text.size()));
}

// Takes the identifier at the front of `text`, or answers an empty view.
std::string_view takeIdentifier(std::string_view& text) {
  auto length = size_t{0};
  while (length < text.size() and isIdentifierChar(text[length])) {
    length++;
  }
  if (length > 0 and isDigit(text[0])) {
    return {};
  }
  const auto identifier = text.substr(0, length);
  text.remove_prefix(length);
  return identifier;
}

// Reads `digits` hex digits from the front of `text` into `value`.
bool takeHex(std::string_view& text, size_t digits, uint32_t& value) {
  if (text.size() < digits) {
    return false;
  }
  value = 0;
  for (size_t i = 0; i < digits; i++) {
    const auto c = text[i];
    if (not isHexDigit(c)) {
      return false;
    }
    const auto digit = isDigit(c)                ? c - '0'
                       : (c >= 'a' and c <= 'f') ? c - 'a' + 10
                                                 : c - 'A' + 10;
    value = (value * 16) + static_cast<uint32_t>(digit);
  }
  text.remove_prefix(digits);
  return true;
}

void appendUtf8(std::string& out, uint32_t codepoint) {
  if (codepoint < 0x80) {
    out += static_cast<char>(codepoint);
  } else if (codepoint < 0x800) {
    out += static_cast<char>(0xC0 | (codepoint >> 6));
    out += static_cast<char>(0x80 | (codepoint & 0x3F));
  } else if (codepoint < 0x10000) {
    out += static_cast<char>(0xE0 | (codepoint >> 12));
    out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (codepoint & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (codepoint >> 18));
    out += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (codepoint & 0x3F));
  }
}

// Reads the `\u` escape after its backslash and `u`, taking a following low
// surrogate where the first names a high one. A lone surrogate is refused.
bool takeUnicodeEscape(std::string_view& text, std::string& out) {
  constexpr auto kHighFirst = uint32_t{0xD800};
  constexpr auto kLowFirst = uint32_t{0xDC00};
  constexpr auto kLowEnd = uint32_t{0xE000};
  constexpr auto kSupplementaryFirst = uint32_t{0x10000};
  constexpr auto kSurrogateBits = 10;
  auto unit = uint32_t{0};
  if (not takeHex(text, 4, unit)) {
    return false;
  }
  if (unit >= kLowFirst and unit < kLowEnd) {
    return false;
  }
  if (unit < kHighFirst or unit >= kLowFirst) {
    appendUtf8(out, unit);
    return true;
  }
  auto low = uint32_t{0};
  if (not text.starts_with("\\u")) {
    return false;
  }
  text.remove_prefix(2);
  if (not takeHex(text, 4, low) or low < kLowFirst or low >= kLowEnd) {
    return false;
  }
  appendUtf8(out, kSupplementaryFirst + ((unit - kHighFirst) << kSurrogateBits)
                      + (low - kLowFirst));
  return true;
}

// The length of the UTF-8 sequence at the front of `text`, which starts with
// a byte of 0x80 or more, or 0 where it is not one: an overlong encoding, a
// surrogate, a code point past U+10FFFF, or one cut short.
size_t utf8SequenceLength(std::string_view text) {
  const auto byte = [&text](size_t i) { return static_cast<uint8_t>(text[i]); };
  const auto lead = byte(0);
  // The lowest and highest second byte each lead allows, which is what rules
  // out overlong encodings, surrogates and code points past U+10FFFF.
  auto length = size_t{0};
  auto secondLow = uint8_t{0x80};
  auto secondHigh = uint8_t{0xBF};
  if (lead >= 0xC2 and lead <= 0xDF) {
    length = 2;
  } else if (lead >= 0xE0 and lead <= 0xEF) {
    length = 3;
    secondLow = (lead == 0xE0) ? uint8_t{0xA0} : secondLow;
    secondHigh = (lead == 0xED) ? uint8_t{0x9F} : secondHigh;
  } else if (lead >= 0xF0 and lead <= 0xF4) {
    length = 4;
    secondLow = (lead == 0xF0) ? uint8_t{0x90} : secondLow;
    secondHigh = (lead == 0xF4) ? uint8_t{0x8F} : secondHigh;
  } else {
    return 0;
  }
  if (text.size() < length or byte(1) < secondLow or byte(1) > secondHigh) {
    return 0;
  }
  for (size_t i = 2; i < length; i++) {
    if (byte(i) < 0x80 or byte(i) > 0xBF) {
      return 0;
    }
  }
  return length;
}

// Reads the quoted string at the front of `text`, quoted with `"` or `'`. As
// the FlatBuffers JSON parser does, it refuses a control character and text
// that is not UTF-8, and takes `\x` escapes as bytes.
bool takeString(std::string_view& text, std::string& out) {
  constexpr auto kFirstPrintable = uint8_t{0x20};
  constexpr auto kFirstMultibyte = uint8_t{0x80};
  const auto quote = text.front();
  text.remove_prefix(1);
  out.clear();
  while (not text.empty()) {
    const auto c = text.front();
    const auto byte = static_cast<uint8_t>(c);
    if (byte >= kFirstMultibyte) {
      const auto length = utf8SequenceLength(text);
      if (length == 0) {
        return false;
      }
      out += text.substr(0, length);
      text.remove_prefix(length);
      continue;
    }
    text.remove_prefix(1);
    if (c == quote) {
      return true;
    }
    if (byte < kFirstPrintable) {
      return false;
    }
    if (c != '\\') {
      out += c;
      continue;
    }
    if (text.empty()) {
      return false;
    }
    const auto escape = text.front();
    text.remove_prefix(1);
    auto escaped = uint32_t{0};
    switch (escape) {
      case '"':
      case '\'':
      case '\\':
      case '/':
        out += escape;
        break;
      case 'b':
        out += '\b';
        break;
      case 'f':
        out += '\f';
        break;
      case 'n':
        out += '\n';
        break;
      case 'r':
        out += '\r';
        break;
      case 't':
        out += '\t';
        break;
      case 'x':
        if (not takeHex(text, 2, escaped)) {
          return false;
        }
        out += static_cast<char>(escaped);
        break;
      case 'u':
        if (not takeUnicodeEscape(text, out)) {
          return false;
        }
        break;
      default:
        return false;
    }
  }
  return false;
}

// The double `numeral` spells, or nullopt where it spells none, or a finite
// value a double does not hold. A value too small for one reads as the
// nearest it holds, 0 or a subnormal, as strtod rounds it.
std::optional<double> readDouble(std::string_view numeral) {
  auto number = 0.0;
  // StringToNumber is locale-independent, which strtod alone is not, and
  // builds where a floating std::from_chars does not.
  if (not flatbuffers::StringToNumber(std::string{numeral}.c_str(), &number)
      or not std::isfinite(number)) {
    return std::nullopt;
  }
  return number;
}

// The length of the run of digits at `from` in `text`, in `base` 10 or 16.
size_t digitsAt(std::string_view text, size_t from, bool hex) {
  auto end = from;
  while (end < text.size()
         and (hex ? isHexDigit(text[end]) : isDigit(text[end]))) {
    end++;
  }
  return end - from;
}

// Reads the number at the front of `text`: an integer as the narrowest of a
// signed or unsigned 64-bit one that holds it, and anything else -- one with
// a fraction or an exponent, or a decimal integer past both 64-bit ranges --
// as a double. A numeral no double holds is refused, as is one that runs on
// into a name or a second point.
bool takeNumber(std::string_view& text, flexbuffers::Builder& builder) {
  auto rest = text;
  const auto negative = rest.starts_with("-");
  if (negative or rest.starts_with("+")) {
    rest.remove_prefix(1);
  }
  if (not rest.empty() and not isDigit(rest.front()) and rest.front() != '.') {
    const auto name = takeIdentifier(rest);
    const auto sign = negative ? -1.0 : 1.0;
    if (name == "inf" or name == "infinity") {
      builder.Double(sign * std::numeric_limits<double>::infinity());
    } else if (name == "nan") {
      builder.Double(std::numeric_limits<double>::quiet_NaN());
    } else {
      return false;
    }
    text = rest;
    return true;
  }

  const auto hex = rest.starts_with("0x") or rest.starts_with("0X");
  const auto prefix = hex ? size_t{2} : size_t{0};
  const auto integerDigits = digitsAt(rest, prefix, hex);
  auto length = prefix + integerDigits;
  auto fractional = false;
  if (not hex and length < rest.size() and rest[length] == '.') {
    fractional = true;
    length += 1 + digitsAt(rest, length + 1, false);
  }
  if (length == prefix + (fractional ? 1 : 0)) {
    // No digit at all: `0x`, `.`, or a sign alone.
    return false;
  }
  if (not hex and length < rest.size()
      and (rest[length] == 'e' or rest[length] == 'E')) {
    fractional = true;
    auto exponent = length + 1;
    if (exponent < rest.size()
        and (rest[exponent] == '+' or rest[exponent] == '-')) {
      exponent++;
    }
    const auto exponentDigits = digitsAt(rest, exponent, false);
    if (exponentDigits == 0) {
      return false;
    }
    length = exponent + exponentDigits;
  }
  if (length < rest.size()
      and (isIdentifierChar(rest[length]) or rest[length] == '.')) {
    return false;
  }
  const auto numeral = rest.substr(0, length);

  auto magnitude = uint64_t{0};
  const auto* digits = numeral.data() + prefix;
  const auto parsed =
      fractional ? std::from_chars_result{digits, std::errc::invalid_argument}
                 : std::from_chars(digits, numeral.data() + numeral.size(),
                                   magnitude, hex ? 16 : 10);
  constexpr auto kSignedMagnitude =
      static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
  if (parsed.ec == std::errc{}
      and (not negative or magnitude <= kSignedMagnitude + 1)) {
    if (negative) {
      builder.Int(magnitude == kSignedMagnitude + 1
                      ? std::numeric_limits<int64_t>::min()
                      : -static_cast<int64_t>(magnitude));
    } else if (magnitude > kSignedMagnitude) {
      builder.UInt(magnitude);
    } else {
      builder.Int(static_cast<int64_t>(magnitude));
    }
  } else {
    // A hex numeral is an integer or nothing.
    if (hex) {
      return false;
    }
    const auto number = readDouble(numeral);
    if (not number) {
      return false;
    }
    builder.Double(negative ? -*number : *number);
  }
  rest.remove_prefix(length);
  text = rest;
  return true;
}

bool takeValue(std::string_view& text, flexbuffers::Builder& builder,
               int depth);

// Reads the items of a map or a list after its opening bracket, through its
// closing one, calling `item` for each. A trailing comma is allowed, as the
// FlatBuffers JSON parser allows one.
template <typename Item>
bool takeItems(std::string_view& text, char closing, Item item) {
  skipWhitespace(text);
  while (not text.starts_with(closing)) {
    if (not item()) {
      return false;
    }
    skipWhitespace(text);
    if (text.starts_with(",")) {
      text.remove_prefix(1);
      skipWhitespace(text);
    } else if (not text.starts_with(closing)) {
      return false;
    }
  }
  text.remove_prefix(1);
  return true;
}

bool takeValue(std::string_view& text, flexbuffers::Builder& builder,
               int depth) {
  skipWhitespace(text);
  if (text.empty() or depth > kMaxPayloadDepth) {
    return false;
  }
  const auto c = text.front();
  if (c == '{') {
    text.remove_prefix(1);
    const auto start = builder.StartMap();
    auto key = std::string{};
    const auto read = takeItems(text, '}', [&] {
      if (text.starts_with("\"") or text.starts_with("'")) {
        if (not takeString(text, key)) {
          return false;
        }
      } else {
        key = takeIdentifier(text);
      }
      skipWhitespace(text);
      if (key.empty() or key.find('\0') != std::string::npos
          or not text.starts_with(":")) {
        return false;
      }
      text.remove_prefix(1);
      builder.Key(key);
      return takeValue(text, builder, depth + 1);
    });
    if (not read) {
      return false;
    }
    builder.EndMap(start);
    return not builder.HasDuplicateKeys();
  }
  if (c == '[') {
    text.remove_prefix(1);
    const auto start = builder.StartVector();
    if (not takeItems(text, ']',
                      [&] { return takeValue(text, builder, depth + 1); })) {
      return false;
    }
    builder.EndVector(start, false, false);
    return true;
  }
  if (c == '"' or c == '\'') {
    auto string = std::string{};
    if (not takeString(text, string)) {
      return false;
    }
    builder.String(string);
    return true;
  }
  if (isDigit(c) or c == '-' or c == '+' or c == '.') {
    return takeNumber(text, builder);
  }
  auto rest = text;
  const auto name = takeIdentifier(rest);
  if (name == "true" or name == "false") {
    builder.Bool(name == "true");
    text = rest;
    return true;
  }
  return takeNumber(text, builder);
}

// A flexbuffer list. `IsVector` is true of a map too, and a typed vector
// reads as empty through `AsVector`, so neither is one.
bool isList(flexbuffers::Reference value) {
  return value.GetType() == flexbuffers::FBT_VECTOR;
}

// Whether `value` is a list of between 1 and `maxLength` scalars of
// `elementType`.
bool scalarListFits(flexbuffers::Reference value,
                    reflection::BaseType elementType, size_t maxLength) {
  if (not isList(value)) {
    return false;
  }
  const auto elements = value.AsVector();
  if (elements.size() == 0 or elements.size() > maxLength) {
    return false;
  }
  for (size_t i = 0; i < elements.size(); i++) {
    if (not scalarFits(elements[i], elementType)) {
      return false;
    }
  }
  return true;
}

void writeScalar(flexbuffers::Reference value, reflection::BaseType type,
                 std::span<uint8_t> at) {
  const auto bytes = scalarBytes(value, type);
  const auto size = GetTypeSize(type);
  if (at.size() >= size) {
    std::memcpy(at.data(), bytes.data(), size);
  }
}

// The member of `structDef` stored at `offset` bytes into it.
const reflection::Field* structFieldAtOffset(
    const reflection::Object* structDef, uint32_t offset) {
  for (const auto* fieldDef : *structDef->fields()) {
    if (fieldDef->offset() == offset) {
      return fieldDef;
    }
  }
  return nullptr;
}

bool writeStructMap(const reflection::Schema* schema,
                    const reflection::Object* structDef,
                    std::span<uint8_t> image, flexbuffers::Map map) {
  const auto keys = map.Keys();
  const auto values = map.Values();
  if (keys.size() == 0) {
    return false;
  }
  for (size_t i = 0; i < keys.size(); i++) {
    const auto* fieldDef = structDef->fields()->LookupByKey(keys[i].AsKey());
    if (fieldDef == nullptr) {
      return false;
    }
    const auto memberOffset = uint32_t{fieldDef->offset()};
    if (not writeStructPayload(schema, structDef, image,
                               std::span{&memberOffset, 1}, values[i])) {
      return false;
    }
  }
  return true;
}

}  // namespace

bool scalarFits(flexbuffers::Reference value, reflection::BaseType type) {
  const auto isInteger = value.IsInt() or value.IsUInt();
  switch (type) {
    case reflection::Bool:
      // 0 and 1 are the bool an integer spells; no other number is one.
      return value.IsBool() or integerFits<bool>(value);
    case reflection::Byte:
      return integerFits<int8_t>(value);
    case reflection::UByte:
      return integerFits<uint8_t>(value);
    case reflection::Short:
      return integerFits<int16_t>(value);
    case reflection::UShort:
      return integerFits<uint16_t>(value);
    case reflection::Int:
      return integerFits<int32_t>(value);
    case reflection::UInt:
      return integerFits<uint32_t>(value);
    case reflection::Long:
      return integerFits<int64_t>(value);
    case reflection::ULong:
      return integerFits<uint64_t>(value);
    case reflection::Float:
      // A finite value a float32 rounds to infinity is not one it holds. An
      // infinity or a NaN is, as itself.
      return isInteger
             or (value.IsFloat()
                 and (not std::isfinite(value.AsDouble())
                      or std::fabs(value.AsDouble()) < kFloatRoundsToInfinity));
    case reflection::Double:
      return isInteger or value.IsFloat();
    default:
      return false;
  }
}

std::array<uint8_t, 8> scalarBytes(flexbuffers::Reference value,
                                   reflection::BaseType type) {
  auto bytes = std::array<uint8_t, 8>{};
  const auto store = [&bytes](auto scalar) {
    std::memcpy(bytes.data(), &scalar, sizeof(scalar));
  };
  switch (type) {
    case reflection::Bool:
      store(value.AsBool());
      break;
    case reflection::Byte:
      store(value.AsInt8());
      break;
    case reflection::UType:
    case reflection::UByte:
      store(value.AsUInt8());
      break;
    case reflection::Short:
      store(value.AsInt16());
      break;
    case reflection::UShort:
      store(value.AsUInt16());
      break;
    case reflection::Int:
      store(value.AsInt32());
      break;
    case reflection::UInt:
      store(value.AsUInt32());
      break;
    case reflection::Long:
      store(value.AsInt64());
      break;
    case reflection::ULong:
      store(value.AsUInt64());
      break;
    case reflection::Float:
      store(value.AsFloat());
      break;
    case reflection::Double:
      store(value.AsDouble());
      break;
    default:
      break;
  }
  return bytes;
}

bool writeStructPayload(const reflection::Schema* schema,
                        const reflection::Object* structDef,
                        std::span<uint8_t> image,
                        std::span<const uint32_t> path,
                        flexbuffers::Reference value) {
  if (path.empty()) {
    return value.IsMap()
           and writeStructMap(schema, structDef, image, value.AsMap());
  }
  const auto* fieldDef = structFieldAtOffset(structDef, path.front());
  if (fieldDef == nullptr or fieldDef->offset() >= image.size()) {
    return false;
  }
  auto at = image.subspan(fieldDef->offset());
  const auto baseType = fieldDef->type()->base_type();
  const auto rest = path.subspan(1);

  if (baseType == reflection::Obj) {
    const auto* nested = schema->objects()->Get(fieldDef->type()->index());
    return writeStructPayload(schema, nested, at.first(nested->bytesize()),
                              rest, value);
  }
  if (baseType == reflection::Array) {
    const auto elementType = fieldDef->type()->element();
    if (not IsScalar(elementType)) {
      // An array of structs: `GetTypeSize` answers a pointer's width for one,
      // not the struct's, so every element would be written at the wrong
      // stride.
      return false;
    }
    const auto elementSize = GetTypeSize(elementType);
    const auto length = size_t{fieldDef->type()->fixed_length()};
    if (rest.empty()) {
      // A shorter array leaves the elements it does not reach.
      if (not scalarListFits(value, elementType, length)) {
        return false;
      }
      const auto elements = value.AsVector();
      for (size_t i = 0; i < elements.size(); i++) {
        writeScalar(elements[i], elementType, at.subspan(i * elementSize));
      }
      return true;
    }
    const auto index = size_t{rest.front()};
    if (index >= length or rest.size() != 1
        or not scalarFits(value, elementType)) {
      return false;
    }
    writeScalar(value, elementType, at.subspan(index * elementSize));
    return true;
  }
  if (not rest.empty() or not scalarFits(value, baseType)) {
    return false;
  }
  writeScalar(value, baseType, at);
  return true;
}

bool payloadFits(const reflection::Schema* schema, const reflection::Type* type,
                 bool element, flexbuffers::Reference value) {
  const auto baseType = element ? type->element() : type->base_type();
  switch (baseType) {
    case reflection::String:
      return value.IsString();
    case reflection::Obj: {
      const auto* objectDef = schema->objects()->Get(type->index());
      if (not objectDef->is_struct()) {
        return false;
      }
      auto image = std::vector<uint8_t>(objectDef->bytesize());
      return writeStructPayload(schema, objectDef, image, {}, value);
    }
    case reflection::Vector: {
      const auto elementType = type->element();
      if (IsScalar(elementType)) {
        // A whole vector may be empty, which an array may not.
        return isList(value)
               and (value.AsVector().size() == 0
                    or scalarListFits(value, elementType,
                                      value.AsVector().size()));
      }
      if (elementType != reflection::String or not isList(value)) {
        return false;
      }
      const auto elements = value.AsVector();
      for (size_t i = 0; i < elements.size(); i++) {
        if (not elements[i].IsString()) {
          return false;
        }
      }
      return true;
    }
    case reflection::Array:
      return IsScalar(type->element())
             and scalarListFits(value, type->element(), type->fixed_length());
    default:
      return scalarFits(value, baseType);
  }
}

bool parseFlexValue(std::string_view& text, flexbuffers::Builder& builder) {
  return takeValue(text, builder, 0);
}
