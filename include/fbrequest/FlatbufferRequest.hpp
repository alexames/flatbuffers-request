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
// With `useStringPooling`, identical strings share one copy in the output.
// Every field is read as its type declares it, as `verifyBuffer` reads it,
// and keeps its alignment. A union payload stored under NONE or under a member
// its union does not declare has no layout to copy it by: a union field keeps
// its discriminant and drops the payload, and an element of a vector of
// unions becomes an empty table. A vector of unions keeps only the elements
// its discriminants pair with, and its discriminants whole. A field at a
// vtable offset the schema does not declare is dropped. `tryApplyRequest`
// refuses a request that would copy any of these.
flatbuffers::Offset<const flatbuffers::Table*> copyTable(
    flatbuffers::FlatBufferBuilder& fbb, const reflection::Schema* schema,
    const uint8_t* ptr, bool useStringPooling);

// Whether `buffer` is a finished flatbuffer whose root is a table of
// `schema`'s root type, every field within the buffer and read as its type
// declares it. Unlike FlatBuffers' reflection `Verify`, this reads a string or
// vector held through a 64-bit offset (`offset64`, `vector64`) through 64
// bits, a vector of structs at the struct's size, and a union member by its
// discriminant's value, as a table, a struct or a string. A union payload
// stored under NONE is not read, as FlatBuffers reads none; one stored under
// a member the union does not declare fails, and so does a vector of unions
// whose discriminants do not pair one to one with its elements.
//
// @param schema Schema the buffer was built against. Read, not stored.
// @param buffer The finished buffer. Its data must be aligned to the widest
//     scalar it holds, as a flatbuffer's alignment is checked from its start.
// @return True where every field verifies; false for anything shorter than a
//     root offset, and for a buffer of FLATBUFFERS_MAX_BUFFER_SIZE or more.
[[nodiscard]] bool verifyBuffer(const reflection::Schema* schema,
                                std::span<const uint8_t> buffer);

// Parses a textual request (`put` / `patch` / `delete`) against `schema` into a
// serialized Request buffer. Returns nullopt when the request is malformed.
// Only the bytes `requestString` views are read; it need not be terminated.
// The buffer returned passes `serialized::VerifyRequestBuffer`, whatever kind
// of payload it carries. Read the result with
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
  // an undeclared one, an empty array or one past its length), or vectors
  // grown past the bound below. The parser refuses each of these, the last
  // only as it would be for an empty builder; a hand-built request can still
  // carry one.
  //
  // Also a request missing a member the parser always writes -- an
  // operation's path, a put's payload, a patch's operations and their types
  // -- and a patch whose path stops anywhere but a table or a struct a put
  // could reach, whatever operations it holds.
  //
  // Also an operation that would copy, unedited, what no copy keeps as it
  // was, in the source or in a put's value: a union payload stored under NONE
  // or under a member its union does not declare, a vector of unions whose
  // discriminants do not pair one to one with its elements, a discriminant
  // stored without its union, or a field the schema does not declare. A request
  // that
  // removes such a field, or switches the union to another member, does not
  // copy it.
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
// where its path stops at a table or a struct a put could reach, and is
// Refused elsewhere, as any patch is.
//
// The bytes `fbb` holds on entry, and those of every vector a request grows
// or builds whole -- each counted at the most its elements take, a string or
// table at its payload's bytes -- are at most half of what a flatbuffer
// holds together, or the request is Refused. A request that is Applied grows
// every vector it names. What the rest of the table copies beside them is not
// counted.
//
// @param fbb Builder the new root is built into. Nothing is added to it for an
//     outcome other than Applied. The caller finishes it.
// @param schema Schema `sourceTable` and the request were built against. Read,
//     not stored.
// @param sourceTable Root table to apply the request to, of `schema`'s root
//     type, in a buffer that `verifyBuffer` accepts. Read, not stored; it
//     need not outlive the call.
// @param request The request, in a buffer that passes
//     `serialized::VerifyRequestBuffer`: as `parseFlatbufferRequest`
//     serializes one, or built by hand. What that verifier does not check
//     is checked here: what its paths name, and what each payload holds,
//     verified as the flatbuffer or flexbuffer its path calls for. Read, not
//     stored.
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
