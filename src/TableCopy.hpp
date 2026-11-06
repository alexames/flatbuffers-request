#pragma once
#include <cstddef>
#include <cstdint>
#include <span>

#include "flatbuffers/flatbuffers.h"
#include "flatbuffers/reflection.h"

// The library's own reflection-driven copy of what a buffer stores, and the
// verifier and checks that decide what it can copy. Internal to the library;
// not part of the installed public headers.
//
// FlatBuffers' `CopyTable` reads every union member as a table, copies a
// vector of unions as offsets into the buffer it copies from, reads a string
// or vector held through a 64-bit offset through 32 bits, and lays every
// vector of structs out at 4 bytes. This copy reads each field as its type
// declares it, so what it copies reads back as the source did.

// Whether `fieldDef` is held through a 64-bit offset: an `offset64` string or
// vector, or a `vector64`, whose length is 64 bits as well.
bool isWideOffsetField(const reflection::Field* fieldDef);

// Whether the field `fieldDef` names is built from an offset: a string, a
// vector, a table or a union. A scalar, a discriminant and a struct are
// written inline.
bool isOffsetField(const reflection::Schema* schema,
                   const reflection::Field* fieldDef);

// The alignment the data of a vector `fieldDef` names is laid out at: its
// element's, raised to what a `force_align`, `nested_flatbuffer` or
// `flexbuffer` attribute asks. A schema serialized without its builtin
// attributes carries none of them, and its vectors align to their elements.
size_t vectorAlignment(const reflection::Schema* schema,
                       const reflection::Field* fieldDef);

// Whether a copy of `table`, a table of `objectDef`, comes out as it went in,
// every stored field but `exceptField` (which may be null) included. A copy
// cannot be faithful to a union holding a payload under NONE or under a
// member its union does not declare, whose layout the schema does not give;
// to a vector of unions whose discriminants do not pair one to one with its
// elements; to a discriminant stored without the union it names, which the
// builders write only beside its value; to a vector held through a 64-bit
// offset whose elements are offsets; or to a field at a vtable offset
// `objectDef` does not declare, as a buffer written with a newer schema may
// store. Skipping `exceptField` skips its discriminant too, since a request
// writing or removing a union writes or removes both. `table` must be in a
// buffer that has been verified. True for a null `table`.
bool copiesFaithfully(const reflection::Schema* schema,
                      const reflection::Object* objectDef,
                      const flatbuffers::Table* table,
                      const reflection::Field* exceptField = nullptr);

// Whether the field `fieldDef` of `table`, a table of `objectDef`, is copied
// faithfully (see copiesFaithfully). True where `table` does not store it.
bool fieldCopiesFaithfully(const reflection::Schema* schema,
                           const reflection::Object* objectDef,
                           const flatbuffers::Table* table,
                           const reflection::Field* fieldDef);

// Whether `value`, the value of a member with discriminant `member` of the
// union whose enum `enumIndex` names, is copied faithfully: the member is
// declared, and is a table copied faithfully, a struct or a string.
bool unionValueCopiesFaithfully(const reflection::Schema* schema,
                                int32_t enumIndex, uint32_t member,
                                const void* value);

// Whether `bytes` is a flatbuffer whose root is a table of `objectDef`, each
// field within it read as its type declares: a string or vector through the
// width of offset it is held through, a vector of structs at the struct's
// size, a union by its discriminant's value, whichever of a table, a struct
// or a string the member holds. A payload stored under NONE is not read, as
// FlatBuffers' own verifier reads none; one under a member the union does not
// declare fails, as does a vector of unions whose discriminants do not pair
// one to one with its elements. Each discriminant is verified before it is
// read. `bytes` must start on a boundary aligned to the widest scalar the
// buffer holds, since alignment is checked from its start. False for fewer
// bytes than a root offset, and for FLATBUFFERS_MAX_BUFFER_SIZE or more.
bool verifyTableStructure(const reflection::Schema* schema,
                          const reflection::Object* objectDef,
                          std::span<const uint8_t> bytes);

// Whether `bytes` is a flatbuffer whose root is a table of `objectDef` that a
// copy reads as it was written: it verifies, each field read as its type
// declares it (see verifyTableStructure), and it copies faithfully.
bool verifyTableBuffer(const reflection::Schema* schema,
                       const reflection::Object* objectDef,
                       std::span<const uint8_t> bytes);

// Copies `table`, a table of `objectDef`, and everything it holds into `fbb`.
//
// @param fbb Builder the copy is built into; no table may be under
//     construction in it.
// @param schema Schema `objectDef` belongs to. Read, not stored.
// @param objectDef The table type of `table`.
// @param table The table to copy, in a verified buffer, which copiesFaithfully
//     should accept: what it refuses is copied as well as the copy can (a
//     union payload it cannot read is dropped, its discriminant kept; such an
//     element of a vector of unions becomes an empty table; a vector of unions
//     keeps the elements its discriminants pair with, the discriminants whole;
//     an undeclared field is dropped). Read, not stored.
// @param useStringPooling Whether identical strings share one copy in `fbb`.
// @return The copy's offset in `fbb`, measured from the end of its buffer as
//     every builder offset is; 0 for a null `table`.
flatbuffers::uoffset_t copyTableDeep(flatbuffers::FlatBufferBuilder& fbb,
                                     const reflection::Schema* schema,
                                     const reflection::Object* objectDef,
                                     const flatbuffers::Table* table,
                                     bool useStringPooling);

// Copies the field `fieldDef` of `table`, a table of `objectDef`, that is
// built from an offset (see isOffsetField) into `fbb`, deep as copyTableDeep
// copies. `table` may be null. `useStringPooling` shares identical strings.
// Answers the copy's offset in `fbb`; 0 for a field stored inline, not
// stored, or a union whose payload cannot be read. Add it to the table being
// built with addFieldOffset.
flatbuffers::uoffset_t copyFieldOffset(flatbuffers::FlatBufferBuilder& fbb,
                                       const reflection::Schema* schema,
                                       const reflection::Object* objectDef,
                                       const flatbuffers::Table* table,
                                       const reflection::Field* fieldDef,
                                       bool useStringPooling);

// Adds `offset` to the table being built as the field `fieldDef`, through a
// 64-bit offset where the field is held through one. A null `offset` leaves
// the field absent.
void addFieldOffset(flatbuffers::FlatBufferBuilder& fbb,
                    const reflection::Field* fieldDef,
                    flatbuffers::uoffset_t offset);

// Copies `value`, the value of a member with discriminant `member` of the
// union whose enum `enumIndex` names, answering its offset: a table copied
// deep, a struct as its bytes at its alignment, a string as a string, with
// `useStringPooling` sharing identical strings. `value` points into a verified
// buffer and is not stored. Answers 0, building nothing, for NONE and for a
// member the union does not declare.
flatbuffers::uoffset_t copyUnionValue(flatbuffers::FlatBufferBuilder& fbb,
                                      const reflection::Schema* schema,
                                      int32_t enumIndex, uint32_t member,
                                      const void* value, bool useStringPooling);
