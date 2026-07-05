#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "fbrequest/FlatbufferRequest_generated.h"
#include "flatbuffers/reflection.h"

namespace fbrequest {

// Owned storage for a serialized flatbuffer.
using ByteBuffer = std::vector<uint8_t>;

// Deep-copies the table at `ptr` (interpreted against `schema`'s root type)
// into `fbb`, finishing the buffer. Returns the offset of the copied root.
flatbuffers::Offset<const flatbuffers::Table*> copyTable(
    flatbuffers::FlatBufferBuilder& fbb, const reflection::Schema* schema,
    const uint8_t* ptr, bool useStringPooling);

// Parses a textual request (`put` / `patch` / `delete`) against `schema` into a
// serialized Request buffer. Returns nullopt when the request is malformed.
// Read the result with
// flatbuffers::GetRoot<serialized::Request>(buffer.data()).
std::optional<ByteBuffer> parseFlatbufferRequest(
    const reflection::Schema* schema, std::string_view requestString);

// Applies `request` to `sourceTable` (which must match `schema`'s root type),
// building the resulting table into `fbb`. Returns the offset of the new root.
// When `useStringPooling` is true, identical strings are shared in the output.
flatbuffers::Offset<const flatbuffers::Table*> applyRequest(
    flatbuffers::FlatBufferBuilder& fbb, const reflection::Schema* schema,
    const flatbuffers::Table* sourceTable, const serialized::Request* request,
    bool useStringPooling = false);

}  // namespace fbrequest
