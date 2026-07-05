#pragma once
#include <cstdint>
#include <utility>
#include <vector>

#include "flatbuffers/flatbuffers.h"

namespace fbrequest {

// Raw byte storage for a serialized flatbuffer.
using ByteBuffer = std::vector<uint8_t>;

template <typename T>
struct flatbuffer_traits {};

enum class FlatbufferPrefix : std::uint8_t {
  None,
  SizePrefixed,
};

// Pairs a flatbuffer binary payload with its root type for type-safe access.
template <typename T, FlatbufferPrefix sizePrefixed = FlatbufferPrefix::None>
class TypedBuffer {
 public:
  TypedBuffer() {}
  explicit TypedBuffer(ByteBuffer buffer) : m_buffer{std::move(buffer)} {}

  [[nodiscard]] ByteBuffer& buffer() { return m_buffer; }
  [[nodiscard]] const ByteBuffer& buffer() const { return m_buffer; }

  [[nodiscard]] const T* get() const {
    if constexpr (sizePrefixed == FlatbufferPrefix::SizePrefixed) {
      return flatbuffers::GetSizePrefixedRoot<T>(m_buffer.data());
    } else {
      return flatbuffers::GetRoot<T>(m_buffer.data());
    }
  }

  [[nodiscard]] const T* operator->() const { return get(); }

  friend bool operator==(const TypedBuffer& lhs, const TypedBuffer& rhs) {
    return lhs.m_buffer == rhs.m_buffer;
  }

 private:
  ByteBuffer m_buffer{};
};

}  // namespace fbrequest
