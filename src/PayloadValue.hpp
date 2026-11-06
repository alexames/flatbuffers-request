#pragma once
#include <array>
#include <cstdint>
#include <span>

#include "flatbuffers/flexbuffers.h"
#include "flatbuffers/reflection.h"

// What a put's flexbuffer payload may hold, decided once for the parser, which
// refuses a request whose payload does not fit, and for the apply, which
// refuses a hand-built request the parser would not have produced. Internal
// to the library; not part of the installed public headers.

// Whether `value` is a value a scalar of `type` holds.
bool scalarFits(flexbuffers::Reference value, reflection::BaseType type);

// The bytes of `value` as a scalar of `type`, in the first GetTypeSize(type)
// of the array. `value` must fit (see scalarFits).
std::array<uint8_t, 8> scalarBytes(flexbuffers::Reference value,
                                   reflection::BaseType type);

// Writes `value` into `image`, the bytes of a struct of `structDef`, at the
// member `path` names: byte offsets into successively nested structs and, last,
// an element index into a fixed-length array. An empty `path` takes `value` as
// a map of the struct, naming any subset of its members.
//
// Answers false where `value` writes nothing or is not a value the member
// takes: a map naming no member, or a member the struct does not declare, or a
// member whose own value does not fit; an empty array, or one longer than its
// declared length; an array of structs; a scalar of the wrong kind. `image` is
// then partly written, and the caller discards it.
bool writeStructPayload(const reflection::Schema* schema,
                        const reflection::Object* structDef,
                        std::span<uint8_t> image,
                        std::span<const uint32_t> path,
                        flexbuffers::Reference value);

// Whether `value` is a flexbuffer payload a put writes into a value of `type`:
// the field or member whole, or with `element`, one element of the vector or
// array it is. A table is not a flexbuffer payload, so it answers false.
bool payloadFits(const reflection::Schema* schema, const reflection::Type* type,
                 bool element, flexbuffers::Reference value);
