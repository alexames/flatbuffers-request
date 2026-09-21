#include <array>
#include <cstring>
#include <optional>
#include <span>

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
                                 std::span<const uint16_t> path,
                                 bool useStringPooling);

static uoffset_t applyRequestDelete(FlatBufferBuilder& fbb,
                                    const reflection::Schema* schema,
                                    const reflection::Object* tableDef,
                                    const Table* sourceTable,
                                    std::span<const uint16_t> path,
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

template <typename IntegerType>
const static Vector<IntegerType>* getFieldVI(
    const Table* table, const reflection::Field* fieldDef) {
  return table->GetPointer<const Vector<IntegerType>*>(fieldDef->offset());
}

static const Vector<Offset<Table>>* getFieldVT(
    const Table* table, const reflection::Field* fieldDef) {
  return table->GetPointer<const Vector<Offset<Table>>*>(fieldDef->offset());
}

static const Vector<Offset<String>>* getFieldVS(
    const Table* table, const reflection::Field* fieldDef) {
  return table->GetPointer<const Vector<Offset<String>>*>(fieldDef->offset());
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

static const reflection::Object* getUnionTableDefFromEnumIndex(
    const reflection::Schema* schema, const reflection::Field* fieldDef,
    uint8_t enumIndex) {
  const auto* enumDef = schema->enums()->Get(fieldDef->type()->index());
  const auto* enumValue = enumDef->values()->Get(enumIndex);
  const auto* unionType = enumValue->union_type();
  return schema->objects()->Get(unionType->index());
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
    const reflection::Object* structDef, uint16_t offset) {
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
                                     std::span<const uint16_t> path,
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
    const auto memberOffset = fieldDef->offset();
    const auto written = writeStructMember(schema, structDef, image,
                                           std::span{&memberOffset, 1},
                                           values[i]);
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
                                     std::span<const uint16_t> path,
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
    return writeStructMember(schema, nested, at.first(nested->bytesize()),
                             rest, value);
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
    std::span<const uint16_t> path) {
  auto image = std::vector<uint8_t>(structDef->bytesize(), uint8_t{0});
  if (fieldInSource) {
    const auto* stored =
        sourceTable->GetStruct<const uint8_t*>(fieldDef->offset());
    if (stored != nullptr) {
      std::memcpy(image.data(), stored, image.size());
    }
  }
  const auto value = flexbuffers::GetRoot(payload.data(), payload.size());
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
                             std::span<const uint16_t> path,
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
      const auto root = flexbuffers::GetRoot(payload.data(), payload.size());
      assert(root.IsString());
      return createString(fbb, root.AsString(), useStringPooling);
    }
    case reflection::Vector: {
      auto elementType = fieldDef->type()->element();
      const auto root = flexbuffers::GetRoot(payload.data(), payload.size());
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
        for (auto i = 0; i < vector.size(); i++) {
          auto blob = vector[i].AsBlob();
          const auto* tableDef =
              schema->objects()->Get(fieldDef->type()->index());
          tables[i] = CopyTable(fbb, *schema, *tableDef,
                                *GetAnyRoot(blob.data()), useStringPooling)
                          .o;
        }
        return fbb.CreateVector(tables.data(), tables.size()).o;
      }
      if (fieldDef->type()->element() == reflection::Union) {
        auto tables = std::vector<Offset<Table>>{};
        tables.resize(vector.size());
        for (auto i = 0; i < vector.size(); i++) {
          auto blob = vector[i].AsBlob();
          const auto* tableDef =
              schema->objects()->Get(fieldDef->type()->index());
          tables[i] = CopyTable(fbb, *schema, *tableDef,
                                *GetAnyRoot(blob.data()), useStringPooling)
                          .o;
        }
        return fbb.CreateVector(tables.data(), tables.size()).o;
      } else {
        return 0;
      }
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
      return CopyTable(fbb, *schema, *subTableDef, *GetAnyRoot(payload.data()),
                       useStringPooling)
          .o;
    }
    case reflection::Union: {
      assert(path.size() == 2);
      auto unionEnumIndex = path[1];
      const auto* unionTableDef =
          getUnionTableDefFromEnumIndex(schema, fieldDef, unionEnumIndex);
      return CopyTable(fbb, *schema, *unionTableDef,
                       *GetAnyRoot(payload.data()), useStringPooling)
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

static std::span<const uint16_t> updatePath(std::span<const uint16_t> path) {
  assert(path.size() > 0);
  return {path.last(path.size() - 1)};
}

static void copyOperationScalar(FlatBufferBuilder& fbb,
                                const reflection::Field* fieldDef,
                                std::span<const uint8_t> payload, size_t align,
                                size_t size) {
  auto baseType = fieldDef->type()->base_type();
  auto root = flexbuffers::GetRoot(payload.data(), payload.size());
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
    std::span<const uint16_t> path, bool useStringPooling) {
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
    std::span<const uint8_t> payload, std::span<const uint16_t> path,
    reflection::BaseType elementBaseType,
    std::array<uint8_t, 8> defaultValue = std::array<uint8_t, 8>{}) {
  const auto index = size_t{path.front()};
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
  auto root = flexbuffers::GetRoot(payload.data(), payload.size());
  auto bytes = std::array<uint8_t, 8>{};
  getFlexbufferScalarDataPointer(root, elementBaseType, &bytes);
  fbb.PushBytes(bytes.data(), elementSize);

  // If the new element is beyond the edge, insert 0's to pad the vector.
  for (int i = initialValues; i < index; i++) {
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
    std::span<const uint16_t>& path, std::span<const uint8_t> payload,
    bool useStringPooling) {
  const auto* vector = getFieldVS(sourceTable, fieldDef);
  auto index = path.front();
  auto elements = std::vector<Offset<const String*>>{};
  elements.resize(std::max(vector->size(), static_cast<uoffset_t>(index + 1)));
  for (uoffset_t i = 0; i < vector->size(); i++) {
    if (i == index) {
      elements[i] = copyPayload(fbb, schema, fieldDef, payload, path, true,
                                useStringPooling);
    } else {
      elements[i] = createString(fbb, vector->Get(i), useStringPooling);
    }
  }
  for (uoffset_t i = vector->size(); i < index; i++) {
    elements[i] = createString(fbb, "", useStringPooling);
  }
  if (index >= vector->size()) {
    elements[index] = copyPayload(fbb, schema, fieldDef, payload, path, true,
                                  useStringPooling);
  }
  return fbb.CreateVector(elements).o;
}

static uoffset_t applyRequestPut_GenerateObjectVectorOffset(
    flatbuffers::FlatBufferBuilder& fbb, const reflection::Schema* schema,
    const reflection::Field* fieldDef, const Table* sourceTable,
    std::span<const uint16_t> path, std::span<const uint8_t> payload,
    bool useStringPooling) {
  const auto* subtableDef = schema->objects()->Get(fieldDef->type()->index());
  const auto* vector = getFieldVT(sourceTable, fieldDef);
  auto index = path.front();
  auto elements = std::vector<Offset<Table>>{};
  elements.resize(std::max(vector->size(), index + 1U));
  for (uoffset_t i = 0; i < vector->size(); i++) {
    if (i == index) {
      if (subtableDef->is_struct()) {
        return 0;
      }
      elements[i] =
          applyRequestPut(fbb, schema, subtableDef, vector->Get(index), payload,
                          updatePath(path), useStringPooling);
    } else {
      elements[i] = CopyTable(fbb, *schema, *subtableDef, *vector->Get(i),
                              useStringPooling)
                        .o;
    }
  }
  for (uoffset_t i = vector->size(); i < index; i++) {
    elements[i] = fbb.EndTable(fbb.StartTable());
  }
  if (index >= vector->size()) {
    elements[index] =
        applyRequestPut(fbb, schema, subtableDef, nullptr, payload,
                        updatePath(path), useStringPooling);
  }
  return fbb.CreateVector(elements).o;
}

static uoffset_t applyRequestPut_GenerateUTypeVectorOffset(
    flatbuffers::FlatBufferBuilder& fbb, const reflection::Field* fieldDef,
    const Table* sourceTable, std::span<const uint16_t> path,
    std::span<const uint8_t> payload, bool useStringPooling) {
  const auto* vector =
      sourceTable->GetPointer<const VectorOfAny*>(fieldDef->offset());

  auto flexbufferValue =
      std::array<uint8_t, 3>{static_cast<uint8_t>(path[1]), 0x08, 0x01};
  static const auto kDefaultEnumValues =
      std::array<uint8_t, 8>{1, 1, 1, 1, 1, 1, 1, 1};
  return applyRequestPut_GenerateScalarVectorOffset(
      fbb, vector, flexbufferValue, path, reflection::UType,
      kDefaultEnumValues);
}

static uoffset_t applyRequestPut_GenerateUnionVectorOffset(
    flatbuffers::FlatBufferBuilder& fbb, const reflection::Schema* schema,
    const reflection::Object* tableDef, const reflection::Field* fieldDef,
    const Table* sourceTable, std::span<const uint16_t> path,
    std::span<const uint8_t> payload, bool useStringPooling) {
  const auto* vector = getFieldVT(sourceTable, fieldDef);
  const auto* unionTypeFieldDef = getUnionTypeField(tableDef, fieldDef);
  const auto* unionTypeVector =
      getFieldVI<uint8_t>(sourceTable, unionTypeFieldDef);
  auto index = path.front();
  auto elements = std::vector<Offset<Table>>{};
  path = updatePath(path);
  elements.resize(std::max(vector->size(), index + 1U));
  for (uoffset_t i = 0; i < vector->size(); i++) {
    if (i == index) {
      // This is the index that is going to be overwritten.
      auto unionEnumIndex = path.front();
      const auto* unionTableDef = getUnionTableDefFromEnumIndex(
          schema, unionTypeFieldDef, unionEnumIndex);
      elements[i] =
          applyRequestPut(fbb, schema, unionTableDef, vector->Get(index),
                          payload, updatePath(path), useStringPooling);
    } else {
      const auto* unionTypeTable = getUnionTableDefFromEnumIndex(
          schema, unionTypeFieldDef, unionTypeVector->Get(i));
      elements[i] = CopyTable(fbb, *schema, *unionTypeTable, *vector->Get(i),
                              useStringPooling)
                        .o;
    }
  }
  for (uoffset_t i = vector->size(); i < index; i++) {
    elements[i] = fbb.EndTable(fbb.StartTable());
  }
  if (index >= vector->size()) {
    auto unionEnumIndex = path.front();
    const auto* unionTableDef = getUnionTableDefFromEnumIndex(
        schema, unionTypeFieldDef, unionEnumIndex);
    elements[index] =
        applyRequestPut(fbb, schema, unionTableDef, nullptr, payload,
                        updatePath(path), useStringPooling);
  }
  return fbb.CreateVector(elements).o;
}

using ApplyPutRequestFunc = uoffset_t (*)(
    FlatBufferBuilder& fbb, const reflection::Schema* schema,
    const reflection::Object* /*object*/, const reflection::Field* fieldDef,
    const Table* sourceTable, std::span<const uint8_t> payload,
    std::span<const uint16_t> path, bool useStringPooling);

static uoffset_t applyRequestPut_GenerateVectorOffset(
    FlatBufferBuilder& fbb, const reflection::Schema* schema,
    const reflection::Object* tableDef, const reflection::Field* fieldDef,
    const Table* sourceTable, std::span<const uint8_t> payload,
    std::span<const uint16_t> path, bool useStringPooling) {
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
      const auto* vector =
          sourceTable->GetPointer<const VectorOfAny*>(fieldDef->offset());
      return applyRequestPut_GenerateScalarVectorOffset(fbb, vector, payload,
                                                        path, elementBaseType);
    }
  }
}

static uoffset_t applyRequestPut_GenerateUnionOffset(
    FlatBufferBuilder& fbb, const reflection::Schema* schema,
    const reflection::Object* tableDef, const reflection::Field* fieldDef,
    const Table* sourceTable, std::span<const uint8_t> payload,
    std::span<const uint16_t> path, bool useStringPooling) {
  path = updatePath(path);
  const auto requestedMember = static_cast<uint8_t>(path.front());
  const auto* subtableDef =
      getUnionTableDefFromEnumIndex(schema, fieldDef, requestedMember);
  path = updatePath(path);
  // The MEMBER is what the member's own fields are copied from, and only
  // while the stored member is the one being written. The table holding the
  // union is not it: reading the member's fields out of the parent lands at
  // the parent's offsets, which is another field or nothing at all. A request
  // that switches member has nothing to copy, and must not carry the old
  // member's bytes into the new one.
  const Table* storedMember = nullptr;
  if (sourceTable != nullptr) {
    const auto* typeFieldDef = getUnionTypeField(tableDef, fieldDef);
    if (typeFieldDef != nullptr
        && GetFieldI<uint8_t>(*sourceTable, *typeFieldDef) == requestedMember) {
      storedMember = GetFieldT(*sourceTable, *fieldDef);
    }
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
      const auto* typeVector = sourceTable->GetPointer<const Vector<uint8_t>*>(
          unionTypeFieldDef->offset());
      assert(vector->size() == typeVector->size());
      auto elements = std::vector<Offset<const Table*>>{};
      elements.resize(vector->size());
      for (uoffset_t i = 0; i < vector->size(); i++) {
        const auto* unionTypeTable =
            getUnionTableDefFromEnumIndex(schema, fieldDef, typeVector->Get(i));
        elements[i] = CopyTable(fbb, *schema, *unionTypeTable, *vector->Get(i),
                                useStringPooling);
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
  const auto& subtableDef =
      GetUnionType(*schema, *tableDef, *fieldDef, *sourceTable);
  return CopyTable(fbb, *schema, subtableDef,
                   *GetFieldT(*sourceTable, *fieldDef), useStringPooling)
      .o;
}

static uoffset_t applyRequestPut_GenerateFieldOffset(
    FlatBufferBuilder& fbb, const reflection::Schema* schema,
    const reflection::Object* tableDef, const reflection::Field* fieldDef,
    const Table* sourceTable, std::span<const uint8_t> payload,
    std::span<const uint16_t> path, bool useStringPooling) {
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
    static const auto funcs =
        std::array<CopyTableFunc, reflection::MaxBaseType>{
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
    auto func = funcs[baseType];
    return (func != nullptr) ? func(fbb, schema, tableDef, fieldDef,
                                    sourceTable, useStringPooling)
                             : 0;
  }
  return 0;
}

static std::vector<uoffset_t> applyRequestPut_GenerateOffsets(
    FlatBufferBuilder& fbb, const reflection::Schema* schema,
    const reflection::Object* tableDef, const Table* sourceTable,
    std::span<const uint8_t> payload, std::span<const uint16_t> path,
    bool useStringPooling) {
  auto offsets = std::vector<uoffset_t>{};
  for (const auto* fieldDef : *tableDef->fields()) {
    auto offset = applyRequestPut_GenerateFieldOffset(
        fbb, schema, tableDef, fieldDef, sourceTable, payload, path,
        useStringPooling);

    // Special case for Vectors of unions, since all Union vectors must also be
    // accompanied by a UType vector.
    if (offset != 0 and fieldDef->type()->base_type() == reflection::Vector
        and fieldDef->type()->element() == reflection::Union) {
      uoffset_t unionTypeOffset = 0;
      const auto* unionTypeFieldDef = getUnionTypeField(tableDef, fieldDef);
      if (path.front() == fieldDef->offset()) {
        unionTypeOffset = applyRequestPut_GenerateUTypeVectorOffset(
            fbb, unionTypeFieldDef, sourceTable, updatePath(path), payload,
            useStringPooling);
      } else {
        unionTypeOffset = CopyTable_GenerateVectorOffset(
            fbb, schema, tableDef, unionTypeFieldDef, sourceTable,
            useStringPooling);
      }
      offsets.push_back(unionTypeOffset);
    }

    if (offset != 0U) {
      offsets.push_back(offset);
    }
  }
  return offsets;
}

static void applyRequestPut_BuildTableField(
    flatbuffers::FlatBufferBuilder& fbb, const reflection::Schema* schema,
    const reflection::Object* tableDef, const reflection::Field* fieldDef,
    const Table* sourceTable, std::span<const uint8_t> payload,
    std::span<const uint16_t> path,
    const std::vector<flatbuffers::uoffset_t>& offsets, size_t& offsetIndex) {
  auto inPath = path.front() == fieldDef->offset();
  auto fieldInSource =
      (sourceTable != nullptr) and sourceTable->CheckField(fieldDef->offset());
  auto baseType = fieldDef->type()->base_type();

  switch (baseType) {
    case reflection::UType: {
      // Do nothing, the union will handle it.
      break;
    }
    case reflection::String:
    case reflection::Vector: {
      if (inPath or fieldInSource) {
        if (fieldDef->type()->element() == reflection::UType) {
          break;
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
      if (inPath or fieldInSource) {
        const auto* subtableDef =
            schema->objects()->Get(fieldDef->type()->index());
        if (not subtableDef->is_struct()) {
          fbb.AddOffset(fieldDef->offset(),
                        Offset<void>(offsets[offsetIndex++]));
        } else if (inPath) {
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
      // If this union was changed, make sure to also update the union's type
      // enum.
      if (inPath or fieldInSource) {
        const auto* unionEnumFieldDef = getUnionTypeField(tableDef, fieldDef);
        auto enumValue =
            inPath ? static_cast<uint8_t>(path[1])
                   : GetFieldI<uint8_t>(*sourceTable, *unionEnumFieldDef);
        auto size = GetTypeSize(unionEnumFieldDef->type()->base_type());

        assert(size == 1);
        fbb.Align(size);
        fbb.PushBytes(reinterpret_cast<uint8_t*>(&enumValue), size);
        fbb.TrackField(unionEnumFieldDef->offset(), fbb.GetSize());
      }
      if (inPath or fieldInSource) {
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
    std::span<const uint8_t> payload, std::span<const uint16_t> path,
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
                          const std::span<const uint16_t> path,
                          bool useStringPooling) {
  if (path.empty()) {
    return CopyTable(fbb, *schema, *tableDef, *GetAnyRoot(payload.data()),
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

static std::vector<uint16_t> concatPath(std::span<const uint16_t> prefix,
                                        const Vector<uint16_t>* offsets) {
  auto full = std::vector<uint16_t>(prefix.begin(), prefix.end());
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
    std::span<const uint16_t> prefix,
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
        auto offset = applyRequestPut(
            builder, schema, rootDef, source,
            std::span{put->payload()->data(), put->payload()->size()},
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
      concatPath(std::span<const uint16_t>{}, patch->offsets()),
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
    std::span<const uint16_t> deeperPath, bool useStringPooling) {
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
    std::span<const uint16_t> deeperPath, bool useStringPooling) {
  const auto* vector = getFieldVT(sourceTable, fieldDef);
  const auto* unionTypeFieldDef = getUnionTypeField(tableDef, fieldDef);
  const auto* typeVector = getFieldVI<uint8_t>(sourceTable, unionTypeFieldDef);
  const auto existingSize = (vector != nullptr and typeVector != nullptr)
                                ? vector->size()
                                : uoffset_t{0};
  // The path selects the element's union member (e.g. `.Integer`); that enum
  // step is only navigation, so strip it before deciding remove-vs-recurse.
  const auto memberPath =
      deeperPath.empty() ? deeperPath : deeperPath.subspan(1);
  auto elements = std::vector<Offset<Table>>{};
  for (uoffset_t i = 0; i < existingSize; i++) {
    const auto* unionDef = getUnionTableDefFromEnumIndex(
        schema, unionTypeFieldDef, typeVector->Get(i));
    if (i == index) {
      if (memberPath.empty()) {
        continue;  // remove this element
      }
      elements.push_back(Offset<Table>(
          applyRequestDelete(fbb, schema, unionDef, vector->Get(i), memberPath,
                             useStringPooling)));
    } else {
      elements.push_back(
          CopyTable(fbb, *schema, *unionDef, *vector->Get(i), useStringPooling)
              .o);
    }
  }
  return fbb.CreateVector(elements.data(), elements.size()).o;
}

static uoffset_t applyRequestDelete_GenerateFieldOffset(
    FlatBufferBuilder& fbb, const reflection::Schema* schema,
    const reflection::Object* tableDef, const reflection::Field* fieldDef,
    const Table* sourceTable, std::span<const uint16_t> path,
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
    switch (baseType) {
      case reflection::Obj: {
        const auto* subDef = schema->objects()->Get(fieldDef->type()->index());
        if (subDef->is_struct() or path.size() == 1) {
          return 0;  // delete the whole (struct or table) field
        }
        const auto* table = (sourceTable != nullptr)
                                ? GetFieldT(*sourceTable, *fieldDef)
                                : nullptr;
        return applyRequestDelete(fbb, schema, subDef, table, updatePath(path),
                                  useStringPooling);
      }
      case reflection::Union: {
        if (path.size() <= 2) {
          return 0;  // delete the whole union (its type is dropped in build)
        }
        const auto* unionDef =
            getUnionTableDefFromEnumIndex(schema, fieldDef, path[1]);
        const auto* table = (sourceTable != nullptr)
                                ? GetFieldT(*sourceTable, *fieldDef)
                                : nullptr;
        return applyRequestDelete(fbb, schema, unionDef, table, path.subspan(2),
                                  useStringPooling);
      }
      case reflection::Vector: {
        if (path.size() == 1) {
          return 0;  // delete the whole vector
        }
        if (not inSource) {
          return 0;  // element op on an absent vector: nothing to remove
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
              // A struct vector stores fixed-size inline elements.
              const auto* vector = sourceTable->GetPointer<const VectorOfAny*>(
                  fieldDef->offset());
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
    static const auto funcs =
        std::array<CopyTableFunc, reflection::MaxBaseType>{
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
    auto func = funcs[baseType];
    return (func != nullptr) ? func(fbb, schema, tableDef, fieldDef,
                                    sourceTable, useStringPooling)
                             : 0;
  }
  return 0;
}

static std::vector<uoffset_t> applyRequestDelete_GenerateOffsets(
    FlatBufferBuilder& fbb, const reflection::Schema* schema,
    const reflection::Object* tableDef, const Table* sourceTable,
    std::span<const uint16_t> path, bool useStringPooling) {
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
      // element delete (size 3) drops that index from the parallel type vector
      // too; a copy or a recurse into the element leaves the types unchanged.
      if (inPath and path.size() == 3) {
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
    const Table* sourceTable, std::span<const uint16_t> path,
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
        deletedWhole = path.size() <= 2;
        break;
      case reflection::Vector:
        deletedWhole = path.size() == 1;
        break;
      default:  // scalar or string
        deletedWhole = true;
        break;
    }
  }
  const bool present =
      (inPath and not deletedWhole) or (inSource and not inPath);

  switch (baseType) {
    case reflection::UType:
      break;  // handled with the union field
    case reflection::String:
    case reflection::Vector: {
      // A string/vector comes from the source: as a copy, or as the target of
      // an element/recurse op. It is absent for a whole delete and for an
      // element op on a field the source lacks (matching the generate phase, so
      // the offset stream stays aligned).
      const bool vectorPresent = inSource and (not inPath or not deletedWhole);
      if (vectorPresent) {
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
                                    std::span<const uint16_t> path,
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
      return applyRequestPut(
          fbb, schema, rootTableDef, sourceTable,
          std::span{put->payload()->data(), put->payload()->size()},
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
