#pragma once
#include <cstdint>

#include "flatbuffers/flatbuffers.h"
#include "flatbuffers/reflection.h"

// Internal to the library; not part of the installed public headers.

// The most bytes one element of a vector of strings, tables or unions takes
// when a put grows the vector to reach an index: its 4-byte offset, and the
// empty string or table that pads it, which is at most 8 bytes (an empty
// string's length, terminator and alignment), with room for a union vector's
// 1-byte discriminant.
inline constexpr uint64_t kMostBytesPerPaddedOffsetElement = 16;

// The most bytes a put may grow one vector to. Half of what a flatbuffer can
// address, so that the rest of the table the vector is built into still fits
// beside it.
inline constexpr uint64_t kLargestGrownVectorBytes =
    FLATBUFFERS_MAX_BUFFER_SIZE / 2;

// Whether a put can grow a vector of `elementType` to hold an element at
// `index`, with `bytesBuilt` bytes already in the builder it is built into.
// Past this the builder would overrun the most a flatbuffer can hold.
inline bool vectorCanGrowTo(reflection::BaseType elementType, uint64_t index,
                            uint64_t bytesBuilt = 0) {
  const auto bytesPerElement =
      flatbuffers::IsScalar(elementType)
          ? uint64_t{flatbuffers::GetTypeSize(elementType)}
          : kMostBytesPerPaddedOffsetElement;
  return (index + 1) * bytesPerElement + bytesBuilt <= kLargestGrownVectorBytes;
}
