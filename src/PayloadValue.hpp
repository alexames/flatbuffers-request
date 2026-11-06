#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <string_view>

#include "flatbuffers/flexbuffers.h"
#include "flatbuffers/reflection.h"

// What a put's flexbuffer payload may hold, decided once for the parser, which
// refuses a request whose payload does not fit, and for the apply, which
// refuses a hand-built request the parser would not have produced. Internal
// to the library; not part of the installed public headers.

// Reads the JSON value at the front of `text` into `builder`, advancing `text`
// past it and no further. Maps (keys quoted or bare), lists, strings, `true`
// and `false` are read as the FlatBuffers JSON parser reads them, a trailing
// comma included. A number is read exactly: a decimal or hex integer as a
// signed 64-bit integer, or as an unsigned one where only that holds it, and
// anything else -- a fraction or an exponent, a decimal integer past both
// 64-bit ranges, or `inf`, `infinity` or `nan` with an optional sign -- as a
// double. Answers false, with `text` and `builder` unspecified, for text that
// does not start with such a value: `null`, a hex integer past 64 bits, a
// finite numeral too large for a double, a numeral running on into a name or
// a second point (`5abc`, `5..`), a string holding a control character or
// text that is not UTF-8, a duplicated map key, or nesting past the
// FlatBuffers parser's depth.
bool parseFlexValue(std::string_view& text, flexbuffers::Builder& builder);

// Whether `value` is a value a scalar of `type` holds. A bool takes `true`,
// `false`, 0 or 1. An integer type takes an integer within its range, never a
// bool or a float. A float32 takes any integer, an infinity or NaN, or a
// finite float it does not round to infinity; a float64 any number.
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
