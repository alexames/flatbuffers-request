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
// Only the bytes `requestString` views are read; it need not be terminated.
// Read the result with
// flatbuffers::GetRoot<serialized::Request>(buffer.data()).
std::optional<ByteBuffer> parseFlatbufferRequest(
    const reflection::Schema* schema, std::string_view requestString);

// What applying a request came to.
enum class ApplyOutcome : uint8_t {
  // Every operation made the edit it names. The new root is built into the
  // builder. A put that writes the value a field already holds is applied.
  Applied,
  // A delete names something the buffer does not store: a field, an element
  // past the end of a vector, an element of a vector the buffer does not
  // store, anything inside a table or union it does not store, or a member of
  // a union other than the one it holds. Nothing is built. For a lone delete
  // the source already lacks what it names, so a caller treating delete as
  // idempotent keeps the source as the result. For a patch it means at least
  // one of its deletes is NotStored and none of its operations is Refused;
  // the patch's other edits were not made either.
  NotStored,
  // An operation cannot make the edit it names, whatever the buffer stores,
  // so nothing is built: a path the schema does not resolve, a union member
  // that holds no table, a delete inside a struct, a put whose payload is not
  // of its field's kind or writes nothing (a struct map naming no member or
  // an undeclared one, an empty array or one past its length), or a vector
  // grown past what a flatbuffer holds. The parser refuses each of these; a
  // hand-built request can still carry one.
  Refused,
};

// The result of `tryApplyRequest`.
struct ApplyResult {
  // The new root, built into the builder the request was applied with. Null
  // unless `outcome` is Applied, and then not to be passed to `Finish`.
  flatbuffers::Offset<const flatbuffers::Table*> root{};
  // What the apply came to.
  ApplyOutcome outcome{ApplyOutcome::Refused};
};

// Applies `request` to `sourceTable`, building the resulting table into `fbb`
// only where every operation makes the edit it names.
//
// A patch is all or nothing. Each of its operations is checked against the
// table the operations before it left; the patch answers Refused where any
// is Refused, otherwise NotStored where any is NotStored, and builds nothing
// for either. An empty patch names no edit: it is Applied and builds a copy,
// whatever its path.
//
// @param fbb Builder the new root is built into. Nothing is added to it for an
//     outcome other than Applied. The caller finishes it.
// @param schema Schema `sourceTable` and the request were built against. Read,
//     not stored.
// @param sourceTable Root table to apply the request to, of `schema`'s root
//     type, in a buffer that passes the flatbuffers verifier. Read, not
//     stored; it need not outlive the call.
// @param request The request, in a buffer that passes the verifier for
//     `serialized::Request`: as `parseFlatbufferRequest` serializes one, or
//     built by hand. What the verifier does not check -- what its paths name,
//     what its payloads hold -- is checked here. Read, not stored.
// @param useStringPooling When true, identical strings are shared in the
//     output.
// @return The root built and Applied, or a null root and why nothing was
//     built.
[[nodiscard]] ApplyResult tryApplyRequest(flatbuffers::FlatBufferBuilder& fbb,
                                          const reflection::Schema* schema,
                                          const flatbuffers::Table* sourceTable,
                                          const serialized::Request* request,
                                          bool useStringPooling = false);

// Applies `request` to `sourceTable`, as `tryApplyRequest` does, answering
// only the root: the offset of the new root, or a null offset, with nothing
// built, where the request does not make the edit it names. A null offset
// must not be passed to `Finish`; `tryApplyRequest` answers why it is null.
// When `useStringPooling` is true, identical strings are shared in the output.
[[nodiscard]] flatbuffers::Offset<const flatbuffers::Table*> applyRequest(
    flatbuffers::FlatBufferBuilder& fbb, const reflection::Schema* schema,
    const flatbuffers::Table* sourceTable, const serialized::Request* request,
    bool useStringPooling = false);

}  // namespace fbrequest
