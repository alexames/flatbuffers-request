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

// Whether a put can build a vector of `count` elements of the struct
// `structDef`, which are inline at its size, with `bytesBuilt` bytes already
// in the builder it is built into. A whole-vector payload is no bound on its
// own: each of its elements may be an offset to one shared map.
inline bool structVectorCanHold(const reflection::Object* structDef,
                                uint64_t count, uint64_t bytesBuilt = 0) {
  return count * static_cast<uint64_t>(structDef->bytesize()) + bytesBuilt
         <= kLargestGrownVectorBytes;
}

// Whether a put can grow a vector of the struct `structDef` to hold an
// element at `index` (see structVectorCanHold).
inline bool structVectorCanGrowTo(const reflection::Object* structDef,
                                  uint64_t index, uint64_t bytesBuilt = 0) {
  return structVectorCanHold(structDef, index + 1, bytesBuilt);
}

// Whether a put can grow a vector of `elementType`, which is not a struct, to
// hold an element at `index`, with `bytesBuilt` bytes already in the builder
// it is built into. Past this the builder would overrun the most a flatbuffer
// can hold.
inline bool vectorCanGrowTo(reflection::BaseType elementType, uint64_t index,
                            uint64_t bytesBuilt = 0) {
  const auto bytesPerElement =
      flatbuffers::IsScalar(elementType)
          ? uint64_t{flatbuffers::GetTypeSize(elementType)}
          : kMostBytesPerPaddedOffsetElement;
  return (index + 1) * bytesPerElement + bytesBuilt <= kLargestGrownVectorBytes;
}
