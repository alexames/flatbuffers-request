#include "PayloadValue.hpp"

#include <cstring>
#include <vector>

#include "flatbuffers/flatbuffers.h"

using flatbuffers::GetTypeSize;
using flatbuffers::IsScalar;

namespace {

// A flexbuffer list. `IsVector` is true of a map too, and a typed vector
// reads as empty through `AsVector`, so neither is one.
bool isList(flexbuffers::Reference value) {
  return value.GetType() == flexbuffers::FBT_VECTOR;
}

// Whether `value` is a list of between 1 and `maxLength` scalars of
// `elementType`.
bool scalarListFits(flexbuffers::Reference value,
                    reflection::BaseType elementType, size_t maxLength) {
  if (not isList(value)) {
    return false;
  }
  const auto elements = value.AsVector();
  if (elements.size() == 0 or elements.size() > maxLength) {
    return false;
  }
  for (size_t i = 0; i < elements.size(); i++) {
    if (not scalarFits(elements[i], elementType)) {
      return false;
    }
  }
  return true;
}

void writeScalar(flexbuffers::Reference value, reflection::BaseType type,
                 std::span<uint8_t> at) {
  const auto bytes = scalarBytes(value, type);
  const auto size = GetTypeSize(type);
  if (at.size() >= size) {
    std::memcpy(at.data(), bytes.data(), size);
  }
}

// The member of `structDef` stored at `offset` bytes into it.
const reflection::Field* structFieldAtOffset(
    const reflection::Object* structDef, uint32_t offset) {
  for (const auto* fieldDef : *structDef->fields()) {
    if (fieldDef->offset() == offset) {
      return fieldDef;
    }
  }
  return nullptr;
}

bool writeStructMap(const reflection::Schema* schema,
                    const reflection::Object* structDef,
                    std::span<uint8_t> image, flexbuffers::Map map) {
  const auto keys = map.Keys();
  const auto values = map.Values();
  if (keys.size() == 0) {
    return false;
  }
  for (size_t i = 0; i < keys.size(); i++) {
    const auto* fieldDef = structDef->fields()->LookupByKey(keys[i].AsKey());
    if (fieldDef == nullptr) {
      return false;
    }
    const auto memberOffset = uint32_t{fieldDef->offset()};
    if (not writeStructPayload(schema, structDef, image,
                               std::span{&memberOffset, 1}, values[i])) {
      return false;
    }
  }
  return true;
}

}  // namespace

bool scalarFits(flexbuffers::Reference value, reflection::BaseType type) {
  if (not IsScalar(type) or type == reflection::UType) {
    return false;
  }
  return not value.IsString() and not value.IsMap() and not value.IsVector()
         and not value.IsTypedVector() and not value.IsFixedTypedVector()
         and not value.IsBlob();
}

std::array<uint8_t, 8> scalarBytes(flexbuffers::Reference value,
                                   reflection::BaseType type) {
  auto bytes = std::array<uint8_t, 8>{};
  const auto store = [&bytes](auto scalar) {
    std::memcpy(bytes.data(), &scalar, sizeof(scalar));
  };
  switch (type) {
    case reflection::Bool:
      store(value.AsBool());
      break;
    case reflection::Byte:
      store(value.AsInt8());
      break;
    case reflection::UType:
    case reflection::UByte:
      store(value.AsUInt8());
      break;
    case reflection::Short:
      store(value.AsInt16());
      break;
    case reflection::UShort:
      store(value.AsUInt16());
      break;
    case reflection::Int:
      store(value.AsInt32());
      break;
    case reflection::UInt:
      store(value.AsUInt32());
      break;
    case reflection::Long:
      store(value.AsInt64());
      break;
    case reflection::ULong:
      store(value.AsUInt64());
      break;
    case reflection::Float:
      store(value.AsFloat());
      break;
    case reflection::Double:
      store(value.AsDouble());
      break;
    default:
      break;
  }
  return bytes;
}

bool writeStructPayload(const reflection::Schema* schema,
                        const reflection::Object* structDef,
                        std::span<uint8_t> image,
                        std::span<const uint32_t> path,
                        flexbuffers::Reference value) {
  if (path.empty()) {
    return value.IsMap()
           and writeStructMap(schema, structDef, image, value.AsMap());
  }
  const auto* fieldDef = structFieldAtOffset(structDef, path.front());
  if (fieldDef == nullptr or fieldDef->offset() >= image.size()) {
    return false;
  }
  auto at = image.subspan(fieldDef->offset());
  const auto baseType = fieldDef->type()->base_type();
  const auto rest = path.subspan(1);

  if (baseType == reflection::Obj) {
    const auto* nested = schema->objects()->Get(fieldDef->type()->index());
    return writeStructPayload(schema, nested, at.first(nested->bytesize()),
                              rest, value);
  }
  if (baseType == reflection::Array) {
    const auto elementType = fieldDef->type()->element();
    if (not IsScalar(elementType)) {
      // An array of structs: `GetTypeSize` answers a pointer's width for one,
      // not the struct's, so every element would be written at the wrong
      // stride.
      return false;
    }
    const auto elementSize = GetTypeSize(elementType);
    const auto length = size_t{fieldDef->type()->fixed_length()};
    if (rest.empty()) {
      // A shorter array leaves the elements it does not reach.
      if (not scalarListFits(value, elementType, length)) {
        return false;
      }
      const auto elements = value.AsVector();
      for (size_t i = 0; i < elements.size(); i++) {
        writeScalar(elements[i], elementType, at.subspan(i * elementSize));
      }
      return true;
    }
    const auto index = size_t{rest.front()};
    if (index >= length or rest.size() != 1
        or not scalarFits(value, elementType)) {
      return false;
    }
    writeScalar(value, elementType, at.subspan(index * elementSize));
    return true;
  }
  if (not rest.empty() or not scalarFits(value, baseType)) {
    return false;
  }
  writeScalar(value, baseType, at);
  return true;
}

bool payloadFits(const reflection::Schema* schema, const reflection::Type* type,
                 bool element, flexbuffers::Reference value) {
  const auto baseType = element ? type->element() : type->base_type();
  switch (baseType) {
    case reflection::String:
      return value.IsString();
    case reflection::Obj: {
      const auto* objectDef = schema->objects()->Get(type->index());
      if (not objectDef->is_struct()) {
        return false;
      }
      auto image = std::vector<uint8_t>(objectDef->bytesize());
      return writeStructPayload(schema, objectDef, image, {}, value);
    }
    case reflection::Vector: {
      const auto elementType = type->element();
      if (IsScalar(elementType)) {
        // A whole vector may be empty, which an array may not.
        return isList(value)
               and (value.AsVector().size() == 0
                    or scalarListFits(value, elementType,
                                      value.AsVector().size()));
      }
      if (elementType != reflection::String or not isList(value)) {
        return false;
      }
      const auto elements = value.AsVector();
      for (size_t i = 0; i < elements.size(); i++) {
        if (not elements[i].IsString()) {
          return false;
        }
      }
      return true;
    }
    case reflection::Array:
      return IsScalar(type->element())
             and scalarListFits(value, type->element(), type->fixed_length());
    default:
      return scalarFits(value, baseType);
  }
}
