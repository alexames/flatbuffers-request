#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

#include "ElementIndex.hpp"
#include "PayloadValue.hpp"
#include "RequestApply.hpp"
#include "TableCopy.hpp"
#include "fbrequest/FlatbufferRequest.hpp"
#include "flatbuffers/flatbuffers.h"
#include "flatbuffers/idl.h"
#include "flatbuffers/reflection.h"

using flatbuffers::FlatBufferBuilder;
using flatbuffers::GetAnyRoot;
using flatbuffers::GetTypeSize;
using flatbuffers::IsScalar;
using flatbuffers::Offset;
using flatbuffers::Parser;
using flatbuffers::String;
using flatbuffers::Table;
using flatbuffers::UnionTypeFieldSuffix;
using flatbuffers::uoffset_t;
using flatbuffers::Vector;
using flatbuffers::VectorOfAny;
using flatbuffers::voffset_t;
using serialized::Delete;
using serialized::FinishRequestBuffer;
using serialized::Operation;
using serialized::Patch;
using serialized::Put;
using serialized::Request;

static uoffset_t applyRequestPut(FlatBufferBuilder& fbb,
                                 const reflection::Schema* schema,
                                 const reflection::Object* tableDef,
                                 const Table* sourceTable,
                                 std::span<const uint8_t> payload,
                                 std::span<const uint32_t> path,
                                 bool useStringPooling);

static uoffset_t applyRequestDelete(FlatBufferBuilder& fbb,
                                    const reflection::Schema* schema,
                                    const reflection::Object* tableDef,
                                    const Table* sourceTable,
                                    std::span<const uint32_t> path,
                                    bool useStringPooling);

flatbuffers::Offset<const flatbuffers::Table*> fbrequest::copyTable(
    FlatBufferBuilder& fbb, const reflection::Schema* schema,
    const uint8_t* ptr, bool useStringPooling) {
  const auto table = Offset<const Table*>(
      copyTableDeep(fbb, schema, schema->root_table(),
                    flatbuffers::GetAnyRoot(ptr), useStringPooling));
  fbb.Finish(table);
  return table;
}

// The widest scalar a flatbuffer or a flexbuffer stores. A flexbuffer aligns
// each value to its width from its start, and a finished flatbuffer is padded
// so its fields align from its start too, so either is read in place only
// from an address on this boundary.
constexpr std::size_t kPayloadAlignment = 8;

struct alignas(kPayloadAlignment) PayloadWord {
  std::array<uint8_t, kPayloadAlignment> bytes;
};

static bool isPayloadAligned(std::span<const uint8_t> bytes) {
  return reinterpret_cast<std::uintptr_t>(bytes.data()) % kPayloadAlignment
         == 0;
}

// `bytes`, or a copy of them in `storage` where they do not start on a
// kPayloadAlignment boundary. A buffer nested in a request is only as aligned
// as what carries it: a [uint8] vector's data is 4-byte aligned within the
// request buffer, and a flexbuffer blob's only to the width of its length
// prefix, often 1 byte. The span returned is valid while both `bytes` and
// `storage` are.
static std::span<const uint8_t> alignPayload(
    std::span<const uint8_t> bytes, std::vector<PayloadWord>& storage) {
  if (bytes.empty() or isPayloadAligned(bytes)) {
    return bytes;
  }
  storage.resize((bytes.size() + kPayloadAlignment - 1) / kPayloadAlignment);
  std::memcpy(storage.data(), bytes.data(), bytes.size());
  return {reinterpret_cast<const uint8_t*>(storage.data()), bytes.size()};
}

// The root table of the flatbuffer `payload` holds, which must already be
// aligned (see alignPayload).
static const Table& payloadTableRoot(std::span<const uint8_t> payload) {
  assert(payload.empty() or isPayloadAligned(payload));
  return *GetAnyRoot(payload.data());
}

// The root of the flexbuffer `payload` holds, which must already be aligned
// (see alignPayload).
static flexbuffers::Reference payloadFlexRoot(
    std::span<const uint8_t> payload) {
  assert(payload.empty() or isPayloadAligned(payload));
  return flexbuffers::GetRoot(payload.data(), payload.size());
}

// The vector `fieldDef` names in `table`, or null where the buffer stores no
// such vector, or no `table` at all.
template <typename VectorType>
static const VectorType* getFieldVector(const Table* table,
                                        const reflection::Field* fieldDef) {
  return (table != nullptr)
             ? table->GetPointer<const VectorType*>(fieldDef->offset())
             : nullptr;
}

template <typename IntegerType>
const static Vector<IntegerType>* getFieldVI(
    const Table* table, const reflection::Field* fieldDef) {
  return getFieldVector<Vector<IntegerType>>(table, fieldDef);
}

static const Vector<Offset<Table>>* getFieldVT(
    const Table* table, const reflection::Field* fieldDef) {
  return getFieldVector<Vector<Offset<Table>>>(table, fieldDef);
}

static const Vector<Offset<String>>* getFieldVS(
    const Table* table, const reflection::Field* fieldDef) {
  return getFieldVector<Vector<Offset<String>>>(table, fieldDef);
}

static const VectorOfAny* getFieldVA(const Table* table,
                                     const reflection::Field* fieldDef) {
  return getFieldVector<VectorOfAny>(table, fieldDef);
}

// TODO: The type vector should be retrievable with
//     auto type_vec =
//         table.GetPointer<Vector<uint8_t> *>(
//             vec_field.offset() - sizeof(voffset_t));
// Clean up uses of this function.
static const reflection::Field* getUnionTypeField(
    const reflection::Object* tableDef, const reflection::Field* fieldDef) {
  return tableDef->fields()->LookupByKey(
      (fieldDef->name()->str() + UnionTypeFieldSuffix()).c_str());
}

// The discriminant of `fieldDef`'s union as `sourceTable` stores it.
static uint8_t storedUnionMember(const reflection::Object* tableDef,
                                 const reflection::Field* fieldDef,
                                 const Table* sourceTable) {
  const auto* typeFieldDef = getUnionTypeField(tableDef, fieldDef);
  return (sourceTable != nullptr and typeFieldDef != nullptr)
             ? GetFieldI<uint8_t>(*sourceTable, *typeFieldDef)
             : uint8_t{0};
}

// The object, table or struct, the member of `fieldDef`'s union with
// discriminant `enumIndex` holds, or null where that member holds none: NONE,
// or a discriminant the union does not declare. Taken as the path carries it,
// so a value past a discriminant's 8 bits is undeclared rather than truncated
// to another.
static const reflection::Object* getUnionMemberDef(
    const reflection::Schema* schema, const reflection::Field* fieldDef,
    uint32_t enumIndex) {
  const auto* enumDef = schema->enums()->Get(fieldDef->type()->index());
  const auto* enumValue = enumDef->values()->LookupByKey(enumIndex);
  if (enumValue == nullptr or enumValue->union_type() == nullptr
      or enumValue->union_type()->base_type() != reflection::Obj) {
    return nullptr;
  }
  return schema->objects()->Get(enumValue->union_type()->index());
}

// The table the member with discriminant `enumIndex` holds, as
// `getUnionMemberDef` answers it, or null for a member holding a struct as
// well: a struct member, which a schema generated for C++ alone may have, is
// copied whole and never edited.
static const reflection::Object* getUnionTableDefFromEnumIndex(
    const reflection::Schema* schema, const reflection::Field* fieldDef,
    uint32_t enumIndex) {
  const auto* memberDef = getUnionMemberDef(schema, fieldDef, enumIndex);
  return (memberDef != nullptr and not memberDef->is_struct()) ? memberDef
                                                               : nullptr;
}

// An element of a vector of tables or of unions has to be SOME table: a
// vector of offsets has no way to store a missing one.
static uoffset_t createEmptyTable(FlatBufferBuilder& fbb) {
  return fbb.EndTable(fbb.StartTable());
}

// A copy of `stored`, the value of the member of `fieldDef`'s union with
// discriminant `member`: a table copied deep, a struct as its bytes, a string
// as a string. The classify pass refuses a request that would copy a member
// the union does not declare, or a payload stored under NONE.
static uoffset_t copyUnionMember(FlatBufferBuilder& fbb,
                                 const reflection::Schema* schema,
                                 const reflection::Field* fieldDef,
                                 uint32_t member, const Table* stored,
                                 bool useStringPooling) {
  return copyUnionValue(fbb, schema, fieldDef->type()->index(), member, stored,
                        useStringPooling);
}

static uoffset_t createString(flatbuffers::FlatBufferBuilder& fbb,
                              std::string_view str, bool useStringPooling) {
  return useStringPooling ? fbb.CreateSharedString(str).o
                          : fbb.CreateString(str).o;
}

static uoffset_t createString(flatbuffers::FlatBufferBuilder& fbb,
                              const flatbuffers::String* str,
                              bool useStringPooling) {
  return createString(fbb, std::string_view(str->c_str(), str->size()),
                      useStringPooling);
}

static uoffset_t createString(flatbuffers::FlatBufferBuilder& fbb,
                              const flexbuffers::String& str,
                              bool useStringPooling) {
  return createString(fbb, std::string_view(str.c_str(), str.size()),
                      useStringPooling);
}

static void copyInline(FlatBufferBuilder& fbb,
                       const reflection::Field* fieldDef, const Table* table,
                       size_t align, size_t size) {
  fbb.Align(align);
  fbb.PushBytes(table->GetStruct<const uint8_t*>(fieldDef->offset()), size);
  fbb.TrackField(fieldDef->offset(), fbb.GetSize());
}

// ---------------------------------------------------------------------------
// Structs
//
// A struct is stored INLINE in whatever holds it, with no vtable and no offset
// of its own: its members sit at fixed byte offsets and are all always
// present. So a struct is never edited in place -- the whole of it is built as
// an image and pushed, and a request naming one member is the source image
// with that member overwritten. A struct the buffer does not store starts from
// zeroes, which is what every member of an absent struct reads as.
// ---------------------------------------------------------------------------

// The image of the struct `fieldDef` names after `payload` is written into it
// at `path`: the source's bytes where the buffer stores one, zeroes where it
// does not. Empty where the struct cannot take the payload (see
// writeStructPayload), which `classifyPut` refuses before anything is built.
static std::optional<std::vector<uint8_t>> buildStructImage(
    const reflection::Schema* schema, const reflection::Object* structDef,
    const reflection::Field* fieldDef, const Table* sourceTable,
    bool fieldInSource, std::span<const uint8_t> payload,
    std::span<const uint32_t> path) {
  auto image = std::vector<uint8_t>(structDef->bytesize(), uint8_t{0});
  if (fieldInSource) {
    const auto* stored =
        sourceTable->GetStruct<const uint8_t*>(fieldDef->offset());
    if (stored != nullptr) {
      std::memcpy(image.data(), stored, image.size());
    }
  }
  const auto value = payloadFlexRoot(payload);
  if (not writeStructPayload(schema, structDef, image, path, value)) {
    return std::nullopt;
  }
  return image;
}

// ---------------------------------------------------------------------------
// Vectors of structs
//
// A vector of structs holds its elements inline, each the struct's bytes at
// its size, so the vector is rebuilt whole around any element a request
// writes: each element is an image as a struct field is, and the vector is
// laid out at the struct's alignment, or what the field's attributes ask.
// ---------------------------------------------------------------------------

// Builds the vector `fieldDef` names from `elements`, a whole-vector payload
// the classify pass accepted (see writeStructVectorPayload).
static uoffset_t createStructVector(FlatBufferBuilder& fbb,
                                    const reflection::Schema* schema,
                                    const reflection::Field* fieldDef,
                                    const reflection::Object* structDef,
                                    flexbuffers::Vector elements) {
  const auto size = static_cast<size_t>(structDef->bytesize());
  auto image = std::vector<uint8_t>(size);
  fbb.StartVector(elements.size(), size, vectorAlignment(schema, fieldDef));
  for (auto i = elements.size(); i > 0;) {
    std::ranges::fill(image, uint8_t{0});
    [[maybe_unused]] const auto written =
        writeStructVectorPayload(schema, structDef, image, elements[--i]);
    assert(written);
    fbb.PushBytes(image.data(), size);
  }
  return fbb.EndVector(elements.size());
}

// Rebuilds the vector of structs `fieldDef` names in `sourceTable` with
// `payload` written into element `path.front()` at the member the rest of
// `path` names, or into the element by map where it names none. An index at
// or past the end grows the vector to reach it, the elements between zeroed,
// as are the members of the new element the payload does not name.
static uoffset_t applyRequestPut_GenerateStructVectorOffset(
    FlatBufferBuilder& fbb, const reflection::Schema* schema,
    const reflection::Field* fieldDef, const reflection::Object* structDef,
    const Table* sourceTable, std::span<const uint32_t> path,
    std::span<const uint8_t> payload) {
  const auto* vector = getFieldVA(sourceTable, fieldDef);
  const auto index = size_t{path.front()};
  const auto size = static_cast<size_t>(structDef->bytesize());
  const auto existingSize = (vector != nullptr) ? size_t{vector->size()} : 0;
  const auto* data = (vector != nullptr) ? vector->Data() : nullptr;
  auto image = std::vector<uint8_t>(size, uint8_t{0});
  if (index < existingSize) {
    std::memcpy(image.data(), data + (index * size), size);
  }
  if (not writeStructElementPayload(schema, structDef, image, path.subspan(1),
                                    payloadFlexRoot(payload))) {
    return 0;
  }
  const auto newSize = std::max(existingSize, index + 1);
  fbb.StartVector(newSize, size, vectorAlignment(schema, fieldDef));
  // Pushed back to front: the elements after the one written, it, the zeroed
  // elements between the stored ones and it, and the elements before it.
  if (existingSize > index + 1) {
    fbb.PushBytes(data + ((index + 1) * size),
                  (existingSize - index - 1) * size);
  }
  fbb.PushBytes(image.data(), size);
  std::ranges::fill(image, uint8_t{0});
  for (auto i = existingSize; i < index; i++) {
    fbb.PushBytes(image.data(), size);
  }
  if (existingSize > 0) {
    fbb.PushBytes(data, std::min(existingSize, index) * size);
  }
  return fbb.EndVector(newSize);
}

static uoffset_t copyPayload(FlatBufferBuilder& fbb,
                             const reflection::Schema* schema,
                             const reflection::Field* fieldDef,
                             std::span<const uint8_t> payload,
                             std::span<const uint32_t> path,
                             bool elementInVector, bool useStringPooling) {
  assert(not elementInVector
         or fieldDef->type()->base_type() == reflection::Vector);
  const auto type = elementInVector ? fieldDef->type()->element()
                                    : fieldDef->type()->base_type();
  switch (type) {
    case reflection::UType: {
      assert(false);
      return 0;
    }
    case reflection::String: {
      const auto root = payloadFlexRoot(payload);
      assert(root.IsString());
      return createString(fbb, root.AsString(), useStringPooling);
    }
    case reflection::Vector: {
      auto elementType = fieldDef->type()->element();
      const auto root = payloadFlexRoot(payload);
      assert(root.IsVector());
      auto vector = root.AsVector();

      if (IsScalar(elementType)) {
        auto elementSize = fieldDef->type()->element_size();
        fbb.StartVector(vector.size(), elementSize,
                        vectorAlignment(schema, fieldDef));
        for (auto i = vector.size(); i > 0;) {
          fbb.Align(elementSize);
          const auto bytes = scalarBytes(vector[--i], elementType);
          fbb.PushBytes(bytes.data(), elementSize);
        }
        return fbb.EndVector(vector.size());
      }
      if (fieldDef->type()->element() == reflection::String) {
        auto strings = std::vector<std::string_view>{};
        strings.resize(vector.size());
        for (auto i = 0; i < vector.size(); i++) {
          auto str = vector[i].AsString();
          strings[i] = std::string_view(str.c_str(), str.length());
        }
        return fbb.CreateVectorOfStrings(strings.begin(), strings.end()).o;
      }
      if (fieldDef->type()->element() == reflection::Obj
          and schema->objects()->Get(fieldDef->type()->index())->is_struct()) {
        return createStructVector(
            fbb, schema, fieldDef,
            schema->objects()->Get(fieldDef->type()->index()), vector);
      }
      if (fieldDef->type()->element() == reflection::Obj) {
        auto tables = std::vector<Offset<Table>>{};
        tables.resize(vector.size());
        auto storage = std::vector<PayloadWord>{};
        for (auto i = 0; i < vector.size(); i++) {
          auto blob = vector[i].AsBlob();
          const auto* tableDef =
              schema->objects()->Get(fieldDef->type()->index());
          const auto element =
              alignPayload(std::span{blob.data(), blob.size()}, storage);
          tables[i] =
              copyTableDeep(fbb, schema, tableDef, &payloadTableRoot(element),
                            useStringPooling);
        }
        return fbb.CreateVector(tables.data(), tables.size()).o;
      }
      // Nor a vector of unions: the member each element is lives in a
      // parallel vector of discriminants that a JSON array cannot carry, and
      // the field's type index names the union's ENUM, not an object. The
      // parser refuses one.
      return 0;
    }
    case reflection::Obj: {
      const auto* subTableDef =
          schema->objects()->Get(fieldDef->type()->index());
      if (subTableDef->is_struct()) {
        // A struct has no offset of its own: it is written inline, by the
        // table builder, out of a flexbuffer payload. Reading that payload as
        // a table root here would be reading it as something it is not.
        return 0;
      }
      return copyTableDeep(fbb, schema, subTableDef, &payloadTableRoot(payload),
                           useStringPooling);
    }
    case reflection::Union: {
      assert(path.size() == 2);
      auto unionEnumIndex = path[1];
      const auto* unionTableDef =
          getUnionTableDefFromEnumIndex(schema, fieldDef, unionEnumIndex);
      if (unionTableDef == nullptr) {
        return 0;
      }
      return copyTableDeep(fbb, schema, unionTableDef,
                           &payloadTableRoot(payload), useStringPooling);
    }
    case reflection::Array: {
      assert(false);
      return 0;
    }
    default: {
      return 0;
    }
  }
}

static std::span<const uint32_t> updatePath(std::span<const uint32_t> path) {
  assert(path.size() > 0);
  return {path.last(path.size() - 1)};
}

static void copyOperationScalar(FlatBufferBuilder& fbb,
                                const reflection::Field* fieldDef,
                                std::span<const uint8_t> payload, size_t align,
                                size_t size) {
  auto baseType = fieldDef->type()->base_type();
  auto root = payloadFlexRoot(payload);
  const auto bytes = scalarBytes(root, baseType);

  fbb.Align(align);
  fbb.PushBytes(bytes.data(), size);
  fbb.TrackField(fieldDef->offset(), fbb.GetSize());
}

static uoffset_t applyRequestPut_GenerateObjectOffset(
    FlatBufferBuilder& fbb, const reflection::Schema* schema,
    const reflection::Object* /*tableDef*/, const reflection::Field* fieldDef,
    const Table* sourceTable, std::span<const uint8_t> payload,
    std::span<const uint32_t> path, bool useStringPooling) {
  const auto* subtableDef = schema->objects()->Get(fieldDef->type()->index());
  if (subtableDef->is_struct()) {
    return 0;
  }
  auto* table =
      (sourceTable != nullptr) ? GetFieldT(*sourceTable, *fieldDef) : nullptr;
  return applyRequestPut(fbb, schema, subtableDef, table, payload,
                         updatePath(path), useStringPooling);
}

static uoffset_t applyRequestPut_GenerateScalarVectorOffset(
    FlatBufferBuilder& fbb, const VectorOfAny* vector,
    std::span<const uint8_t> payload, std::span<const uint32_t> path,
    reflection::BaseType elementBaseType, size_t alignment,
    std::array<uint8_t, 8> defaultValue = std::array<uint8_t, 8>{}) {
  const auto index = size_t{path.front()};
  const auto elementSize = GetTypeSize(elementBaseType);
  auto existingVectorSize = (vector != nullptr) ? vector->size() : size_t{0};
  auto initialValues = std::min(existingVectorSize, index);
  auto newVectorSize = std::max(existingVectorSize, index + 1U);
  auto finalElements = newVectorSize - index - 1;
  auto zeroes = defaultValue;
  const auto* data = (vector != nullptr) ? vector->Data() : nullptr;

  fbb.StartVector(newVectorSize, elementSize, alignment);
  // Flatbuffers expects elements to be inserted into the vector in
  // reverse order. They are stored forward, but the internal buffer that
  // Flatbuffers uses grows downwards, so elements are added back to
  // front.

  // If there are elements after our newly inserted item, add them to the
  // vector.
  if (finalElements != 0U) {
    fbb.PushBytes(data + (elementSize * (existingVectorSize - finalElements)),
                  elementSize * finalElements);
  }

  // Add the new element itself.
  auto root = payloadFlexRoot(payload);
  const auto bytes = scalarBytes(root, elementBaseType);
  fbb.PushBytes(bytes.data(), elementSize);

  // If the new element is beyond the edge, insert 0's to pad the vector.
  for (auto i = initialValues; i < index; i++) {
    fbb.PushBytes(zeroes.data(), elementSize);
  }

  // Finally, insert existing elements from the begining up to the index
  // of the new element.
  fbb.PushBytes(data, elementSize * initialValues);
  return fbb.EndVector(newVectorSize);
}

static uoffset_t applyRequestPut_GenerateStringVectorOffset(
    flatbuffers::FlatBufferBuilder& fbb, const reflection::Schema* schema,
    const reflection::Field* fieldDef, const Table* sourceTable,
    std::span<const uint32_t>& path, std::span<const uint8_t> payload,
    bool useStringPooling) {
  // A vector the buffer does not store grows from empty, as a stored one
  // grows past its end.
  const auto* vector = getFieldVS(sourceTable, fieldDef);
  const auto existingSize = (vector != nullptr) ? vector->size() : uoffset_t{0};
  auto index = path.front();
  auto elements = std::vector<Offset<const String*>>{};
  elements.resize(std::max(existingSize, static_cast<uoffset_t>(index + 1)));
  for (uoffset_t i = 0; i < existingSize; i++) {
    if (i == index) {
      elements[i] = copyPayload(fbb, schema, fieldDef, payload, path, true,
                                useStringPooling);
    } else {
      elements[i] = createString(fbb, vector->Get(i), useStringPooling);
    }
  }
  for (uoffset_t i = existingSize; i < index; i++) {
    elements[i] = createString(fbb, "", useStringPooling);
  }
  if (index >= existingSize) {
    elements[index] = copyPayload(fbb, schema, fieldDef, payload, path, true,
                                  useStringPooling);
  }
  return fbb.CreateVector(elements).o;
}

static uoffset_t applyRequestPut_GenerateObjectVectorOffset(
    flatbuffers::FlatBufferBuilder& fbb, const reflection::Schema* schema,
    const reflection::Field* fieldDef, const Table* sourceTable,
    std::span<const uint32_t> path, std::span<const uint8_t> payload,
    bool useStringPooling) {
  const auto* subtableDef = schema->objects()->Get(fieldDef->type()->index());
  if (subtableDef->is_struct()) {
    return applyRequestPut_GenerateStructVectorOffset(
        fbb, schema, fieldDef, subtableDef, sourceTable, path, payload);
  }
  const auto* vector = getFieldVT(sourceTable, fieldDef);
  const auto existingSize = (vector != nullptr) ? vector->size() : uoffset_t{0};
  auto index = path.front();
  auto elements = std::vector<Offset<Table>>{};
  elements.resize(std::max(existingSize, index + 1U));
  for (uoffset_t i = 0; i < existingSize; i++) {
    if (i == index) {
      elements[i] =
          applyRequestPut(fbb, schema, subtableDef, vector->Get(index), payload,
                          updatePath(path), useStringPooling);
    } else {
      elements[i] = copyTableDeep(fbb, schema, subtableDef, vector->Get(i),
                                  useStringPooling);
    }
  }
  for (uoffset_t i = existingSize; i < index; i++) {
    elements[i] = createEmptyTable(fbb);
  }
  if (index >= existingSize) {
    elements[index] =
        applyRequestPut(fbb, schema, subtableDef, nullptr, payload,
                        updatePath(path), useStringPooling);
  }
  return fbb.CreateVector(elements).o;
}

// The discriminant of the first member of `fieldDef`'s union that holds a
// table, which is what the padding elements of a grown union vector are: an
// empty table is a valid one of any table member. Not necessarily 1, since a
// union's members may be given values.
static uint8_t firstTableMember(const reflection::Schema* schema,
                                const reflection::Field* fieldDef) {
  const auto* enumDef = schema->enums()->Get(fieldDef->type()->index());
  for (const auto* enumValue : *enumDef->values()) {
    const auto* memberType = enumValue->union_type();
    if (memberType != nullptr and memberType->base_type() == reflection::Obj
        and not schema->objects()->Get(memberType->index())->is_struct()) {
      return static_cast<uint8_t>(enumValue->value());
    }
  }
  return 0;
}

// `path` is `[index, member, ...]`, its member one that holds a table.
static uoffset_t applyRequestPut_GenerateUTypeVectorOffset(
    flatbuffers::FlatBufferBuilder& fbb, const reflection::Schema* schema,
    const reflection::Field* fieldDef, const Table* sourceTable,
    std::span<const uint32_t> path, std::span<const uint8_t> payload,
    bool useStringPooling) {
  const auto* vector = getFieldVA(sourceTable, fieldDef);

  // Aligned only because every payload is (see payloadFlexRoot): a 1-byte
  // wide flexbuffer has no wider value to misread.
  alignas(kPayloadAlignment) auto flexbufferValue =
      std::array<uint8_t, 3>{static_cast<uint8_t>(path[1]), 0x08, 0x01};
  auto padding = std::array<uint8_t, 8>{};
  padding.fill(firstTableMember(schema, fieldDef));
  return applyRequestPut_GenerateScalarVectorOffset(
      fbb, vector, flexbufferValue, path, reflection::UType,
      GetTypeSize(reflection::UType), padding);
}

static uoffset_t applyRequestPut_GenerateUnionVectorOffset(
    flatbuffers::FlatBufferBuilder& fbb, const reflection::Schema* schema,
    const reflection::Object* tableDef, const reflection::Field* fieldDef,
    const Table* sourceTable, std::span<const uint32_t> path,
    std::span<const uint8_t> payload, bool useStringPooling) {
  const auto* unionTypeFieldDef = getUnionTypeField(tableDef, fieldDef);
  const auto* vector = getFieldVT(sourceTable, fieldDef);
  const auto* unionTypeVector =
      getFieldVI<uint8_t>(sourceTable, unionTypeFieldDef);
  // Without both halves the buffer stores no element to keep, so the vector
  // grows from empty, as a stored one grows past its end.
  const auto existingSize =
      (vector != nullptr and unionTypeVector != nullptr)
          ? std::min(vector->size(), unionTypeVector->size())
          : uoffset_t{0};
  if (path.size() < 2) {
    // A hand-built path stopping at the element names no member to write.
    return 0;
  }
  auto index = path.front();
  path = updatePath(path);
  const auto requestedMember = path.front();
  const auto* requestedDef =
      getUnionTableDefFromEnumIndex(schema, unionTypeFieldDef, requestedMember);
  // A member that holds no table has nothing to write, as for a single union,
  // and the vector keeps what the buffer stores.
  if (requestedDef == nullptr) {
    return 0;
  }
  auto elements = std::vector<Offset<Table>>{};
  elements.resize(std::max(existingSize, index + 1U));
  const auto writeRequested = [&](const Table* stored) -> uoffset_t {
    return applyRequestPut(fbb, schema, requestedDef, stored, payload,
                           updatePath(path), useStringPooling);
  };
  for (uoffset_t i = 0; i < existingSize; i++) {
    if (i == index) {
      // As for a single union: the stored element is a source for the fields
      // the request does not name only while it is the member being written.
      elements[i] = writeRequested(unionTypeVector->Get(i) == requestedMember
                                       ? vector->Get(i)
                                       : nullptr);
      continue;
    }
    elements[i] =
        copyUnionMember(fbb, schema, unionTypeFieldDef, unionTypeVector->Get(i),
                        vector->Get(i), useStringPooling);
  }
  for (uoffset_t i = existingSize; i < index; i++) {
    elements[i] = createEmptyTable(fbb);
  }
  if (index >= existingSize) {
    elements[index] = writeRequested(nullptr);
  }
  return fbb.CreateVector(elements).o;
}

using ApplyPutRequestFunc = uoffset_t (*)(
    FlatBufferBuilder& fbb, const reflection::Schema* schema,
    const reflection::Object* /*object*/, const reflection::Field* fieldDef,
    const Table* sourceTable, std::span<const uint8_t> payload,
    std::span<const uint32_t> path, bool useStringPooling);

static uoffset_t applyRequestPut_GenerateVectorOffset(
    FlatBufferBuilder& fbb, const reflection::Schema* schema,
    const reflection::Object* tableDef, const reflection::Field* fieldDef,
    const Table* sourceTable, std::span<const uint8_t> payload,
    std::span<const uint32_t> path, bool useStringPooling) {
  path = updatePath(path);
  const auto elementBaseType = fieldDef->type()->element();
  switch (elementBaseType) {
    case reflection::Union: {
      return applyRequestPut_GenerateUnionVectorOffset(
          fbb, schema, tableDef, fieldDef, sourceTable, path, payload,
          useStringPooling);
    }
    case reflection::String: {
      return applyRequestPut_GenerateStringVectorOffset(
          fbb, schema, fieldDef, sourceTable, path, payload, useStringPooling);
    }
    case reflection::Obj: {
      return applyRequestPut_GenerateObjectVectorOffset(
          fbb, schema, fieldDef, sourceTable, path, payload, useStringPooling);
    }
    default: {
      return applyRequestPut_GenerateScalarVectorOffset(
          fbb, getFieldVA(sourceTable, fieldDef), payload, path,
          elementBaseType, vectorAlignment(schema, fieldDef));
    }
  }
}

static uoffset_t applyRequestPut_GenerateUnionOffset(
    FlatBufferBuilder& fbb, const reflection::Schema* schema,
    const reflection::Object* tableDef, const reflection::Field* fieldDef,
    const Table* sourceTable, std::span<const uint8_t> payload,
    std::span<const uint32_t> path, bool useStringPooling) {
  if (path.size() < 2) {
    // A hand-built path stopping at the union names no member to write.
    return 0;
  }
  path = updatePath(path);
  const auto requestedMember = path.front();
  const auto* subtableDef =
      getUnionTableDefFromEnumIndex(schema, fieldDef, requestedMember);
  if (subtableDef == nullptr) {
    return 0;
  }
  path = updatePath(path);
  // The MEMBER is what the member's own fields are copied from, and only
  // while the stored member is the one being written. The table holding the
  // union is not it: reading the member's fields out of the parent lands at
  // the parent's offsets, which is another field or nothing at all. A request
  // that switches member has nothing to copy, and must not carry the old
  // member's bytes into the new one.
  const Table* storedMember = nullptr;
  if (sourceTable != nullptr and sourceTable->CheckField(fieldDef->offset())
      and storedUnionMember(tableDef, fieldDef, sourceTable)
              == requestedMember) {
    storedMember = GetFieldT(*sourceTable, *fieldDef);
  }
  return applyRequestPut(fbb, schema, subtableDef, storedMember, payload, path,
                         useStringPooling);
}

// A copy of the field `fieldDef` names, as `sourceTable` stores it, for every
// field built from an offset; 0 for one stored inline.
static uoffset_t copyStoredField(FlatBufferBuilder& fbb,
                                 const reflection::Schema* schema,
                                 const reflection::Object* tableDef,
                                 const reflection::Field* fieldDef,
                                 const Table* sourceTable,
                                 bool useStringPooling) {
  return copyFieldOffset(fbb, schema, tableDef, sourceTable, fieldDef,
                         useStringPooling);
}

static uoffset_t applyRequestPut_GenerateFieldOffset(
    FlatBufferBuilder& fbb, const reflection::Schema* schema,
    const reflection::Object* tableDef, const reflection::Field* fieldDef,
    const Table* sourceTable, std::span<const uint8_t> payload,
    std::span<const uint32_t> path, bool useStringPooling) {
  const auto baseType = fieldDef->type()->base_type();

  // Special case for Vectors of UTypes - they will be handled elsewhere.
  if (baseType == reflection::Vector
      and fieldDef->type()->element() == reflection::UType) {
    return 0;
  }

  // If we have reached the field that we are going to overwrite, simply
  // overwrite it.
  const bool terminalField =
      path.size() == (baseType == reflection::Union ? 2 : 1)
      and path.front() == fieldDef->offset();
  if (terminalField) {
    return copyPayload(fbb, schema, fieldDef, payload, path, false,
                       useStringPooling);
  }

  // If the field is in the path of the new value(s) that we are adding,
  // generate the object. This may result in objects or arrays that are mostly
  // empty, except for the specific field that is being set.
  const bool inPath = path.front() == fieldDef->offset();
  if (inPath) {
    static const auto funcs =
        std::array<ApplyPutRequestFunc, reflection::MaxBaseType>{
            /* None   */ nullptr,
            /* UType  */ nullptr,
            /* Bool   */ nullptr,
            /* Byte   */ nullptr,
            /* UByte  */ nullptr,
            /* Short  */ nullptr,
            /* UShort */ nullptr,
            /* Int    */ nullptr,
            /* UInt   */ nullptr,
            /* Long   */ nullptr,
            /* ULong  */ nullptr,
            /* Float  */ nullptr,
            /* Double */ nullptr,
            /* String */ nullptr,
            /* Vector */ applyRequestPut_GenerateVectorOffset,
            /* Obj    */ applyRequestPut_GenerateObjectOffset,
            /* Union  */ applyRequestPut_GenerateUnionOffset,
            /* Array  */ nullptr,
            // Refused by classifyPutInVector.
            /* Vector64 */ nullptr,
        };
    auto func = funcs[baseType];
    assert(func);
    return func(fbb, schema, tableDef, fieldDef, sourceTable, payload, path,
                useStringPooling);
  }

  // A field the request does not reach is copied as the source stores it.
  auto fieldInSource =
      (sourceTable != nullptr) and sourceTable->CheckField(fieldDef->offset());
  if (fieldInSource) {
    return copyStoredField(fbb, schema, tableDef, fieldDef, sourceTable,
                           useStringPooling);
  }
  return 0;
}

// Whether a put along `path` writes the union `fieldDef` names: the path
// reaches it, and names a member that holds a table. Otherwise the union
// keeps what the buffer stores, discriminant and value together.
static bool putWritesUnionMember(const reflection::Schema* schema,
                                 const reflection::Field* fieldDef,
                                 std::span<const uint32_t> path) {
  return path.size() >= 2 and path.front() == fieldDef->offset()
         and getUnionTableDefFromEnumIndex(schema, fieldDef, path[1])
                 != nullptr;
}

// Whether `applyRequestPut_BuildTableField` reads an offset for `fieldDef`
// (two, for a vector of unions: its discriminants', then its own). Both passes
// decide it here: the build pass reads the offsets in order, so one the
// generate pass did not push hands every later field its neighbour's.
static bool putFieldTakesOffset(const reflection::Schema* schema,
                                const reflection::Field* fieldDef, bool inPath,
                                bool fieldInSource) {
  if (not inPath and not fieldInSource) {
    return false;
  }
  const auto* type = fieldDef->type();
  return isOffsetField(schema, fieldDef)
         and not(type->base_type() == reflection::Vector
                 and type->element() == reflection::UType);
}

static std::vector<uoffset_t> applyRequestPut_GenerateOffsets(
    FlatBufferBuilder& fbb, const reflection::Schema* schema,
    const reflection::Object* tableDef, const Table* sourceTable,
    std::span<const uint8_t> payload, std::span<const uint32_t> path,
    bool useStringPooling) {
  auto offsets = std::vector<uoffset_t>{};
  for (const auto* fieldDef : *tableDef->fields()) {
    auto offset = applyRequestPut_GenerateFieldOffset(
        fbb, schema, tableDef, fieldDef, sourceTable, payload, path,
        useStringPooling);
    const auto inPath = path.front() == fieldDef->offset();
    const auto fieldInSource = (sourceTable != nullptr)
                               and sourceTable->CheckField(fieldDef->offset());
    if (not putFieldTakesOffset(schema, fieldDef, inPath, fieldInSource)) {
      continue;
    }
    // A put that could build nothing for the field it names leaves the field
    // as the buffer stores it.
    const auto wrote = offset != 0;
    if (not wrote and fieldInSource) {
      offset = copyStoredField(fbb, schema, tableDef, fieldDef, sourceTable,
                               useStringPooling);
    }

    // Special case for Vectors of unions, since all Union vectors must also be
    // accompanied by a UType vector.
    if (fieldDef->type()->base_type() == reflection::Vector
        and fieldDef->type()->element() == reflection::Union) {
      uoffset_t unionTypeOffset = 0;
      const auto* unionTypeFieldDef = getUnionTypeField(tableDef, fieldDef);
      if (inPath and wrote) {
        unionTypeOffset = applyRequestPut_GenerateUTypeVectorOffset(
            fbb, schema, unionTypeFieldDef, sourceTable, updatePath(path),
            payload, useStringPooling);
      } else if (offset != 0) {
        unionTypeOffset =
            copyStoredField(fbb, schema, tableDef, unionTypeFieldDef,
                            sourceTable, useStringPooling);
      }
      offsets.push_back(unionTypeOffset);
    }
    offsets.push_back(offset);
  }
  return offsets;
}

// Reads the offsets `applyRequestPut_GenerateOffsets` pushed, in field order,
// for exactly the fields `putFieldTakesOffset` names. A null one is skipped by
// `AddOffset`, which leaves the field absent.
static void applyRequestPut_BuildTableField(
    flatbuffers::FlatBufferBuilder& fbb, const reflection::Schema* schema,
    const reflection::Object* tableDef, const reflection::Field* fieldDef,
    const Table* sourceTable, std::span<const uint8_t> payload,
    std::span<const uint32_t> path,
    const std::vector<flatbuffers::uoffset_t>& offsets, size_t& offsetIndex) {
  auto inPath = path.front() == fieldDef->offset();
  auto fieldInSource =
      (sourceTable != nullptr) and sourceTable->CheckField(fieldDef->offset());
  auto baseType = fieldDef->type()->base_type();
  const auto takesOffset =
      putFieldTakesOffset(schema, fieldDef, inPath, fieldInSource);

  switch (baseType) {
    case reflection::UType: {
      // Do nothing, the union will handle it.
      break;
    }
    case reflection::String:
    case reflection::Vector:
    case reflection::Vector64: {
      if (takesOffset) {
        if (fieldDef->type()->element() == reflection::Union) {
          const auto* unionTypeFieldDef = getUnionTypeField(tableDef, fieldDef);
          fbb.AddOffset(unionTypeFieldDef->offset(),
                        Offset<void>(offsets[offsetIndex++]));
        }
        addFieldOffset(fbb, fieldDef, offsets[offsetIndex++]);
      }
      break;
    }
    case reflection::Obj: {
      if (takesOffset) {
        fbb.AddOffset(fieldDef->offset(), Offset<void>(offsets[offsetIndex++]));
      } else if (inPath or fieldInSource) {
        const auto* subtableDef =
            schema->objects()->Get(fieldDef->type()->index());
        if (inPath) {
          // The struct is rebuilt rather than copied: it is inline and has no
          // vtable, so there is nothing to overwrite a single member of in
          // place. `copyInline` reads the source unconditionally, which for a
          // struct the buffer does not store reads a field that is not there.
          const auto image =
              buildStructImage(schema, subtableDef, fieldDef, sourceTable,
                               fieldInSource, payload, updatePath(path));
          if (image.has_value()) {
            fbb.Align(subtableDef->minalign());
            fbb.PushBytes(image->data(), image->size());
            fbb.TrackField(fieldDef->offset(), fbb.GetSize());
          } else if (fieldInSource) {
            // A write that did not happen leaves the stored struct as it was.
            copyInline(fbb, fieldDef, sourceTable, subtableDef->minalign(),
                       subtableDef->bytesize());
          }
        } else {
          copyInline(fbb, fieldDef, sourceTable, subtableDef->minalign(),
                     subtableDef->bytesize());
        }
      }
      break;
    }
    case reflection::Union: {
      // The discriminant goes with the value: the requested member's where
      // the put wrote one, the stored one where the union was copied.
      const auto writesMember = putWritesUnionMember(schema, fieldDef, path);
      if (writesMember or fieldInSource) {
        const auto* unionEnumFieldDef = getUnionTypeField(tableDef, fieldDef);
        auto enumValue =
            writesMember ? static_cast<uint8_t>(path[1])
                         : GetFieldI<uint8_t>(*sourceTable, *unionEnumFieldDef);
        auto size = GetTypeSize(unionEnumFieldDef->type()->base_type());

        assert(size == 1);
        fbb.Align(size);
        fbb.PushBytes(reinterpret_cast<uint8_t*>(&enumValue), size);
        fbb.TrackField(unionEnumFieldDef->offset(), fbb.GetSize());
      }
      if (takesOffset) {
        fbb.AddOffset(fieldDef->offset(), Offset<void>(offsets[offsetIndex++]));
      }
      break;
    }
    default: {  // Scalars.
      auto size = GetTypeSize(baseType);
      if (inPath) {
        copyOperationScalar(fbb, fieldDef, payload, size, size);
      } else if (fieldInSource) {
        copyInline(fbb, fieldDef, sourceTable, size, size);
      }
      break;
    }
  }
}

static uoffset_t applyRequestPut_BuildTable(
    FlatBufferBuilder& fbb, const reflection::Schema* schema,
    const reflection::Object* tableDef, const Table* sourceTable,
    std::span<const uint8_t> payload, std::span<const uint32_t> path,
    const std::vector<uoffset_t>& offsets) {
  auto start = tableDef->is_struct() ? fbb.StartStruct(tableDef->minalign())
                                     : fbb.StartTable();
  auto offsetIndex = size_t{0};
  for (const auto* fieldDef : *tableDef->fields()) {
    applyRequestPut_BuildTableField(fbb, schema, tableDef, fieldDef,
                                    sourceTable, payload, path, offsets,
                                    offsetIndex);
  }
  FLATBUFFERS_ASSERT(offsetIndex == offsets.size());
  if (tableDef->is_struct()) {
    fbb.ClearOffsets();
    return fbb.EndStruct();
  }
  return fbb.EndTable(start);
}

// Rebuilds `sourceTable` with `payload` written at `path`, copying every
// field the path does not reach.
uoffset_t applyRequestPut(FlatBufferBuilder& fbb,
                          const reflection::Schema* schema,
                          const reflection::Object* tableDef,
                          const Table* sourceTable,
                          std::span<const uint8_t> payload,
                          const std::span<const uint32_t> path,
                          bool useStringPooling) {
  if (path.empty()) {
    return copyTableDeep(fbb, schema, tableDef, &payloadTableRoot(payload),
                         useStringPooling);
  }
  // Before we can construct the table, we have to first generate any
  // subobjects, and collect their offsets.
  auto offsets = applyRequestPut_GenerateOffsets(
      fbb, schema, tableDef, sourceTable, payload, path, useStringPooling);

  // Now we can build the actual table from either offsets or scalar data.
  return applyRequestPut_BuildTable(fbb, schema, tableDef, sourceTable, payload,
                                    path, offsets);
}

// ---------------------------------------------------------------------------
// Classification
//
// A request's path is resolved against the schema and the buffer before
// anything is built, so that one which does not make the edit it names leaves
// the caller's builder as it was. The builders below assume a request this
// classified Applied.
// ---------------------------------------------------------------------------

using fbrequest::ApplyOutcome;

// The field of `objectDef` at vtable offset (or, in a struct, byte offset)
// `offset`, or null where it declares none.
static const reflection::Field* fieldAtOffset(
    const reflection::Object* objectDef, uint32_t offset) {
  for (const auto* fieldDef : *objectDef->fields()) {
    if (fieldDef->offset() == offset) {
      return fieldDef;
    }
  }
  return nullptr;
}

// A union's discriminator, or a vector of unions' vector of them, which is
// written only with the union beside it.
static bool isDiscriminator(const reflection::Field* fieldDef) {
  const auto* type = fieldDef->type();
  return type->base_type() == reflection::UType
         or (type->base_type() == reflection::Vector
             and type->element() == reflection::UType);
}

// Whether `table` stores the field `fieldDef` names; false for a null `table`.
static bool storesField(const Table* table, const reflection::Field* fieldDef) {
  return table != nullptr and table->CheckField(fieldDef->offset());
}

// The table `fieldDef` names in `table`, or null where `table` is null or does
// not store the field.
static const Table* storedSubtable(const Table* table,
                                   const reflection::Field* fieldDef) {
  return storesField(table, fieldDef) ? GetFieldT(*table, *fieldDef) : nullptr;
}

// Whether `payload` is a well-formed flexbuffer whose root `fits` accepts. A
// hand-built payload is read only once it verifies.
template <typename Fits>
static bool flexPayloadFits(std::span<const uint8_t> payload, Fits fits) {
  return not payload.empty()
         and flexbuffers::VerifyBuffer(payload.data(), payload.size())
         and fits(payloadFlexRoot(payload));
}

static ApplyOutcome appliedIf(bool applied) {
  return applied ? ApplyOutcome::Applied : ApplyOutcome::Refused;
}

static ApplyOutcome storedIf(bool stored) {
  return stored ? ApplyOutcome::Applied : ApplyOutcome::NotStored;
}

static ApplyOutcome classifyDelete(const reflection::Schema* schema,
                                   const reflection::Object* tableDef,
                                   const Table* table,
                                   std::span<const uint32_t> path);

// `outcome`, or Refused where it is Applied but the apply would copy, unedited,
// something it cannot copy as it went in (see copiesFaithfully).
static ApplyOutcome refusedUnlessFaithful(ApplyOutcome outcome,
                                          bool copiesFaithfully) {
  return (outcome == ApplyOutcome::Applied and not copiesFaithfully)
             ? ApplyOutcome::Refused
             : outcome;
}

// Whether every element but the one at `index` of the vector `fieldDef` names
// in `table` copies faithfully, as an edit of element `index` copies the rest.
// A vector of unions must also pair each element with one discriminant, since
// the rebuild keeps only the elements both vectors hold. True where `table`
// is null.
static bool otherElementsCopyFaithfully(const reflection::Schema* schema,
                                        const reflection::Object* tableDef,
                                        const reflection::Field* fieldDef,
                                        const Table* table, uint64_t index) {
  if (table == nullptr) {
    return true;
  }
  const auto* type = fieldDef->type();
  if (type->element() == reflection::Obj) {
    const auto* elementDef = schema->objects()->Get(type->index());
    const auto* vector = getFieldVT(table, fieldDef);
    if (elementDef->is_struct() or vector == nullptr) {
      return true;
    }
    for (uoffset_t i = 0; i < vector->size(); i++) {
      if (i != index
          and not copiesFaithfully(schema, elementDef, vector->Get(i))) {
        return false;
      }
    }
    return true;
  }
  if (type->element() == reflection::Union) {
    const auto* vector = getFieldVT(table, fieldDef);
    const auto* types =
        getFieldVI<uint8_t>(table, getUnionTypeField(tableDef, fieldDef));
    if (vector == nullptr and types == nullptr) {
      return true;
    }
    if (vector == nullptr or types == nullptr
        or vector->size() != types->size()) {
      return false;
    }
    for (uoffset_t i = 0; i < vector->size(); i++) {
      if (i != index
          and not unionValueCopiesFaithfully(schema, type->index(),
                                             types->Get(i), vector->Get(i))) {
        return false;
      }
    }
  }
  return true;
}

// `rest` is the path past `fieldDef`, a vector field of `tableDef`; `table` is
// null where the buffer stores no such vector.
static ApplyOutcome classifyDeleteInVector(const reflection::Schema* schema,
                                           const reflection::Object* tableDef,
                                           const reflection::Field* fieldDef,
                                           const Table* table,
                                           std::span<const uint32_t> rest) {
  if (rest.empty()) {
    return storedIf(table != nullptr);
  }
  if (isWideOffsetField(fieldDef)) {
    // Its elements are read through 32-bit accessors; only the whole vector
    // is removed.
    return ApplyOutcome::Refused;
  }
  const auto index = rest.front();
  const auto deeper = rest.subspan(1);
  const auto elementType = fieldDef->type()->element();
  const auto othersFaithful =
      otherElementsCopyFaithfully(schema, tableDef, fieldDef, table, index);
  if (elementType == reflection::Obj) {
    const auto* elementDef = schema->objects()->Get(fieldDef->type()->index());
    if (elementDef->is_struct()) {
      // A struct cannot lose a member.
      if (not deeper.empty()) {
        return ApplyOutcome::Refused;
      }
      const auto* vector = getFieldVA(table, fieldDef);
      return storedIf(vector != nullptr and index < vector->size());
    }
    const auto* vector = getFieldVT(table, fieldDef);
    const auto* element = (vector != nullptr and index < vector->size())
                              ? vector->Get(index)
                              : nullptr;
    return refusedUnlessFaithful(
        deeper.empty() ? storedIf(element != nullptr)
                       : classifyDelete(schema, elementDef, element, deeper),
        othersFaithful);
  }
  if (elementType == reflection::Union) {
    const auto* vector = getFieldVT(table, fieldDef);
    const auto* types =
        getFieldVI<uint8_t>(table, getUnionTypeField(tableDef, fieldDef));
    const auto held = vector != nullptr and types != nullptr
                      and index < std::min(vector->size(), types->size());
    if (deeper.empty()) {
      return refusedUnlessFaithful(storedIf(held), othersFaithful);
    }
    const auto member = deeper.front();
    const auto* memberDef =
        getUnionTableDefFromEnumIndex(schema, fieldDef, member);
    if (memberDef == nullptr) {
      return ApplyOutcome::Refused;
    }
    const auto holdsMember = held and types->Get(index) == member;
    if (deeper.size() == 1) {
      return refusedUnlessFaithful(storedIf(holdsMember), othersFaithful);
    }
    return refusedUnlessFaithful(
        classifyDelete(schema, memberDef,
                       holdsMember ? vector->Get(index) : nullptr,
                       deeper.subspan(1)),
        othersFaithful);
  }
  // A scalar or a string, which holds nothing to delete.
  if (not deeper.empty()) {
    return ApplyOutcome::Refused;
  }
  const auto* vector = getFieldVA(table, fieldDef);
  return storedIf(vector != nullptr and index < vector->size());
}

// What a delete of `fieldDef`, a field of `tableDef`, or of what `rest` names
// inside it, comes to in `table`.
static ApplyOutcome classifyDeleteField(const reflection::Schema* schema,
                                        const reflection::Object* tableDef,
                                        const reflection::Field* fieldDef,
                                        const Table* table,
                                        std::span<const uint32_t> rest) {
  const auto stored = storesField(table, fieldDef);
  const auto* type = fieldDef->type();
  switch (type->base_type()) {
    case reflection::Obj: {
      const auto* subDef = schema->objects()->Get(type->index());
      if (rest.empty()) {
        return storedIf(stored);
      }
      if (subDef->is_struct()) {
        return ApplyOutcome::Refused;
      }
      return classifyDelete(schema, subDef, storedSubtable(table, fieldDef),
                            rest);
    }
    case reflection::Union: {
      if (rest.empty()) {
        return storedIf(stored);
      }
      const auto member = rest.front();
      const auto* memberDef =
          getUnionTableDefFromEnumIndex(schema, fieldDef, member);
      if (memberDef == nullptr) {
        return ApplyOutcome::Refused;
      }
      const auto held =
          stored and storedUnionMember(tableDef, fieldDef, table) == member;
      if (rest.size() == 1) {
        return storedIf(held);
      }
      return classifyDelete(schema, memberDef,
                            held ? GetFieldT(*table, *fieldDef) : nullptr,
                            rest.subspan(1));
    }
    case reflection::Vector:
    case reflection::Vector64:
      return classifyDeleteInVector(schema, tableDef, fieldDef,
                                    stored ? table : nullptr, rest);
    case reflection::Array:
      return ApplyOutcome::Refused;
    default:
      return rest.empty() ? storedIf(stored) : ApplyOutcome::Refused;
  }
}

// What a delete along `path` comes to in `table`, a table of `tableDef` or
// null where the buffer stores none. The path is checked against the schema
// whether or not what it names is stored, so a delete the schema cannot serve
// is Refused even where there is nothing to delete. The fields of `table` the
// path does not name are copied, so a delete is Refused where one of them
// does not copy faithfully.
static ApplyOutcome classifyDelete(const reflection::Schema* schema,
                                   const reflection::Object* tableDef,
                                   const Table* table,
                                   std::span<const uint32_t> path) {
  if (path.empty()) {
    return ApplyOutcome::Refused;
  }
  const auto* fieldDef = fieldAtOffset(tableDef, path.front());
  if (fieldDef == nullptr or isDiscriminator(fieldDef)) {
    return ApplyOutcome::Refused;
  }
  return refusedUnlessFaithful(
      classifyDeleteField(schema, tableDef, fieldDef, table, path.subspan(1)),
      copiesFaithfully(schema, tableDef, table, fieldDef));
}

static ApplyOutcome classifyPut(const reflection::Schema* schema,
                                const reflection::Object* tableDef,
                                const Table* table,
                                std::span<const uint8_t> payload,
                                std::span<const uint32_t> path,
                                uint64_t growthBudget);

// Whether `payload` is a flatbuffer whose root is a table of `tableDef` that
// the builders copy as it was written (see verifyTableBuffer). `payload` must
// already be aligned (see alignPayload).
static bool tablePayloadFits(const reflection::Schema* schema,
                             const reflection::Object* tableDef,
                             std::span<const uint8_t> payload) {
  return verifyTableBuffer(schema, tableDef, payload);
}

// Whether a whole-vector payload for a vector of tables of `tableDef` is a
// list of the flatbuffers that build its elements.
static bool tableListFits(const reflection::Schema* schema,
                          const reflection::Object* tableDef,
                          flexbuffers::Reference root) {
  if (root.GetType() != flexbuffers::FBT_VECTOR) {
    return false;
  }
  const auto elements = root.AsVector();
  auto storage = std::vector<PayloadWord>{};
  for (size_t i = 0; i < elements.size(); i++) {
    if (not elements[i].IsBlob()) {
      return false;
    }
    const auto blob = elements[i].AsBlob();
    if (not tablePayloadFits(
            schema, tableDef,
            alignPayload(std::span{blob.data(), blob.size()}, storage))) {
      return false;
    }
  }
  return true;
}

// The most bytes the vector of `type` that `root`, a whole-vector payload the
// classify pass accepted, builds, which a payload's size does not bound: each
// of its elements may be an offset to one shared string or table, which the
// builders copy once per element. A table is counted as its flatbuffer's
// bytes.
static uint64_t wholeVectorBytes(const reflection::Type* type,
                                 flexbuffers::Reference root) {
  const auto elements = root.AsVector();
  if (IsScalar(type->element())) {
    return uint64_t{elements.size()} * GetTypeSize(type->element());
  }
  auto bytes = uint64_t{0};
  for (size_t i = 0; i < elements.size(); i++) {
    const auto element = elements[i];
    const auto size = (type->element() == reflection::String)
                          ? element.AsString().size()
                          : element.AsBlob().size();
    bytes += uint64_t{size} + kMostBytesPerPaddedOffsetElement;
  }
  return bytes;
}

// `rest` is the path past `fieldDef`, a vector field of `tableDef`; `table` is
// null where the buffer stores no such vector.
static ApplyOutcome classifyPutInVector(const reflection::Schema* schema,
                                        const reflection::Object* tableDef,
                                        const reflection::Field* fieldDef,
                                        const Table* table,
                                        std::span<const uint8_t> payload,
                                        std::span<const uint32_t> rest,
                                        uint64_t growthBudget) {
  const auto* type = fieldDef->type();
  const auto elementType = type->element();
  const auto* elementDef = (elementType == reflection::Obj)
                               ? schema->objects()->Get(type->index())
                               : nullptr;
  if (type->base_type() == reflection::Vector64
      or (isWideOffsetField(fieldDef) and not rest.empty())) {
    // A vector held through a 64-bit offset with a 32-bit length is written
    // whole; its elements, and a vector with a 64-bit length, are read and
    // written through 32-bit accessors.
    return ApplyOutcome::Refused;
  }
  if (elementDef != nullptr and elementDef->is_struct()) {
    if (rest.empty()) {
      return appliedIf(flexPayloadFits(payload, [&](auto root) {
        return payloadFits(schema, type, false, root)
               and structVectorCanHold(elementDef, root.AsVector().size(),
                                       growthBudget);
      }));
    }
    if (not structVectorCanGrowTo(elementDef, rest.front(), growthBudget)) {
      return ApplyOutcome::Refused;
    }
    return appliedIf(flexPayloadFits(payload, [&](auto root) {
      auto image = std::vector<uint8_t>(elementDef->bytesize());
      return writeStructElementPayload(schema, elementDef, image,
                                       rest.subspan(1), root);
    }));
  }
  if (rest.empty()) {
    if (elementType == reflection::Union) {
      // The members' types live in a second vector no payload carries.
      return ApplyOutcome::Refused;
    }
    return appliedIf(flexPayloadFits(payload, [&](auto root) {
      const auto fits = (elementDef != nullptr)
                            ? tableListFits(schema, elementDef, root)
                            : payloadFits(schema, type, false, root);
      return fits and wholeVectorBytes(type, root) <= growthBudget;
    }));
  }
  const auto index = rest.front();
  const auto deeper = rest.subspan(1);
  if (not vectorCanGrowTo(elementType, index, growthBudget)) {
    return ApplyOutcome::Refused;
  }
  // What the path reaches inside the element is built beside the vector, so
  // it shares what the vector leaves of the budget.
  const auto deeperBudget = growthBudget - grownVectorBytes(elementType, index);
  const auto othersFaithful =
      otherElementsCopyFaithfully(schema, tableDef, fieldDef, table, index);
  if (elementDef != nullptr) {
    const auto* vector = getFieldVT(table, fieldDef);
    const auto* element = (vector != nullptr and index < vector->size())
                              ? vector->Get(index)
                              : nullptr;
    return refusedUnlessFaithful(
        classifyPut(schema, elementDef, element, payload, deeper, deeperBudget),
        othersFaithful);
  }
  if (elementType == reflection::Union) {
    if (deeper.empty()) {
      return ApplyOutcome::Refused;
    }
    const auto member = deeper.front();
    const auto* memberDef =
        getUnionTableDefFromEnumIndex(schema, fieldDef, member);
    if (memberDef == nullptr) {
      return ApplyOutcome::Refused;
    }
    const auto* vector = getFieldVT(table, fieldDef);
    const auto* types =
        getFieldVI<uint8_t>(table, getUnionTypeField(tableDef, fieldDef));
    const auto holdsMember = vector != nullptr and types != nullptr
                             and index < std::min(vector->size(), types->size())
                             and types->Get(index) == member;
    return refusedUnlessFaithful(
        classifyPut(schema, memberDef,
                    holdsMember ? vector->Get(index) : nullptr, payload,
                    deeper.subspan(1), deeperBudget),
        othersFaithful);
  }
  if (not deeper.empty()) {
    return ApplyOutcome::Refused;
  }
  return appliedIf(flexPayloadFits(payload, [&](auto root) {
    return payloadFits(schema, type, true, root);
  }));
}

// What a put of `payload` into `fieldDef`, a field of `tableDef`, or into
// what `rest` names inside it, comes to in `table`.
static ApplyOutcome classifyPutField(const reflection::Schema* schema,
                                     const reflection::Object* tableDef,
                                     const reflection::Field* fieldDef,
                                     const Table* table,
                                     std::span<const uint8_t> payload,
                                     std::span<const uint32_t> rest,
                                     uint64_t growthBudget) {
  const auto* type = fieldDef->type();
  switch (type->base_type()) {
    case reflection::Obj: {
      const auto* subDef = schema->objects()->Get(type->index());
      if (subDef->is_struct()) {
        return appliedIf(flexPayloadFits(payload, [&](auto root) {
          auto image = std::vector<uint8_t>(subDef->bytesize());
          return writeStructPayload(schema, subDef, image, rest, root);
        }));
      }
      return classifyPut(schema, subDef, storedSubtable(table, fieldDef),
                         payload, rest, growthBudget);
    }
    case reflection::Union: {
      if (rest.empty()) {
        return ApplyOutcome::Refused;
      }
      const auto member = rest.front();
      const auto* memberDef =
          getUnionTableDefFromEnumIndex(schema, fieldDef, member);
      if (memberDef == nullptr) {
        return ApplyOutcome::Refused;
      }
      const auto held =
          storesField(table, fieldDef)
          and storedUnionMember(tableDef, fieldDef, table) == member;
      return classifyPut(schema, memberDef,
                         held ? GetFieldT(*table, *fieldDef) : nullptr, payload,
                         rest.subspan(1), growthBudget);
    }
    case reflection::Vector:
    case reflection::Vector64:
      return classifyPutInVector(schema, tableDef, fieldDef,
                                 storesField(table, fieldDef) ? table : nullptr,
                                 payload, rest, growthBudget);
    case reflection::Array:
      return ApplyOutcome::Refused;
    default:
      return appliedIf(rest.empty()
                       and flexPayloadFits(payload, [&](auto root) {
                             return payloadFits(schema, type, false, root);
                           }));
  }
}

// What a put along `path` comes to in `table`, a table of `tableDef` or null
// where the buffer stores none. A put creates what it reaches through, so it
// is only ever Applied or Refused. A path that ends at a table carries a
// flatbuffer payload, verified as that table; every other payload is a
// flexbuffer, checked against the type it is written as. The fields of
// `table` the path does not name are copied, so a put is Refused where one of
// them does not copy faithfully. A vector it grows, or builds whole of
// structs, may take `growthBudget` bytes (see vectorGrowthBudget); the builders
// grow every vector a put classified Applied grows.
static ApplyOutcome classifyPut(const reflection::Schema* schema,
                                const reflection::Object* tableDef,
                                const Table* table,
                                std::span<const uint8_t> payload,
                                std::span<const uint32_t> path,
                                uint64_t growthBudget) {
  if (path.empty()) {
    return appliedIf(tablePayloadFits(schema, tableDef, payload));
  }
  const auto* fieldDef = fieldAtOffset(tableDef, path.front());
  if (fieldDef == nullptr or isDiscriminator(fieldDef)) {
    return ApplyOutcome::Refused;
  }
  return refusedUnlessFaithful(
      classifyPutField(schema, tableDef, fieldDef, table, payload,
                       path.subspan(1), growthBudget),
      copiesFaithfully(schema, tableDef, table, fieldDef));
}

// The offsets an operation carries, which `wellFormed` checks it does.
static std::span<const uint32_t> operationPath(
    const Vector<uint32_t>* offsets) {
  return std::span{offsets->data(), offsets->size()};
}

// The payload a put carries, which `wellFormed` checks it does.
static std::span<const uint8_t> operationPayload(const Put* put) {
  return std::span{put->payload()->data(), put->payload()->size()};
}

// ---------------------------------------------------------------------------
// Request shape
//
// What the schema alone decides about a request is checked once, before any
// operation is classified, so that the classify pass and the builders see
// only a request of the shape the parser writes: every operation present and
// of a type the union declares, with every member the parser writes, and
// every patch stopping at a table or struct. The paths of puts and deletes
// are resolved by the classify pass, against the buffer as well.
// ---------------------------------------------------------------------------

// The table or struct a patch along `path` from `objectDef` patches, or null
// where the path stops anywhere else -- at a scalar, a string, a whole
// vector, a union with no member named, or in an array -- or does not
// resolve. `objectDef` itself for an empty path.
static const reflection::Object* patchTarget(
    const reflection::Schema* schema, const reflection::Object* objectDef,
    std::span<const uint32_t> path) {
  const auto* current = objectDef;
  // The table member of the union `fieldDef` names that the next step names,
  // consuming the step; null where there is none.
  const auto takeMember =
      [&](const reflection::Field* fieldDef) -> const reflection::Object* {
    if (path.empty()) {
      return nullptr;
    }
    const auto member = path.front();
    path = path.subspan(1);
    return getUnionTableDefFromEnumIndex(schema, fieldDef, member);
  };
  while (not path.empty()) {
    const auto* fieldDef = fieldAtOffset(current, path.front());
    path = path.subspan(1);
    if (fieldDef == nullptr or isDiscriminator(fieldDef)) {
      return nullptr;
    }
    const auto* type = fieldDef->type();
    switch (type->base_type()) {
      case reflection::Obj:
        current = schema->objects()->Get(type->index());
        break;
      case reflection::Union:
        current = takeMember(fieldDef);
        break;
      case reflection::Vector:
        if (path.empty()) {
          return nullptr;
        }
        path = path.subspan(1);
        if (type->element() == reflection::Obj) {
          current = schema->objects()->Get(type->index());
        } else if (type->element() == reflection::Union) {
          current = takeMember(fieldDef);
        } else {
          return nullptr;
        }
        break;
      default:
        return nullptr;
    }
    if (current == nullptr) {
      return nullptr;
    }
  }
  return current;
}

// Whether `operation`, of type `type`, is of the shape the parser writes,
// its path relative to `objectDef`: a put carries a path and a payload, a
// delete a path, and a patch a path to a table or struct (see patchTarget)
// and one type for each of its operations, each of this shape relative to
// what the patch patches.
static bool wellFormed(const reflection::Schema* schema,
                       const reflection::Object* objectDef, Operation type,
                       const void* operation) {
  if (operation == nullptr) {
    return false;
  }
  switch (type) {
    case Operation::Put: {
      const auto* put = static_cast<const Put*>(operation);
      return put->offsets() != nullptr and put->payload() != nullptr;
    }
    case Operation::Delete:
      return static_cast<const Delete*>(operation)->offsets() != nullptr;
    case Operation::Patch: {
      const auto* patch = static_cast<const Patch*>(operation);
      const auto* offsets = patch->offsets();
      const auto* types = patch->updates_type();
      const auto* updates = patch->updates();
      if (offsets == nullptr or types == nullptr or updates == nullptr
          or types->size() != updates->size()) {
        return false;
      }
      const auto* target = patchTarget(
          schema, objectDef, std::span{offsets->data(), offsets->size()});
      if (target == nullptr) {
        return false;
      }
      for (uoffset_t i = 0; i < updates->size(); i++) {
        if (not wellFormed(schema, target, types->Get(i), updates->Get(i))) {
          return false;
        }
      }
      return true;
    }
    default:
      return false;
  }
}

// ---------------------------------------------------------------------------
// Patch
//
// A Patch navigates via `offsets` to a subobject and applies a list of nested
// operations (Put / Patch / Delete) to it. Each nested operation's offsets are
// relative to the patched subobject, so applying it is equivalent to applying
// the operation to the root with the patch's offsets prepended. Operations are
// applied in sequence, threading the result of each into the next, so a Patch
// can carry multiple independent updates.
// ---------------------------------------------------------------------------

static std::vector<uint32_t> concatPath(std::span<const uint32_t> prefix,
                                        const Vector<uint32_t>* offsets) {
  auto full = std::vector<uint32_t>(prefix.begin(), prefix.end());
  full.insert(full.end(), offsets->begin(), offsets->end());
  return full;
}

// Applies each operation in `updates` (prefixing its offsets with `prefix`) to
// the running table, leaving the accumulated flatbuffer in `result`. The first
// operation reads `sourceTable`; each subsequent one reads the previous
// result.
//
// Answers Refused as soon as one operation is Refused against the table it
// would edit. An operation that is NotStored builds nothing and leaves the
// running table as it was, and the rest are still checked, so that a later
// Refused one is not reported as NotStored; the list then answers NotStored.
// `result` is meaningful only for Applied, and is left empty where no
// operation built anything.
//
// Each put is classified against what `bytesHeld`, the bytes held beside the
// list -- by the builder the result is folded into, and by the running table
// of any patch the list is nested in -- and the running table the list has
// built leave of `largestGrownVectorBytes` (see vectorGrowthBudget). So a
// lone put in a patch grows a vector as far as it does alone, and the
// vectors several puts grow are bounded together.
static ApplyOutcome applyOperationList(
    const reflection::Schema* schema, const Table* sourceTable,
    std::span<const uint32_t> prefix,
    const Vector<serialized::Operation>* updateTypes,
    const Vector<flatbuffers::Offset<void>>* updates, bool useStringPooling,
    uint64_t bytesHeld, uint64_t largestGrownVectorBytes,
    flatbuffers::DetachedBuffer& result) {
  const auto* rootDef = schema->root_table();

  // An empty update list names no edit. It answers a standalone copy of the
  // source so every caller -- a nested empty patch included -- has a buffer
  // to thread onward.
  if (updates->size() == 0) {
    if (not copiesFaithfully(schema, rootDef, sourceTable)) {
      return ApplyOutcome::Refused;
    }
    FlatBufferBuilder builder;
    builder.Finish(Offset<Table>(copyTableDeep(builder, schema, rootDef,
                                               sourceTable, useStringPooling)));
    result = builder.Release();
    return ApplyOutcome::Applied;
  }
  auto outcome = ApplyOutcome::Applied;
  const Table* source = sourceTable;
  // Takes `built` as the running table.
  const auto advance = [&](flatbuffers::DetachedBuffer built) {
    result = std::move(built);
    source = flatbuffers::GetAnyRoot(result.data());
  };
  for (uoffset_t i = 0; i < updates->size(); i++) {
    const auto* operation = updates->Get(i);
    switch (updateTypes->Get(i)) {
      case Operation::Patch: {
        const auto* subpatch = static_cast<const Patch*>(operation);
        auto nested = flatbuffers::DetachedBuffer{};
        const auto nestedOutcome = applyOperationList(
            schema, source, concatPath(prefix, subpatch->offsets()),
            subpatch->updates_type(), subpatch->updates(), useStringPooling,
            bytesHeld + result.size(), largestGrownVectorBytes, nested);
        if (nestedOutcome == ApplyOutcome::Refused) {
          return ApplyOutcome::Refused;
        }
        if (nestedOutcome == ApplyOutcome::NotStored) {
          outcome = ApplyOutcome::NotStored;
        }
        if (nested.size() != 0) {
          advance(std::move(nested));
        }
        break;
      }
      case Operation::Put: {
        const auto* put = static_cast<const Put*>(operation);
        auto storage = std::vector<PayloadWord>{};
        const auto payload = alignPayload(operationPayload(put), storage);
        const auto path = concatPath(prefix, put->offsets());
        const auto budget = vectorGrowthBudget(bytesHeld + result.size(),
                                               largestGrownVectorBytes);
        if (classifyPut(schema, rootDef, source, payload, path, budget)
            != ApplyOutcome::Applied) {
          return ApplyOutcome::Refused;
        }
        FlatBufferBuilder builder;
        builder.Finish(
            Offset<Table>(applyRequestPut(builder, schema, rootDef, source,
                                          payload, path, useStringPooling)));
        advance(builder.Release());
        break;
      }
      case Operation::Delete: {
        const auto* del = static_cast<const Delete*>(operation);
        const auto path = concatPath(prefix, del->offsets());
        const auto deleteOutcome =
            classifyDelete(schema, rootDef, source, path);
        if (deleteOutcome == ApplyOutcome::Refused) {
          return ApplyOutcome::Refused;
        }
        if (deleteOutcome == ApplyOutcome::NotStored) {
          outcome = ApplyOutcome::NotStored;
          break;
        }
        FlatBufferBuilder builder;
        builder.Finish(Offset<Table>(applyRequestDelete(
            builder, schema, rootDef, source, path, useStringPooling)));
        advance(builder.Release());
        break;
      }
      default:
        return ApplyOutcome::Refused;
    }
  }
  return outcome;
}

static fbrequest::ApplyResult applyRequestPatch(
    FlatBufferBuilder& fbb, const reflection::Schema* schema,
    const Table* sourceTable, const Patch* patch, bool useStringPooling,
    uint64_t largestGrownVectorBytes) {
  const auto* rootDef = schema->root_table();
  const auto* updates = patch->updates();
  if (updates->size() == 0) {
    // A patch with no updates leaves the source unchanged.
    if (not copiesFaithfully(schema, rootDef, sourceTable)) {
      return {0, ApplyOutcome::Refused};
    }
    return {copyTableDeep(fbb, schema, rootDef, sourceTable, useStringPooling),
            ApplyOutcome::Applied};
  }

  auto result = flatbuffers::DetachedBuffer{};
  const auto outcome = applyOperationList(
      schema, sourceTable,
      concatPath(std::span<const uint32_t>{}, patch->offsets()),
      patch->updates_type(), updates, useStringPooling, fbb.GetSize(),
      largestGrownVectorBytes, result);
  if (outcome != ApplyOutcome::Applied) {
    return {0, outcome};
  }

  // Fold the accumulated result into the caller's builder.
  return {
      copyTableDeep(fbb, schema, rootDef,
                    flatbuffers::GetAnyRoot(result.data()), useStringPooling),
      ApplyOutcome::Applied};
}

// ---------------------------------------------------------------------------
// Delete
//
// Rebuilds a table from `sourceTable`, removing the field or vector element
// identified by `path`. Unaffected fields are copied by the same copy the Put
// path uses; the affected field is either omitted (terminal
// delete), rebuilt without one element (vector element delete), or rebuilt by
// recursing (delete inside a subobject / vector element).
// ---------------------------------------------------------------------------

// Whether a delete along `path`, which reaches the union `fieldDef` names,
// removes it whole: the path stops at the union, or at the member it holds.
// A path naming another member names nothing the buffer stores.
static bool deletesWholeUnion(const reflection::Object* tableDef,
                              const reflection::Field* fieldDef,
                              const Table* sourceTable,
                              std::span<const uint32_t> path) {
  return path.size() == 1
         or (path.size() == 2
             and storedUnionMember(tableDef, fieldDef, sourceTable) == path[1]);
}

// Whether a delete removes element `index` of a vector of unions whole, given
// `pathPastIndex`: it names no member, or names the member the element holds
// and nothing inside it.
static bool deletesWholeUnionElement(const Vector<Offset<Table>>* vector,
                                     const Vector<uint8_t>* typeVector,
                                     size_t index,
                                     std::span<const uint32_t> pathPastIndex) {
  if (vector == nullptr or typeVector == nullptr
      or index >= std::min(vector->size(), typeVector->size())
      or pathPastIndex.size() > 1) {
    return false;
  }
  return pathPastIndex.empty()
         or typeVector->Get(index) == pathPastIndex.front();
}

// Rebuild an inline-element vector (scalars, UType, or fixed-size structs)
// without the element at `index`. `elementSize`/`alignment` are the element's
// byte size and alignment. A null vector or out-of-range index yields the
// (unchanged) copy.
static uoffset_t deleteInlineVectorElement(FlatBufferBuilder& fbb,
                                           const VectorOfAny* vector,
                                           size_t index, size_t elementSize,
                                           size_t alignment) {
  const auto existingSize = (vector != nullptr) ? vector->size() : uoffset_t{0};
  const auto* data = (vector != nullptr) ? vector->Data() : nullptr;
  if (index >= existingSize) {
    // Nothing to remove; copy the vector unchanged.
    fbb.StartVector(existingSize, elementSize, alignment);
    fbb.PushBytes(data, elementSize * existingSize);
    return fbb.EndVector(existingSize);
  }
  const auto newSize = existingSize - 1;
  fbb.StartVector(newSize, elementSize, alignment);
  // Elements are pushed back-to-front: trailing elements first, then leading.
  const auto trailing = existingSize - index - 1;
  if (trailing != 0U) {
    fbb.PushBytes(data + (elementSize * (index + 1)), elementSize * trailing);
  }
  fbb.PushBytes(data, elementSize * index);
  return fbb.EndVector(newSize);
}

static uoffset_t deleteStringVectorElement(FlatBufferBuilder& fbb,
                                           const reflection::Field* fieldDef,
                                           const Table* sourceTable,
                                           size_t index,
                                           bool useStringPooling) {
  const auto* vector = getFieldVS(sourceTable, fieldDef);
  const auto existingSize = (vector != nullptr) ? vector->size() : uoffset_t{0};
  auto elements = std::vector<Offset<const String*>>{};
  for (uoffset_t i = 0; i < existingSize; i++) {
    if (i == index) {
      continue;
    }
    elements.push_back(createString(fbb, vector->Get(i), useStringPooling));
  }
  return fbb.CreateVector(elements).o;
}

static uoffset_t deleteObjectVectorElement(
    FlatBufferBuilder& fbb, const reflection::Schema* schema,
    const reflection::Field* fieldDef, const Table* sourceTable, size_t index,
    std::span<const uint32_t> deeperPath, bool useStringPooling) {
  const auto* subDef = schema->objects()->Get(fieldDef->type()->index());
  const auto* vector = getFieldVT(sourceTable, fieldDef);
  const auto existingSize = (vector != nullptr) ? vector->size() : uoffset_t{0};
  auto elements = std::vector<Offset<Table>>{};
  for (uoffset_t i = 0; i < existingSize; i++) {
    if (i == index) {
      if (deeperPath.empty()) {
        continue;  // remove this element
      }
      elements.push_back(Offset<Table>(applyRequestDelete(
          fbb, schema, subDef, vector->Get(i), deeperPath, useStringPooling)));
    } else {
      elements.push_back(Offset<Table>(copyTableDeep(
          fbb, schema, subDef, vector->Get(i), useStringPooling)));
    }
  }
  return fbb.CreateVector(elements.data(), elements.size()).o;
}

static uoffset_t deleteUnionVectorElement(
    FlatBufferBuilder& fbb, const reflection::Schema* schema,
    const reflection::Object* tableDef, const reflection::Field* fieldDef,
    const Table* sourceTable, size_t index,
    std::span<const uint32_t> deeperPath, bool useStringPooling) {
  const auto* vector = getFieldVT(sourceTable, fieldDef);
  const auto* unionTypeFieldDef = getUnionTypeField(tableDef, fieldDef);
  const auto* typeVector = getFieldVI<uint8_t>(sourceTable, unionTypeFieldDef);
  const auto existingSize = (vector != nullptr and typeVector != nullptr)
                                ? std::min(vector->size(), typeVector->size())
                                : uoffset_t{0};
  // The path selects the element's union member (e.g. `.Integer`); that enum
  // step is only navigation, so strip it before recursing.
  const auto memberPath =
      deeperPath.empty() ? deeperPath : deeperPath.subspan(1);
  const auto removesElement =
      deletesWholeUnionElement(vector, typeVector, index, deeperPath);
  auto elements = std::vector<Offset<Table>>{};
  for (uoffset_t i = 0; i < existingSize; i++) {
    if (i == index and removesElement) {
      continue;
    }
    // Only a table member has a field to delete, and only the member the path
    // names; anything else is copied as it is.
    const auto* unionDef = getUnionTableDefFromEnumIndex(
        schema, unionTypeFieldDef, typeVector->Get(i));
    if (unionDef != nullptr and i == index and not memberPath.empty()
        and typeVector->Get(i) == deeperPath.front()) {
      elements.push_back(Offset<Table>(
          applyRequestDelete(fbb, schema, unionDef, vector->Get(i), memberPath,
                             useStringPooling)));
    } else {
      elements.push_back(Offset<Table>(
          copyUnionMember(fbb, schema, unionTypeFieldDef, typeVector->Get(i),
                          vector->Get(i), useStringPooling)));
    }
  }
  return fbb.CreateVector(elements.data(), elements.size()).o;
}

static uoffset_t applyRequestDelete_GenerateFieldOffset(
    FlatBufferBuilder& fbb, const reflection::Schema* schema,
    const reflection::Object* tableDef, const reflection::Field* fieldDef,
    const Table* sourceTable, std::span<const uint32_t> path,
    bool useStringPooling) {
  const auto baseType = fieldDef->type()->base_type();

  // Union type scalars/vectors are emitted alongside their union field.
  if (baseType == reflection::UType
      or (baseType == reflection::Vector
          and fieldDef->type()->element() == reflection::UType)) {
    return 0;
  }

  const bool inPath = not path.empty() and path.front() == fieldDef->offset();
  const bool inSource =
      (sourceTable != nullptr) and sourceTable->CheckField(fieldDef->offset());

  if (inPath) {
    if (not inSource) {
      // Nothing to remove, and nothing to build: a delete reaching into a
      // field the buffer does not store leaves it absent.
      return 0;
    }
    switch (baseType) {
      case reflection::Obj: {
        const auto* subDef = schema->objects()->Get(fieldDef->type()->index());
        if (subDef->is_struct() or path.size() == 1) {
          return 0;  // delete the whole (struct or table) field
        }
        return applyRequestDelete(fbb, schema, subDef,
                                  GetFieldT(*sourceTable, *fieldDef),
                                  updatePath(path), useStringPooling);
      }
      case reflection::Union: {
        if (deletesWholeUnion(tableDef, fieldDef, sourceTable, path)) {
          return 0;  // its type is dropped in build
        }
        const auto* unionDef =
            getUnionTableDefFromEnumIndex(schema, fieldDef, path[1]);
        if (unionDef == nullptr
            or storedUnionMember(tableDef, fieldDef, sourceTable) != path[1]) {
          // A member the union does not hold, or one that holds no table, has
          // nothing to delete, whole or in part. Reading the stored member as
          // another would read its fields at the other's offsets.
          return copyStoredField(fbb, schema, tableDef, fieldDef, sourceTable,
                                 useStringPooling);
        }
        return applyRequestDelete(fbb, schema, unionDef,
                                  GetFieldT(*sourceTable, *fieldDef),
                                  path.subspan(2), useStringPooling);
      }
      case reflection::Vector: {
        if (path.size() == 1) {
          return 0;  // delete the whole vector
        }
        const auto index = size_t{path[1]};
        const auto elementType = fieldDef->type()->element();
        const auto deeperPath = path.subspan(2);
        switch (elementType) {
          case reflection::String:
            return deleteStringVectorElement(fbb, fieldDef, sourceTable, index,
                                             useStringPooling);
          case reflection::Obj: {
            const auto* subDef =
                schema->objects()->Get(fieldDef->type()->index());
            if (subDef->is_struct()) {
              // A struct vector stores fixed-size inline elements. A path
              // into one names a member, which a struct cannot lose, so the
              // vector is kept whole.
              const auto* vector = sourceTable->GetPointer<const VectorOfAny*>(
                  fieldDef->offset());
              if (not deeperPath.empty()) {
                return copyStoredField(fbb, schema, tableDef, fieldDef,
                                       sourceTable, useStringPooling);
              }
              return deleteInlineVectorElement(
                  fbb, vector, index, subDef->bytesize(),
                  vectorAlignment(schema, fieldDef));
            }
            return deleteObjectVectorElement(fbb, schema, fieldDef, sourceTable,
                                             index, deeperPath,
                                             useStringPooling);
          }
          case reflection::Union:
            return deleteUnionVectorElement(fbb, schema, tableDef, fieldDef,
                                            sourceTable, index, deeperPath,
                                            useStringPooling);
          default: {
            const auto* vector =
                sourceTable->GetPointer<const VectorOfAny*>(fieldDef->offset());
            return deleteInlineVectorElement(fbb, vector, index,
                                             GetTypeSize(elementType),
                                             vectorAlignment(schema, fieldDef));
          }
        }
      }
      default:
        return 0;  // scalar or string: delete the whole field
    }
  }

  if (inSource) {
    // Copy the field unchanged. Scalars/structs are inlined in the build phase.
    return copyStoredField(fbb, schema, tableDef, fieldDef, sourceTable,
                           useStringPooling);
  }
  return 0;
}

static std::vector<uoffset_t> applyRequestDelete_GenerateOffsets(
    FlatBufferBuilder& fbb, const reflection::Schema* schema,
    const reflection::Object* tableDef, const Table* sourceTable,
    std::span<const uint32_t> path, bool useStringPooling) {
  auto offsets = std::vector<uoffset_t>{};
  for (const auto* fieldDef : *tableDef->fields()) {
    auto offset = applyRequestDelete_GenerateFieldOffset(
        fbb, schema, tableDef, fieldDef, sourceTable, path, useStringPooling);

    // A surviving union vector needs a matching type vector rebuilt too.
    if (offset != 0 and fieldDef->type()->base_type() == reflection::Vector
        and fieldDef->type()->element() == reflection::Union) {
      const auto* unionTypeFieldDef = getUnionTypeField(tableDef, fieldDef);
      const bool inPath =
          not path.empty() and path.front() == fieldDef->offset();
      uoffset_t typeOffset = 0;
      // A union vector element path is [field, index, enumValue, ...]. A whole-
      // element delete drops that index from the parallel type vector too; a
      // copy or a recurse into the element leaves the types unchanged.
      if (inPath and path.size() >= 2
          and deletesWholeUnionElement(
              getFieldVT(sourceTable, fieldDef),
              getFieldVI<uint8_t>(sourceTable, unionTypeFieldDef), path[1],
              path.subspan(2))) {
        const auto* typeVector = sourceTable->GetPointer<const VectorOfAny*>(
            unionTypeFieldDef->offset());
        typeOffset = deleteInlineVectorElement(fbb, typeVector, path[1],
                                               GetTypeSize(reflection::UType),
                                               GetTypeSize(reflection::UType));
      } else {
        typeOffset = copyStoredField(fbb, schema, tableDef, unionTypeFieldDef,
                                     sourceTable, useStringPooling);
      }
      offsets.push_back(typeOffset);
    }

    if (offset != 0U) {
      offsets.push_back(offset);
    }
  }
  return offsets;
}

static void applyRequestDelete_BuildTableField(
    FlatBufferBuilder& fbb, const reflection::Schema* schema,
    const reflection::Object* tableDef, const reflection::Field* fieldDef,
    const Table* sourceTable, std::span<const uint32_t> path,
    const std::vector<uoffset_t>& offsets, size_t& offsetIndex) {
  const auto baseType = fieldDef->type()->base_type();
  const bool inPath = not path.empty() and path.front() == fieldDef->offset();
  const bool inSource =
      (sourceTable != nullptr) and sourceTable->CheckField(fieldDef->offset());

  // Whether this whole field is being removed (as opposed to copied or rebuilt
  // with an element removed / a subobject recursed into).
  bool deletedWhole = false;
  if (inPath) {
    switch (baseType) {
      case reflection::Obj:
        // A struct can't have individual fields removed, so a path INTO a
        // struct copies it unchanged; only a size-1 path removes the field.
        deletedWhole = path.size() == 1;
        break;
      case reflection::Union:
        deletedWhole = deletesWholeUnion(tableDef, fieldDef, sourceTable, path);
        break;
      case reflection::Vector:
        deletedWhole = path.size() == 1;
        break;
      default:  // scalar or string
        deletedWhole = true;
        break;
    }
  }
  // A field the buffer does not store stays absent whatever the path asks of
  // it, which keeps this in step with the generate pass, and keeps a struct
  // from being copied out of bytes that are not there.
  const bool present = inSource and not deletedWhole;

  switch (baseType) {
    case reflection::UType:
      break;  // handled with the union field
    case reflection::String:
    case reflection::Vector:
    case reflection::Vector64: {
      if (present) {
        if (fieldDef->type()->element() == reflection::UType) {
          break;  // handled with the union field
        }
        if (fieldDef->type()->element() == reflection::Union) {
          const auto* unionTypeFieldDef = getUnionTypeField(tableDef, fieldDef);
          fbb.AddOffset(unionTypeFieldDef->offset(),
                        Offset<void>(offsets[offsetIndex++]));
        }
        addFieldOffset(fbb, fieldDef, offsets[offsetIndex++]);
      }
      break;
    }
    case reflection::Obj: {
      if (present) {
        const auto* subDef = schema->objects()->Get(fieldDef->type()->index());
        if (subDef->is_struct()) {
          copyInline(fbb, fieldDef, sourceTable, subDef->minalign(),
                     subDef->bytesize());
        } else {
          fbb.AddOffset(fieldDef->offset(),
                        Offset<void>(offsets[offsetIndex++]));
        }
      }
      break;
    }
    case reflection::Union: {
      if (present) {
        // The union survives unchanged, so its type enum comes from the source.
        const auto* unionEnumFieldDef = getUnionTypeField(tableDef, fieldDef);
        auto enumValue = GetFieldI<uint8_t>(*sourceTable, *unionEnumFieldDef);
        auto size = GetTypeSize(unionEnumFieldDef->type()->base_type());
        fbb.Align(size);
        fbb.PushBytes(reinterpret_cast<uint8_t*>(&enumValue), size);
        fbb.TrackField(unionEnumFieldDef->offset(), fbb.GetSize());
        fbb.AddOffset(fieldDef->offset(), Offset<void>(offsets[offsetIndex++]));
      }
      break;
    }
    default: {  // scalars: inlined, present unless deleted
      if (inSource and not inPath) {
        auto size = GetTypeSize(baseType);
        copyInline(fbb, fieldDef, sourceTable, size, size);
      }
      break;
    }
  }
}

static uoffset_t applyRequestDelete(FlatBufferBuilder& fbb,
                                    const reflection::Schema* schema,
                                    const reflection::Object* tableDef,
                                    const Table* sourceTable,
                                    std::span<const uint32_t> path,
                                    bool useStringPooling) {
  auto offsets = applyRequestDelete_GenerateOffsets(
      fbb, schema, tableDef, sourceTable, path, useStringPooling);
  auto start = fbb.StartTable();
  auto offsetIndex = size_t{0};
  for (const auto* fieldDef : *tableDef->fields()) {
    applyRequestDelete_BuildTableField(fbb, schema, tableDef, fieldDef,
                                       sourceTable, path, offsets, offsetIndex);
  }
  FLATBUFFERS_ASSERT(offsetIndex == offsets.size());
  return fbb.EndTable(start);
}

fbrequest::ApplyResult fbrequest::tryApplyRequestGrowingWithin(
    FlatBufferBuilder& fbb, const reflection::Schema* schema,
    const Table* sourceTable, const Request* request, bool useStringPooling,
    uint64_t largestGrownVectorBytes) {
  const auto* rootTableDef = schema->root_table();
  if (not wellFormed(schema, rootTableDef, request->operation_type(),
                     request->operation())) {
    return {0, ApplyOutcome::Refused};
  }
  // Past the real bound a grown vector could overrun what a flatbuffer holds.
  largestGrownVectorBytes =
      std::min(largestGrownVectorBytes, kLargestGrownVectorBytes);
  const auto budget =
      vectorGrowthBudget(fbb.GetSize(), largestGrownVectorBytes);
  switch (request->operation_type()) {
    case Operation::Put: {
      const auto* put = request->operation_as_Put();
      auto storage = std::vector<PayloadWord>{};
      const auto payload = alignPayload(operationPayload(put), storage);
      const auto path = operationPath(put->offsets());
      const auto outcome =
          classifyPut(schema, rootTableDef, sourceTable, payload, path, budget);
      if (outcome != ApplyOutcome::Applied) {
        return {0, outcome};
      }
      return {applyRequestPut(fbb, schema, rootTableDef, sourceTable, payload,
                              path, useStringPooling),
              ApplyOutcome::Applied};
    }
    case Operation::Patch:
      return applyRequestPatch(fbb, schema, sourceTable,
                               request->operation_as_Patch(), useStringPooling,
                               largestGrownVectorBytes);
    case Operation::Delete: {
      const auto path =
          operationPath(request->operation_as_Delete()->offsets());
      const auto outcome =
          classifyDelete(schema, rootTableDef, sourceTable, path);
      if (outcome != ApplyOutcome::Applied) {
        return {0, outcome};
      }
      return {applyRequestDelete(fbb, schema, rootTableDef, sourceTable, path,
                                 useStringPooling),
              ApplyOutcome::Applied};
    }
    default:
      return {0, ApplyOutcome::Refused};
  }
}

fbrequest::ApplyResult fbrequest::tryApplyRequest(
    FlatBufferBuilder& fbb, const reflection::Schema* schema,
    const Table* sourceTable, const Request* request, bool useStringPooling) {
  return tryApplyRequestGrowingWithin(fbb, schema, sourceTable, request,
                                      useStringPooling,
                                      kLargestGrownVectorBytes);
}

bool fbrequest::verifyBuffer(const reflection::Schema* schema,
                             std::span<const uint8_t> buffer) {
  return verifyTableStructure(schema, schema->root_table(), buffer);
}

flatbuffers::Offset<const flatbuffers::Table*> fbrequest::applyRequest(
    FlatBufferBuilder& fbb, const reflection::Schema* schema,
    const Table* sourceTable, const Request* request, bool useStringPooling) {
  return tryApplyRequest(fbb, schema, sourceTable, request, useStringPooling)
      .root;
}
