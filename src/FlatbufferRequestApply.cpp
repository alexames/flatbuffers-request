#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

#include "ElementIndex.hpp"
#include "fbrequest/FlatbufferRequest.hpp"
#include "flatbuffers/flatbuffers.h"
#include "flatbuffers/idl.h"
#include "flatbuffers/reflection.h"

// TODO: Hook up Patch and Delete
// TODO: Add Struct and Array support

using flatbuffers::CopyTable;
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
    const uint8_t* ptr, bool /*useStringPooling*/) {
  auto table = CopyTable(fbb, *schema, *schema->root_table(),
                         *flatbuffers::GetAnyRoot(ptr), true);
  fbb.Finish(table);
  return table;
}

static uoffset_t CopyTable_GenerateObjectOffset(
    FlatBufferBuilder& fbb, const reflection::Schema* schema,
    const reflection::Object* tableDef, const reflection::Field* fieldDef,
    const Table* sourceTable, bool useStringPooling);

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
// discriminant `member`. A table is copied as one and a struct, which a union
// stores out of line, as its bytes; a member holding neither, NONE or an
// undeclared discriminant, is kept as an empty table under the same
// discriminant, since the verifier accepts both.
static uoffset_t copyUnionMember(FlatBufferBuilder& fbb,
                                 const reflection::Schema* schema,
                                 const reflection::Field* fieldDef,
                                 uint32_t member, const Table* stored,
                                 bool useStringPooling) {
  const auto* memberDef = getUnionMemberDef(schema, fieldDef, member);
  if (memberDef == nullptr) {
    return createEmptyTable(fbb);
  }
  if (memberDef->is_struct()) {
    fbb.Align(memberDef->minalign());
    fbb.PushBytes(reinterpret_cast<const uint8_t*>(stored),
                  memberDef->bytesize());
    return fbb.GetSize();
  }
  return CopyTable(fbb, *schema, *memberDef, *stored, useStringPooling).o;
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

static void getFlexbufferScalarDataPointer(flexbuffers::Reference reference,
                                           reflection::BaseType type,
                                           std::array<uint8_t, 8>* bytes) {
  switch (type) {
    case reflection::Bool: {
      auto val = reference.AsBool();
      std::memcpy(bytes->data(), &val, sizeof(val));
      return;
    }
    case reflection::Byte: {
      auto val = reference.AsInt8();
      std::memcpy(bytes->data(), &val, sizeof(val));
      return;
    }
    case reflection::UType:
    case reflection::UByte: {
      auto val = reference.AsUInt8();
      std::memcpy(bytes->data(), &val, sizeof(val));
      return;
    }
    case reflection::Short: {
      auto val = reference.AsInt16();
      std::memcpy(bytes->data(), &val, sizeof(val));
      return;
    }
    case reflection::UShort: {
      auto val = reference.AsUInt16();
      std::memcpy(bytes->data(), &val, sizeof(val));
      return;
    }
    case reflection::Int: {
      auto val = reference.AsInt32();
      std::memcpy(bytes->data(), &val, sizeof(val));
      return;
    }
    case reflection::UInt: {
      auto val = reference.AsUInt32();
      std::memcpy(bytes->data(), &val, sizeof(val));
      return;
    }
    case reflection::Long: {
      auto val = reference.AsInt64();
      std::memcpy(bytes->data(), &val, sizeof(val));
      return;
    }
    case reflection::ULong: {
      auto val = reference.AsUInt64();
      std::memcpy(bytes->data(), &val, sizeof(val));
      return;
    }
    case reflection::Float: {
      auto val = reference.AsFloat();
      std::memcpy(bytes->data(), &val, sizeof(val));
      return;
    }
    case reflection::Double: {
      auto val = reference.AsDouble();
      std::memcpy(bytes->data(), &val, sizeof(val));
      return;
    }
    default: {
      assert(false);
    }
  }
}

// TODO: Clean this up. Does it need to be one giant function?
// This should probably be refactored.
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

// The member of `structDef` stored at `offset` bytes into it.
static const reflection::Field* structFieldAtOffset(
    const reflection::Object* structDef, uint32_t offset) {
  for (const auto* fieldDef : *structDef->fields()) {
    if (fieldDef->offset() == offset) {
      return fieldDef;
    }
  }
  return nullptr;
}

// Whether a flexbuffer value is the kind a scalar member can take. Anything
// else -- a string, a map, a vector -- is converted rather than refused by
// `writeScalarInto`, which answers a wrong number instead of a wrong request.
static bool holdsAScalar(flexbuffers::Reference value) {
  return not value.IsString() and not value.IsMap() and not value.IsVector()
         and not value.IsTypedVector() and not value.IsFixedTypedVector()
         and not value.IsBlob();
}

static void writeScalarInto(flexbuffers::Reference value,
                            reflection::BaseType type, std::span<uint8_t> at) {
  auto bytes = std::array<uint8_t, 8>{};
  getFlexbufferScalarDataPointer(value, type, &bytes);
  const auto size = GetTypeSize(type);
  if (at.size() < size) {
    return;
  }
  std::memcpy(at.data(), bytes.data(), size);
}

// What writing a payload into a struct image came to. "Nothing" is not a
// failure: a map naming no member the struct declares asked for nothing, and
// the members BESIDE it are still written. It is only where nothing at all
// was written that the caller leaves the field alone, so a payload that wrote
// nothing does not create a struct of zeroes.
enum class StructWrite { Rejected, Nothing, Wrote };

static StructWrite writeStructMember(const reflection::Schema* schema,
                                     const reflection::Object* structDef,
                                     std::span<uint8_t> image,
                                     std::span<const uint32_t> path,
                                     flexbuffers::Reference value);

// Writes every member `map` names into `image`, leaving the rest as they were.
// A name the struct does not declare is ignored rather than refusing the whole
// map: the payload is parsed as a flexbuffer, which cannot be checked against
// the schema until here, and a struct has no way to carry an unknown member.
//
// Answers `Nothing` where it wrote nothing, so that a map naming no member
// the struct declares -- an empty one included -- does not count as a write.
// The caller leaves the field as it was, which is what keeps such a payload
// from creating a struct of zeroes where the buffer had none. A member whose
// own payload writes nothing is skipped; only a `Rejected` one, whose payload
// the struct cannot take at all, refuses the map around it.
static StructWrite writeStructMap(const reflection::Schema* schema,
                                  const reflection::Object* structDef,
                                  std::span<uint8_t> image,
                                  flexbuffers::Map map) {
  auto keys = map.Keys();
  auto values = map.Values();
  auto wroteAMember = false;
  for (size_t i = 0; i < keys.size(); i++) {
    const auto* fieldDef = structDef->fields()->LookupByKey(keys[i].AsKey());
    if (fieldDef == nullptr) {
      continue;
    }
    const auto memberOffset = uint32_t{fieldDef->offset()};
    const auto written = writeStructMember(
        schema, structDef, image, std::span{&memberOffset, 1}, values[i]);
    if (written == StructWrite::Rejected) {
      return StructWrite::Rejected;
    }
    wroteAMember = wroteAMember or written == StructWrite::Wrote;
  }
  return wroteAMember ? StructWrite::Wrote : StructWrite::Nothing;
}

// Writes `value` into the member `path` names, `path` being byte offsets into
// successively nested structs and, for a fixed-length array, the element
// index. An empty `path` means `value` is a map of the whole struct.
static StructWrite writeStructMember(const reflection::Schema* schema,
                                     const reflection::Object* structDef,
                                     std::span<uint8_t> image,
                                     std::span<const uint32_t> path,
                                     flexbuffers::Reference value) {
  if (path.empty()) {
    if (not value.IsMap()) {
      return StructWrite::Rejected;
    }
    return writeStructMap(schema, structDef, image, value.AsMap());
  }
  const auto* fieldDef = structFieldAtOffset(structDef, path.front());
  if (fieldDef == nullptr or fieldDef->offset() >= image.size()) {
    return StructWrite::Rejected;
  }
  auto at = image.subspan(fieldDef->offset());
  const auto baseType = fieldDef->type()->base_type();
  const auto rest = path.subspan(1);

  if (baseType == reflection::Obj) {
    const auto* nested = schema->objects()->Get(fieldDef->type()->index());
    return writeStructMember(schema, nested, at.first(nested->bytesize()), rest,
                             value);
  }
  if (baseType == reflection::Array) {
    const auto elementType = fieldDef->type()->element();
    if (not IsScalar(elementType)) {
      // An array of structs: `GetTypeSize` answers a pointer's width for one,
      // not the struct's, so every element would be written at the wrong
      // stride. Nothing writes one, so nothing is written.
      return StructWrite::Rejected;
    }
    const auto elementSize = GetTypeSize(elementType);
    const auto length = size_t{fieldDef->type()->fixed_length()};
    if (rest.empty()) {
      // The whole array, as a flexbuffer vector. A shorter one leaves the
      // elements it does not reach; a longer one is refused, because a
      // fixed-length array cannot grow.
      if (not value.IsVector()) {
        return StructWrite::Rejected;
      }
      auto elements = value.AsVector();
      if (elements.size() > length) {
        return StructWrite::Rejected;
      }
      if (elements.size() == 0) {
        // An empty vector reaches no element, so it writes nothing -- the
        // same as a map naming no member, and not a reason to create a
        // struct of zeroes.
        return StructWrite::Nothing;
      }
      for (size_t i = 0; i < elements.size(); i++) {
        writeScalarInto(elements[static_cast<int>(i)], elementType,
                        at.subspan(i * elementSize));
      }
      return StructWrite::Wrote;
    }
    const auto index = size_t{rest.front()};
    if (index >= length or rest.size() != 1) {
      return StructWrite::Rejected;
    }
    writeScalarInto(value, elementType, at.subspan(index * elementSize));
    return StructWrite::Wrote;
  }
  if (not rest.empty() or not IsScalar(baseType)) {
    return StructWrite::Rejected;
  }
  if (not holdsAScalar(value)) {
    // `writeScalarInto` converts whatever it is given: a string reads as 0 (or
    // as the number it spells), and a vector reads as its LENGTH. A member
    // named in a map has to be checked here, because the request parser sees
    // only the map and cannot tell which key goes to which type.
    return StructWrite::Rejected;
  }
  writeScalarInto(value, baseType, at);
  return StructWrite::Wrote;
}

// The image of the struct `fieldDef` names after `payload` is written into it
// at `path`: the source's bytes where the buffer stores one, zeroes where it
// does not. EMPTY where the payload wrote nothing, whether because the struct
// cannot take it -- a fixed-length array too long for its field, say -- or
// because it named nothing, so that a request which writes nothing does not
// leave a struct of zeroes where there was none.
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
  if (writeStructMember(schema, structDef, image, path, value)
      != StructWrite::Wrote) {
    return std::nullopt;
  }
  return image;
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
        fbb.StartVector(vector.size(), elementSize, elementSize);
        for (auto i = vector.size(); i > 0;) {
          fbb.Align(elementSize);
          std::array<uint8_t, 8> bytes{};
          getFlexbufferScalarDataPointer(vector[--i], elementType, &bytes);
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
          tables[i] = CopyTable(fbb, *schema, *tableDef,
                                payloadTableRoot(element), useStringPooling)
                          .o;
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
      return CopyTable(fbb, *schema, *subTableDef, payloadTableRoot(payload),
                       useStringPooling)
          .o;
    }
    case reflection::Union: {
      assert(path.size() == 2);
      auto unionEnumIndex = path[1];
      const auto* unionTableDef =
          getUnionTableDefFromEnumIndex(schema, fieldDef, unionEnumIndex);
      if (unionTableDef == nullptr) {
        return 0;
      }
      return CopyTable(fbb, *schema, *unionTableDef, payloadTableRoot(payload),
                       useStringPooling)
          .o;
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
  auto bytes = std::array<uint8_t, 8>{};
  getFlexbufferScalarDataPointer(root, baseType, &bytes);

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
    reflection::BaseType elementBaseType,
    std::array<uint8_t, 8> defaultValue = std::array<uint8_t, 8>{}) {
  const auto index = size_t{path.front()};
  if (not vectorCanGrowTo(elementBaseType, index, fbb.GetSize())) {
    return 0;
  }
  const auto elementSize = GetTypeSize(elementBaseType);
  auto existingVectorSize = (vector != nullptr) ? vector->size() : size_t{0};
  auto initialValues = std::min(existingVectorSize, index);
  auto newVectorSize = std::max(existingVectorSize, index + 1U);
  auto finalElements = newVectorSize - index - 1;
  auto zeroes = defaultValue;
  const auto* data = (vector != nullptr) ? vector->Data() : nullptr;

  fbb.StartVector(newVectorSize, elementSize, elementSize);
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
  auto bytes = std::array<uint8_t, 8>{};
  getFlexbufferScalarDataPointer(root, elementBaseType, &bytes);
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
  if (not vectorCanGrowTo(reflection::String, index, fbb.GetSize())) {
    return 0;
  }
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
    return 0;
  }
  const auto* vector = getFieldVT(sourceTable, fieldDef);
  const auto existingSize = (vector != nullptr) ? vector->size() : uoffset_t{0};
  auto index = path.front();
  if (not vectorCanGrowTo(reflection::Obj, index, fbb.GetSize())) {
    return 0;
  }
  auto elements = std::vector<Offset<Table>>{};
  elements.resize(std::max(existingSize, index + 1U));
  for (uoffset_t i = 0; i < existingSize; i++) {
    if (i == index) {
      elements[i] =
          applyRequestPut(fbb, schema, subtableDef, vector->Get(index), payload,
                          updatePath(path), useStringPooling);
    } else {
      elements[i] = CopyTable(fbb, *schema, *subtableDef, *vector->Get(i),
                              useStringPooling)
                        .o;
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
      fbb, vector, flexbufferValue, path, reflection::UType, padding);
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
  if (requestedDef == nullptr
      or not vectorCanGrowTo(reflection::Union, index, fbb.GetSize())) {
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
          elementBaseType);
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

using CopyTableFunc = uoffset_t (*)(FlatBufferBuilder& fbb,
                                    const reflection::Schema* schema,
                                    const reflection::Object* tableDef,
                                    const reflection::Field* fieldDef,
                                    const Table* sourceTable,
                                    bool useStringPooling);

static uoffset_t CopyTable_GenerateStringOffset(
    FlatBufferBuilder& fbb, const reflection::Schema* /*schema*/,
    const reflection::Object* /*tableDef*/, const reflection::Field* fieldDef,
    const Table* sourceTable, bool useStringPooling) {
  return createString(fbb, GetFieldS(*sourceTable, *fieldDef),
                      useStringPooling);
}

static uoffset_t CopyTable_GenerateVectorOffset(
    FlatBufferBuilder& fbb, const reflection::Schema* schema,
    const reflection::Object* tableDef, const reflection::Field* fieldDef,
    const Table* sourceTable, bool useStringPooling) {
  const auto* vector = getFieldVT(sourceTable, fieldDef);
  if (vector == nullptr) {
    return 0;
  }
  auto elementBaseType = fieldDef->type()->element();
  const auto* elementTableDef =
      elementBaseType == reflection::Obj
          ? schema->objects()->Get(fieldDef->type()->index())
          : nullptr;
  switch (elementBaseType) {
    case reflection::String: {
      auto elements = std::vector<Offset<const String*>>{};
      elements.resize(vector->size());
      const auto* stringVector =
          reinterpret_cast<const Vector<Offset<String>>*>(vector);
      for (uoffset_t i = 0; i < stringVector->size(); i++) {
        elements[i] = createString(fbb, stringVector->Get(i), useStringPooling);
      }
      return fbb.CreateVector(elements).o;
    }
    case reflection::Union: {
      const auto* unionTypeFieldDef = getUnionTypeField(tableDef, fieldDef);
      const auto* typeVector =
          getFieldVI<uint8_t>(sourceTable, unionTypeFieldDef);
      const auto size = (typeVector != nullptr)
                            ? std::min(vector->size(), typeVector->size())
                            : uoffset_t{0};
      auto elements = std::vector<Offset<const Table*>>{};
      elements.resize(size);
      for (uoffset_t i = 0; i < size; i++) {
        elements[i] = copyUnionMember(fbb, schema, fieldDef, typeVector->Get(i),
                                      vector->Get(i), useStringPooling);
      }
      return fbb.CreateVector(elements).o;
    }
    case reflection::Obj: {
      if (!elementTableDef->is_struct()) {
        std::vector<Offset<const Table*>> elements(vector->size());
        for (uoffset_t i = 0; i < vector->size(); i++) {
          elements[i] = CopyTable(fbb, *schema, *elementTableDef,
                                  *vector->Get(i), useStringPooling);
        }
        return fbb.CreateVector(elements).o;
      }
    }
      FLATBUFFERS_FALLTHROUGH();  // fall thru
    case reflection::UType:
    default: {  // Scalars and structs.
      auto elementSize = GetTypeSize(elementBaseType);
      auto elementAlignment = elementSize;  // For primitive elements
      if ((elementTableDef != nullptr) && elementTableDef->is_struct()) {
        elementSize = elementTableDef->bytesize();
      }
      fbb.StartVector(vector->size(), elementSize, elementAlignment);
      fbb.PushBytes(vector->Data(), elementSize * vector->size());
      return fbb.EndVector(vector->size());
    }
  }
  return 0;
}

static uoffset_t CopyTable_GenerateObjectOffset(
    FlatBufferBuilder& fbb, const reflection::Schema* schema,
    const reflection::Object* /*tableDef*/, const reflection::Field* fieldDef,
    const Table* sourceTable, bool useStringPooling) {
  const auto* subtableDef = schema->objects()->Get(fieldDef->type()->index());
  if (subtableDef->is_struct()) {
    return 0;
  }
  return CopyTable(fbb, *schema, *subtableDef,
                   *GetFieldT(*sourceTable, *fieldDef), useStringPooling)
      .o;
}

static uoffset_t CopyTable_GenerateUnionOffset(
    FlatBufferBuilder& fbb, const reflection::Schema* schema,
    const reflection::Object* tableDef, const reflection::Field* fieldDef,
    const Table* sourceTable, bool useStringPooling) {
  return copyUnionMember(fbb, schema, fieldDef,
                         storedUnionMember(tableDef, fieldDef, sourceTable),
                         GetFieldT(*sourceTable, *fieldDef), useStringPooling);
}

// A copy of the field `fieldDef` names, as `sourceTable` stores it, for every
// field built from an offset; 0 for one stored inline.
static uoffset_t copyStoredField(FlatBufferBuilder& fbb,
                                 const reflection::Schema* schema,
                                 const reflection::Object* tableDef,
                                 const reflection::Field* fieldDef,
                                 const Table* sourceTable,
                                 bool useStringPooling) {
  static const auto funcs = std::array<CopyTableFunc, reflection::MaxBaseType>{
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
      /* String */ CopyTable_GenerateStringOffset,
      /* Vector */ CopyTable_GenerateVectorOffset,
      /* Obj    */ CopyTable_GenerateObjectOffset,
      /* Union  */ CopyTable_GenerateUnionOffset,
      /* Array  */ nullptr,
  };
  auto func = funcs[fieldDef->type()->base_type()];
  return (func != nullptr) ? func(fbb, schema, tableDef, fieldDef, sourceTable,
                                  useStringPooling)
                           : 0;
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
        };
    auto func = funcs[baseType];
    assert(func);
    return func(fbb, schema, tableDef, fieldDef, sourceTable, payload, path,
                useStringPooling);
  }

  // If the field is in the source table, generate the subobject object. Since
  // there is no further changes down this path, use functions modeled after the
  // CopyTable function in the main flatbuffers repository.
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
  switch (fieldDef->type()->base_type()) {
    case reflection::String:
    case reflection::Union:
      return true;
    case reflection::Vector:
      return fieldDef->type()->element() != reflection::UType;
    case reflection::Obj:
      return not schema->objects()->Get(fieldDef->type()->index())->is_struct();
    default:
      return false;
  }
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
        unionTypeOffset = CopyTable_GenerateVectorOffset(
            fbb, schema, tableDef, unionTypeFieldDef, sourceTable,
            useStringPooling);
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
    case reflection::Vector: {
      if (takesOffset) {
        if (fieldDef->type()->element() == reflection::Union) {
          const auto* unionTypeFieldDef = getUnionTypeField(tableDef, fieldDef);
          fbb.AddOffset(unionTypeFieldDef->offset(),
                        Offset<void>(offsets[offsetIndex++]));
        }
        fbb.AddOffset(fieldDef->offset(), Offset<void>(offsets[offsetIndex++]));
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

// This largely duplicates functionality in CopyTable, but
// with extra logic to update fields based on the given operation.
uoffset_t applyRequestPut(FlatBufferBuilder& fbb,
                          const reflection::Schema* schema,
                          const reflection::Object* tableDef,
                          const Table* sourceTable,
                          std::span<const uint8_t> payload,
                          const std::span<const uint32_t> path,
                          bool useStringPooling) {
  if (path.empty()) {
    return CopyTable(fbb, *schema, *tableDef, payloadTableRoot(payload),
                     useStringPooling)
        .o;
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
  if (offsets != nullptr) {
    full.insert(full.end(), offsets->begin(), offsets->end());
  }
  return full;
}

// Applies each operation in `updates` (prefixing its offsets with `prefix`) to
// the running table, returning the accumulated flatbuffer. The first operation
// reads `sourceTable`; each subsequent one reads the previous result.
static flatbuffers::DetachedBuffer applyOperationList(
    const reflection::Schema* schema, const Table* sourceTable,
    std::span<const uint32_t> prefix,
    const Vector<serialized::Operation>* updateTypes,
    const Vector<flatbuffers::Offset<void>>* updates, bool useStringPooling) {
  const auto* rootDef = schema->root_table();

  // An empty (or null) update list is a no-op; return a standalone copy of the
  // source so every caller — including a nested empty patch — always gets a
  // valid buffer to thread onward.
  if (updates == nullptr or updates->size() == 0) {
    FlatBufferBuilder builder;
    builder.Finish(Offset<Table>(
        CopyTable(builder, *schema, *rootDef, *sourceTable, useStringPooling)
            .o));
    return builder.Release();
  }

  auto current = flatbuffers::DetachedBuffer{};
  const Table* source = sourceTable;

  for (uoffset_t i = 0; i < updates->size(); i++) {
    switch (updateTypes->Get(i)) {
      case Operation::Patch: {
        const auto* subpatch = static_cast<const Patch*>(updates->Get(i));
        current = applyOperationList(
            schema, source, concatPath(prefix, subpatch->offsets()),
            subpatch->updates_type(), subpatch->updates(), useStringPooling);
        break;
      }
      case Operation::Put: {
        const auto* put = static_cast<const Put*>(updates->Get(i));
        FlatBufferBuilder builder;
        auto storage = std::vector<PayloadWord>{};
        auto offset = applyRequestPut(
            builder, schema, rootDef, source,
            alignPayload(
                std::span{put->payload()->data(), put->payload()->size()},
                storage),
            concatPath(prefix, put->offsets()), useStringPooling);
        builder.Finish(Offset<Table>(offset));
        current = builder.Release();
        break;
      }
      case Operation::Delete: {
        const auto* del = static_cast<const Delete*>(updates->Get(i));
        FlatBufferBuilder builder;
        auto offset = applyRequestDelete(builder, schema, rootDef, source,
                                         concatPath(prefix, del->offsets()),
                                         useStringPooling);
        builder.Finish(Offset<Table>(offset));
        current = builder.Release();
        break;
      }
      case Operation::NONE:
        continue;  // skip; leave the running result untouched
    }
    source = flatbuffers::GetAnyRoot(current.data());
  }
  return current;
}

static uoffset_t applyRequestPatch(FlatBufferBuilder& fbb,
                                   const reflection::Schema* schema,
                                   const Table* sourceTable, const Patch* patch,
                                   bool useStringPooling) {
  const auto* rootDef = schema->root_table();
  const auto* updates = patch->updates();
  if (updates == nullptr or updates->size() == 0) {
    // A patch with no updates leaves the source unchanged.
    return CopyTable(fbb, *schema, *rootDef, *sourceTable, useStringPooling).o;
  }

  auto result = applyOperationList(
      schema, sourceTable,
      concatPath(std::span<const uint32_t>{}, patch->offsets()),
      patch->updates_type(), updates, useStringPooling);

  // Fold the accumulated result into the caller's builder.
  return CopyTable(fbb, *schema, *rootDef,
                   *flatbuffers::GetAnyRoot(result.data()), useStringPooling)
      .o;
}

// ---------------------------------------------------------------------------
// Delete
//
// Rebuilds a table from `sourceTable`, removing the field or vector element
// identified by `path`. Unaffected fields are copied via the same CopyTable_*
// helpers the Put path uses; the affected field is either omitted (terminal
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
      elements.push_back(
          CopyTable(fbb, *schema, *subDef, *vector->Get(i), useStringPooling)
              .o);
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
          return CopyTable_GenerateUnionOffset(fbb, schema, tableDef, fieldDef,
                                               sourceTable, useStringPooling);
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
                  fbb, vector, index, subDef->bytesize(), subDef->minalign());
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
                                             GetTypeSize(elementType));
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
        typeOffset = CopyTable_GenerateVectorOffset(
            fbb, schema, tableDef, unionTypeFieldDef, sourceTable,
            useStringPooling);
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
    case reflection::Vector: {
      if (present) {
        if (fieldDef->type()->element() == reflection::UType) {
          break;  // handled with the union field
        }
        if (fieldDef->type()->element() == reflection::Union) {
          const auto* unionTypeFieldDef = getUnionTypeField(tableDef, fieldDef);
          fbb.AddOffset(unionTypeFieldDef->offset(),
                        Offset<void>(offsets[offsetIndex++]));
        }
        fbb.AddOffset(fieldDef->offset(), Offset<void>(offsets[offsetIndex++]));
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

flatbuffers::Offset<const flatbuffers::Table*> fbrequest::applyRequest(
    FlatBufferBuilder& fbb, const reflection::Schema* schema,
    const Table* sourceTable, const Request* request, bool useStringPooling) {
  const auto* rootTableDef = schema->root_table();
  switch (request->operation_type()) {
    case Operation::Put: {
      const auto* put = request->operation_as_Put();
      const auto* offsets = put->offsets();
      auto storage = std::vector<PayloadWord>{};
      return applyRequestPut(
          fbb, schema, rootTableDef, sourceTable,
          alignPayload(
              std::span{put->payload()->data(), put->payload()->size()},
              storage),
          std::span{offsets->data(), offsets->size()}, useStringPooling);
    }
    case Operation::Patch: {
      const auto* patch = request->operation_as_Patch();
      return applyRequestPatch(fbb, schema, sourceTable, patch,
                               useStringPooling);
    }
    case Operation::Delete: {
      const auto* del = request->operation_as_Delete();
      const auto* offsets = del->offsets();
      return applyRequestDelete(fbb, schema, rootTableDef, sourceTable,
                                std::span{offsets->data(), offsets->size()},
                                useStringPooling);
    }
  }
  assert(false);
  return 0;
}
