#include "TableCopy.hpp"

#include <algorithm>
#include <charconv>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

using flatbuffers::FlatBufferBuilder;
using flatbuffers::GetTypeSize;
using flatbuffers::IsScalar;
using flatbuffers::Offset;
using flatbuffers::ReadScalar;
using flatbuffers::String;
using flatbuffers::Table;
using flatbuffers::uoffset64_t;
using flatbuffers::uoffset_t;
using flatbuffers::Vector;
using flatbuffers::Verifier;

namespace {

// The discriminant field FlatBuffers pairs with the union, or vector of
// unions, `fieldDef` names; null where `objectDef` declares none.
const reflection::Field* discriminantField(const reflection::Object* objectDef,
                                           const reflection::Field* fieldDef) {
  return objectDef->fields()->LookupByKey(
      (fieldDef->name()->str() + flatbuffers::UnionTypeFieldSuffix()).c_str());
}

// The union, or vector of unions, the discriminant field `typeFieldDef`
// belongs to; null where `objectDef` declares none.
const reflection::Field* discriminatedField(
    const reflection::Object* objectDef,
    const reflection::Field* typeFieldDef) {
  const auto name = typeFieldDef->name()->string_view();
  const auto suffix = std::string_view(flatbuffers::UnionTypeFieldSuffix());
  if (not name.ends_with(suffix)) {
    return nullptr;
  }
  return objectDef->fields()->LookupByKey(
      std::string(name.substr(0, name.size() - suffix.size())).c_str());
}

bool isDiscriminant(const reflection::Field* fieldDef) {
  const auto* type = fieldDef->type();
  return type->base_type() == reflection::UType
         or (type->base_type() == reflection::Vector
             and type->element() == reflection::UType);
}

// The type the member with discriminant `member` of the union whose enum
// `enumIndex` names holds, or null for NONE and for a discriminant the union
// does not declare. Compared untruncated, so a value past 8 bits is
// undeclared.
const reflection::Type* unionMemberType(const reflection::Schema* schema,
                                        int32_t enumIndex, uint32_t member) {
  if (member == 0 or enumIndex < 0
      or static_cast<uoffset_t>(enumIndex) >= schema->enums()->size()) {
    return nullptr;
  }
  const auto* enumDef = schema->enums()->Get(enumIndex);
  const auto* enumValue = enumDef->values()->LookupByKey(member);
  if (enumValue == nullptr or enumValue->union_type() == nullptr
      or enumValue->union_type()->base_type() == reflection::None) {
    return nullptr;
  }
  return enumValue->union_type();
}

const reflection::Object* objectOf(const reflection::Schema* schema,
                                   const reflection::Type* type) {
  return schema->objects()->Get(type->index());
}

// The stored vector `fieldDef` names in `table`, through whichever width of
// offset the field is held through; null where it is not stored.
const uint8_t* storedVector(const Table* table,
                            const reflection::Field* fieldDef) {
  return fieldDef->offset64()
             ? table->GetPointer<const uint8_t*, uoffset64_t>(
                   fieldDef->offset())
             : table->GetPointer<const uint8_t*>(fieldDef->offset());
}

const String* storedString(const Table* table,
                           const reflection::Field* fieldDef) {
  return fieldDef->offset64()
             ? table->GetPointer<const String*, uoffset64_t>(fieldDef->offset())
             : table->GetPointer<const String*>(fieldDef->offset());
}

bool hasWideLength(const reflection::Field* fieldDef) {
  return fieldDef->type()->base_type() == reflection::Vector64;
}

uint64_t vectorLength(const uint8_t* vector, bool wideLength) {
  return wideLength ? ReadScalar<uint64_t>(vector)
                    : uint64_t{ReadScalar<uint32_t>(vector)};
}

// The bytes one element of a vector of `type` takes in the vector: an
// inline scalar's or struct's own size, and an offset's otherwise.
size_t elementSize(const reflection::Schema* schema,
                   const reflection::Type* type) {
  const auto element = type->element();
  if (element == reflection::Obj and objectOf(schema, type)->is_struct()) {
    return objectOf(schema, type)->bytesize();
  }
  return IsScalar(element) ? GetTypeSize(element) : sizeof(uoffset_t);
}

// Whether the elements of a vector of `type` are stored in it whole, rather
// than through offsets.
bool hasInlineElements(const reflection::Schema* schema,
                       const reflection::Type* type) {
  const auto element = type->element();
  return IsScalar(element)
         or (element == reflection::Obj
             and objectOf(schema, type)->is_struct());
}

const flatbuffers::String* attributeValue(const reflection::Field* fieldDef,
                                          std::string_view key, bool& present) {
  present = false;
  if (fieldDef->attributes() == nullptr) {
    return nullptr;
  }
  for (const auto* attribute : *fieldDef->attributes()) {
    if (attribute->key() != nullptr
        and attribute->key()->string_view() == key) {
      present = true;
      return attribute->value();
    }
  }
  return nullptr;
}

bool isValidAlignment(size_t alignment) {
  return alignment != 0 and (alignment & (alignment - 1)) == 0
         and alignment <= FLATBUFFERS_MAX_ALIGNMENT;
}

// The widest alignment anything `schema` declares needs. A nested flatbuffer
// is aligned to the buffer it holds, whose alignment is that of the widest
// thing written into it; this bounds that without reading the buffer.
size_t forcedAlignment(const reflection::Field* fieldDef);

size_t widestAlignment(const reflection::Schema* schema) {
  auto widest = sizeof(flatbuffers::largest_scalar_t);
  for (const auto* objectDef : *schema->objects()) {
    widest = std::max(widest, static_cast<size_t>(objectDef->minalign()));
    for (const auto* fieldDef : *objectDef->fields()) {
      widest = std::max(widest, forcedAlignment(fieldDef));
    }
  }
  return widest;
}

bool verifyObject(Verifier& verifier, const reflection::Schema* schema,
                  const reflection::Object* objectDef, const Table* table);

bool verifyField(Verifier& verifier, const reflection::Schema* schema,
                 const reflection::Object* objectDef, const Table* table,
                 const reflection::Field* fieldDef);

// Whether `value`, a union value the verifier has reached through a checked
// offset, is the member `member` declares.
bool verifyUnionValue(Verifier& verifier, const reflection::Schema* schema,
                      int32_t enumIndex, uint32_t member,
                      const uint8_t* value) {
  if (member == 0) {
    // As FlatBuffers' own verifier, which reads nothing stored under NONE.
    // copiesFaithfully refuses to copy one.
    return true;
  }
  const auto* memberType = unionMemberType(schema, enumIndex, member);
  if (memberType == nullptr) {
    return false;
  }
  switch (memberType->base_type()) {
    case reflection::Obj: {
      const auto* memberDef = objectOf(schema, memberType);
      if (memberDef->is_struct()) {
        return verifier.VerifyFromPointer(value, memberDef->bytesize());
      }
      return verifyObject(verifier, schema, memberDef,
                          reinterpret_cast<const Table*>(value));
    }
    case reflection::String:
      return verifier.VerifyString(reinterpret_cast<const String*>(value));
    default:
      return false;
  }
}

// Whether the field at vtable offset `field` of `table`, of `size` bytes and
// `alignment`, lies within the buffer; true where it is not stored.
bool verifyInline(const Verifier& verifier, const Table* table,
                  flatbuffers::voffset_t field, size_t size, size_t alignment) {
  const auto fieldOffset = table->GetOptionalFieldOffset(field);
  return fieldOffset == 0
         or verifier.VerifyFieldStruct(reinterpret_cast<const uint8_t*>(table),
                                       fieldOffset, size, alignment);
}

bool verifyVector(Verifier& verifier, const reflection::Schema* schema,
                  const reflection::Object* objectDef, const Table* table,
                  const reflection::Field* fieldDef) {
  const auto* type = fieldDef->type();
  const auto* vector = storedVector(table, fieldDef);
  const auto wideLength = hasWideLength(fieldDef);
  const auto lengthChecked = wideLength
                                 ? verifier.VerifyVectorOrString<uint64_t>(
                                       vector, elementSize(schema, type))
                                 : verifier.VerifyVectorOrString<uint32_t>(
                                       vector, elementSize(schema, type));
  if (not lengthChecked) {
    return false;
  }
  if (hasInlineElements(schema, type)) {
    return true;
  }
  if (isWideOffsetField(fieldDef)) {
    // FlatBuffers stores no offsets in a vector held through a 64-bit
    // offset; nothing here says how wide they would be.
    return false;
  }
  const auto* offsets = reinterpret_cast<const Vector<Offset<Table>>*>(vector);
  switch (type->element()) {
    case reflection::String:
      for (uoffset_t i = 0; i < offsets->size(); i++) {
        if (not verifier.VerifyString(
                reinterpret_cast<const String*>(offsets->Get(i)))) {
          return false;
        }
      }
      return true;
    case reflection::Obj:
      for (uoffset_t i = 0; i < offsets->size(); i++) {
        if (not verifyObject(verifier, schema, objectOf(schema, type),
                             offsets->Get(i))) {
          return false;
        }
      }
      return true;
    case reflection::Union: {
      // The discriminants are read here, before the field loop may reach
      // them, so they are verified first.
      const auto* typeFieldDef = discriminantField(objectDef, fieldDef);
      if (typeFieldDef == nullptr or not isDiscriminant(typeFieldDef)
          or not table->CheckField(typeFieldDef->offset())
          or not verifyField(verifier, schema, objectDef, table,
                             typeFieldDef)) {
        return false;
      }
      const auto* types =
          table->GetPointer<const Vector<uint8_t>*>(typeFieldDef->offset());
      if (types->size() != offsets->size()) {
        return false;
      }
      for (uoffset_t i = 0; i < offsets->size(); i++) {
        if (not verifyUnionValue(
                verifier, schema, type->index(), types->Get(i),
                reinterpret_cast<const uint8_t*>(offsets->Get(i)))) {
          return false;
        }
      }
      return true;
    }
    default:
      return false;
  }
}

bool verifyField(Verifier& verifier, const reflection::Schema* schema,
                 const reflection::Object* objectDef, const Table* table,
                 const reflection::Field* fieldDef) {
  const auto* type = fieldDef->type();
  const auto baseType = type->base_type();
  const auto field = fieldDef->offset();
  if (IsScalar(baseType)) {
    return verifyInline(verifier, table, field, GetTypeSize(baseType),
                        GetTypeSize(baseType));
  }
  if (baseType == reflection::Obj and objectOf(schema, type)->is_struct()) {
    const auto* structDef = objectOf(schema, type);
    return verifyInline(verifier, table, field, structDef->bytesize(),
                        structDef->minalign());
  }
  if (not table->CheckField(field)) {
    return true;
  }
  const auto offsetChecked =
      isWideOffsetField(fieldDef)
          ? table->VerifyOffset<uoffset64_t>(verifier, field)
          : table->VerifyOffset<uoffset_t>(verifier, field);
  if (not offsetChecked) {
    return false;
  }
  switch (baseType) {
    case reflection::String:
      return verifier.VerifyString(storedString(table, fieldDef));
    case reflection::Obj:
      return verifyObject(verifier, schema, objectOf(schema, type),
                          table->GetPointer<const Table*>(field));
    case reflection::Union: {
      const auto* typeFieldDef = discriminantField(objectDef, fieldDef);
      if (typeFieldDef == nullptr or isWideOffsetField(fieldDef)
          or typeFieldDef->type()->base_type() != reflection::UType
          or not verifyField(verifier, schema, objectDef, table,
                             typeFieldDef)) {
        return false;
      }
      return verifyUnionValue(
          verifier, schema, type->index(),
          table->GetField<uint8_t>(typeFieldDef->offset(), 0),
          table->GetPointer<const uint8_t*>(field));
    }
    case reflection::Vector:
    case reflection::Vector64:
      return verifyVector(verifier, schema, objectDef, table, fieldDef);
    default:
      return false;
  }
}

bool verifyObject(Verifier& verifier, const reflection::Schema* schema,
                  const reflection::Object* objectDef, const Table* table) {
  if (objectDef->is_struct() or not table->VerifyTableStart(verifier)) {
    return false;
  }
  for (const auto* fieldDef : *objectDef->fields()) {
    if (not verifyField(verifier, schema, objectDef, table, fieldDef)) {
      return false;
    }
  }
  return verifier.EndTable();
}

uoffset_t copyString(FlatBufferBuilder& fbb, const String* str,
                     bool useStringPooling) {
  const auto view = std::string_view(str->c_str(), str->size());
  return useStringPooling ? fbb.CreateSharedString(view).o
                          : fbb.CreateString(view).o;
}

uoffset_t copyVector(FlatBufferBuilder& fbb, const reflection::Schema* schema,
                     const reflection::Object* objectDef, const Table* table,
                     const reflection::Field* fieldDef, bool useStringPooling) {
  const auto* type = fieldDef->type();
  const auto* vector = storedVector(table, fieldDef);
  if (vector == nullptr) {
    return 0;
  }
  const auto wideLength = hasWideLength(fieldDef);
  const auto length = vectorLength(vector, wideLength);
  if (hasInlineElements(schema, type)) {
    const auto* data =
        vector + (wideLength ? sizeof(uint64_t) : sizeof(uint32_t));
    const auto size = elementSize(schema, type);
    const auto alignment = vectorAlignment(schema, fieldDef);
    if (wideLength) {
      fbb.StartVector<Offset, uint64_t>(length, size, alignment);
      fbb.PushBytes(data, length * size);
      return fbb.EndVector<uint64_t, uoffset_t>(length);
    }
    fbb.StartVector(length, size, alignment);
    fbb.PushBytes(data, length * size);
    return fbb.EndVector(length);
  }
  const auto* offsets = reinterpret_cast<const Vector<Offset<Table>>*>(vector);
  auto elements = std::vector<Offset<void>>(offsets->size());
  switch (type->element()) {
    case reflection::String:
      for (uoffset_t i = 0; i < offsets->size(); i++) {
        elements[i] =
            copyString(fbb, reinterpret_cast<const String*>(offsets->Get(i)),
                       useStringPooling);
      }
      break;
    case reflection::Obj:
      for (uoffset_t i = 0; i < offsets->size(); i++) {
        elements[i] = copyTableDeep(fbb, schema, objectOf(schema, type),
                                    offsets->Get(i), useStringPooling);
      }
      break;
    case reflection::Union: {
      // Only copiesFaithfully decides that every element pairs with a
      // discriminant naming what it holds. A copy made without asking it
      // keeps the elements both vectors hold, and an empty table for an
      // element it cannot read, since a vector of offsets cannot omit one.
      const auto* typeFieldDef = discriminantField(objectDef, fieldDef);
      const auto* types = (typeFieldDef != nullptr)
                              ? table->GetPointer<const Vector<uint8_t>*>(
                                    typeFieldDef->offset())
                              : nullptr;
      elements.resize(
          (types != nullptr) ? std::min(offsets->size(), types->size()) : 0);
      for (uoffset_t i = 0; i < elements.size(); i++) {
        elements[i] = copyUnionValue(fbb, schema, type->index(), types->Get(i),
                                     offsets->Get(i), useStringPooling);
        if (elements[i].IsNull()) {
          elements[i] = fbb.EndTable(fbb.StartTable());
        }
      }
      break;
    }
    default:
      return 0;
  }
  return fbb.CreateVector(elements).o;
}

// The alignment a `force_align` attribute on `fieldDef` asks, or 1 where it
// carries none, or one that is not a valid alignment.
size_t forcedAlignment(const reflection::Field* fieldDef) {
  auto present = false;
  const auto* forced = attributeValue(fieldDef, "force_align", present);
  if (forced == nullptr) {
    return 1;
  }
  auto value = size_t{0};
  const auto text = forced->string_view();
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  return (error == std::errc{} and end == text.data() + text.size()
          and isValidAlignment(value))
             ? value
             : 1;
}

// Whether `table` stores a field at a vtable offset `objectDef` declares no
// field at, as a buffer written with a newer schema may. A copy driven by
// `objectDef` cannot keep it.
bool storesUndeclaredField(const reflection::Object* objectDef,
                           const Table* table) {
  const auto* data = reinterpret_cast<const uint8_t*>(table);
  const auto* vtable = data - ReadScalar<flatbuffers::soffset_t>(data);
  const auto vtableSize = ReadScalar<flatbuffers::voffset_t>(vtable);
  constexpr auto kFirstField =
      static_cast<flatbuffers::voffset_t>(2 * sizeof(flatbuffers::voffset_t));
  for (auto field = kFirstField; field < vtableSize;
       field += sizeof(flatbuffers::voffset_t)) {
    if (ReadScalar<flatbuffers::voffset_t>(vtable + field) == 0) {
      continue;
    }
    auto declared = false;
    for (const auto* fieldDef : *objectDef->fields()) {
      declared = declared or fieldDef->offset() == field;
    }
    if (not declared) {
      return true;
    }
  }
  return false;
}

}  // namespace

bool isWideOffsetField(const reflection::Field* fieldDef) {
  return fieldDef->offset64() or hasWideLength(fieldDef);
}

bool isOffsetField(const reflection::Schema* schema,
                   const reflection::Field* fieldDef) {
  switch (fieldDef->type()->base_type()) {
    case reflection::String:
    case reflection::Vector:
    case reflection::Vector64:
    case reflection::Union:
      return true;
    case reflection::Obj:
      return not objectOf(schema, fieldDef->type())->is_struct();
    default:
      return false;
  }
}

size_t vectorAlignment(const reflection::Schema* schema,
                       const reflection::Field* fieldDef) {
  const auto* type = fieldDef->type();
  auto alignment = (type->element() == reflection::Obj
                    and objectOf(schema, type)->is_struct())
                       ? static_cast<size_t>(objectOf(schema, type)->minalign())
                       : elementSize(schema, type);
  alignment = std::max(alignment, forcedAlignment(fieldDef));
  auto present = false;
  attributeValue(fieldDef, "nested_flatbuffer", present);
  if (present) {
    alignment = std::max(alignment, widestAlignment(schema));
  }
  attributeValue(fieldDef, "flexbuffer", present);
  if (present) {
    // The FlatBuffers parser aligns a flexbuffer to its widest scalar.
    alignment = std::max(alignment, sizeof(flatbuffers::largest_scalar_t));
  }
  return alignment;
}

bool unionValueCopiesFaithfully(const reflection::Schema* schema,
                                int32_t enumIndex, uint32_t member,
                                const void* value) {
  const auto* memberType = unionMemberType(schema, enumIndex, member);
  if (memberType == nullptr) {
    return false;
  }
  switch (memberType->base_type()) {
    case reflection::Obj: {
      const auto* memberDef = objectOf(schema, memberType);
      return memberDef->is_struct()
             or copiesFaithfully(schema, memberDef,
                                 static_cast<const Table*>(value));
    }
    case reflection::String:
      return true;
    default:
      return false;
  }
}

bool fieldCopiesFaithfully(const reflection::Schema* schema,
                           const reflection::Object* objectDef,
                           const Table* table,
                           const reflection::Field* fieldDef) {
  if (table == nullptr or not table->CheckField(fieldDef->offset())) {
    return true;
  }
  const auto* type = fieldDef->type();
  if (isDiscriminant(fieldDef)) {
    // The builders write a discriminant only beside its value, so one stored
    // without it would be lost. NONE, or no element, loses nothing.
    const auto* valueDef = discriminatedField(objectDef, fieldDef);
    if (valueDef == nullptr or table->CheckField(valueDef->offset())) {
      return true;
    }
    return (type->base_type() == reflection::UType)
               ? table->GetField<uint8_t>(fieldDef->offset(), 0) == 0
               : table->GetPointer<const Vector<uint8_t>*>(fieldDef->offset())
                         ->size()
                     == 0;
  }
  switch (type->base_type()) {
    case reflection::Obj: {
      const auto* subDef = objectOf(schema, type);
      return subDef->is_struct()
             or copiesFaithfully(
                 schema, subDef,
                 table->GetPointer<const Table*>(fieldDef->offset()));
    }
    case reflection::Union: {
      const auto* typeFieldDef = discriminantField(objectDef, fieldDef);
      return typeFieldDef != nullptr and not isWideOffsetField(fieldDef)
             and unionValueCopiesFaithfully(
                 schema, type->index(),
                 table->GetField<uint8_t>(typeFieldDef->offset(), 0),
                 table->GetPointer<const void*>(fieldDef->offset()));
    }
    case reflection::Vector:
    case reflection::Vector64: {
      if (hasInlineElements(schema, type)) {
        return true;
      }
      if (isWideOffsetField(fieldDef)) {
        return false;
      }
      const auto* offsets =
          table->GetPointer<const Vector<Offset<Table>>*>(fieldDef->offset());
      if (type->element() == reflection::Obj) {
        for (uoffset_t i = 0; i < offsets->size(); i++) {
          if (not copiesFaithfully(schema, objectOf(schema, type),
                                   offsets->Get(i))) {
            return false;
          }
        }
        return true;
      }
      if (type->element() == reflection::Union) {
        const auto* typeFieldDef = discriminantField(objectDef, fieldDef);
        const auto* types = (typeFieldDef != nullptr)
                                ? table->GetPointer<const Vector<uint8_t>*>(
                                      typeFieldDef->offset())
                                : nullptr;
        if (types == nullptr or types->size() != offsets->size()) {
          return false;
        }
        for (uoffset_t i = 0; i < offsets->size(); i++) {
          if (not unionValueCopiesFaithfully(schema, type->index(),
                                             types->Get(i), offsets->Get(i))) {
            return false;
          }
        }
        return true;
      }
      return type->element() == reflection::String;
    }
    case reflection::String:
      return true;
    default:
      return IsScalar(type->base_type());
  }
}

bool copiesFaithfully(const reflection::Schema* schema,
                      const reflection::Object* objectDef, const Table* table,
                      const reflection::Field* exceptField) {
  if (table == nullptr) {
    return true;
  }
  if (storesUndeclaredField(objectDef, table)) {
    return false;
  }
  for (const auto* fieldDef : *objectDef->fields()) {
    // A request writing or removing a union writes or removes its
    // discriminant with it.
    const auto skipped =
        exceptField != nullptr
        and (fieldDef == exceptField
             or (isDiscriminant(fieldDef)
                 and discriminatedField(objectDef, fieldDef) == exceptField));
    if (not skipped
        and not fieldCopiesFaithfully(schema, objectDef, table, fieldDef)) {
      return false;
    }
  }
  return true;
}

bool verifyTableStructure(const reflection::Schema* schema,
                          const reflection::Object* objectDef,
                          std::span<const uint8_t> bytes) {
  if (bytes.size() < sizeof(uoffset_t)
      or bytes.size() >= FLATBUFFERS_MAX_BUFFER_SIZE) {
    return false;
  }
  auto verifier = Verifier(bytes.data(), bytes.size());
  if (verifier.VerifyOffset<uoffset_t>(0) == 0) {
    return false;
  }
  return verifyObject(verifier, schema, objectDef,
                      flatbuffers::GetAnyRoot(bytes.data()));
}

bool verifyTableBuffer(const reflection::Schema* schema,
                       const reflection::Object* objectDef,
                       std::span<const uint8_t> bytes) {
  return verifyTableStructure(schema, objectDef, bytes)
         and copiesFaithfully(schema, objectDef,
                              flatbuffers::GetAnyRoot(bytes.data()));
}

uoffset_t copyUnionValue(FlatBufferBuilder& fbb,
                         const reflection::Schema* schema, int32_t enumIndex,
                         uint32_t member, const void* value,
                         bool useStringPooling) {
  const auto* memberType = unionMemberType(schema, enumIndex, member);
  if (memberType == nullptr) {
    return 0;
  }
  if (memberType->base_type() == reflection::String) {
    return copyString(fbb, static_cast<const String*>(value), useStringPooling);
  }
  const auto* memberDef = objectOf(schema, memberType);
  if (memberDef->is_struct()) {
    // A union stores a struct out of line, at the struct's own alignment.
    fbb.Align(memberDef->minalign());
    fbb.PushBytes(static_cast<const uint8_t*>(value), memberDef->bytesize());
    return fbb.GetSize();
  }
  return copyTableDeep(fbb, schema, memberDef, static_cast<const Table*>(value),
                       useStringPooling);
}

uoffset_t copyFieldOffset(FlatBufferBuilder& fbb,
                          const reflection::Schema* schema,
                          const reflection::Object* objectDef,
                          const Table* table, const reflection::Field* fieldDef,
                          bool useStringPooling) {
  if (table == nullptr or not table->CheckField(fieldDef->offset())
      or not isOffsetField(schema, fieldDef)) {
    return 0;
  }
  const auto* type = fieldDef->type();
  switch (type->base_type()) {
    case reflection::String:
      return copyString(fbb, storedString(table, fieldDef), useStringPooling);
    case reflection::Obj:
      return copyTableDeep(fbb, schema, objectOf(schema, type),
                           table->GetPointer<const Table*>(fieldDef->offset()),
                           useStringPooling);
    case reflection::Union: {
      const auto* typeFieldDef = discriminantField(objectDef, fieldDef);
      if (typeFieldDef == nullptr) {
        return 0;
      }
      return copyUnionValue(fbb, schema, type->index(),
                            table->GetField<uint8_t>(typeFieldDef->offset(), 0),
                            table->GetPointer<const void*>(fieldDef->offset()),
                            useStringPooling);
    }
    case reflection::Vector:
    case reflection::Vector64:
      return copyVector(fbb, schema, objectDef, table, fieldDef,
                        useStringPooling);
    default:
      return 0;
  }
}

void addFieldOffset(FlatBufferBuilder& fbb, const reflection::Field* fieldDef,
                    uoffset_t offset) {
  if (isWideOffsetField(fieldDef)) {
    fbb.AddOffset(fieldDef->offset(), flatbuffers::Offset64<void>(offset));
  } else {
    fbb.AddOffset(fieldDef->offset(), Offset<void>(offset));
  }
}

uoffset_t copyTableDeep(FlatBufferBuilder& fbb,
                        const reflection::Schema* schema,
                        const reflection::Object* objectDef, const Table* table,
                        bool useStringPooling) {
  if (table == nullptr) {
    return 0;
  }
  const auto* fields = objectDef->fields();
  auto offsets = std::vector<uoffset_t>(fields->size());
  for (uoffset_t i = 0; i < fields->size(); i++) {
    offsets[i] = copyFieldOffset(fbb, schema, objectDef, table, fields->Get(i),
                                 useStringPooling);
  }
  const auto start = fbb.StartTable();
  for (uoffset_t i = 0; i < fields->size(); i++) {
    const auto* fieldDef = fields->Get(i);
    if (not table->CheckField(fieldDef->offset())) {
      continue;
    }
    if (isOffsetField(schema, fieldDef)) {
      addFieldOffset(fbb, fieldDef, offsets[i]);
      continue;
    }
    const auto* type = fieldDef->type();
    const auto* structDef = (type->base_type() == reflection::Obj)
                                ? objectOf(schema, type)
                                : nullptr;
    const auto size = (structDef != nullptr)
                          ? static_cast<size_t>(structDef->bytesize())
                          : GetTypeSize(type->base_type());
    const auto alignment = (structDef != nullptr)
                               ? static_cast<size_t>(structDef->minalign())
                               : size;
    fbb.Align(alignment);
    fbb.PushBytes(table->GetStruct<const uint8_t*>(fieldDef->offset()), size);
    fbb.TrackField(fieldDef->offset(), fbb.GetSize());
  }
  return fbb.EndTable(start);
}
