#include <optional>
#include <span>

#include "StringScan.hpp"
#include "fbrequest/FlatbufferRequest.hpp"
#include "flatbuffers/flatbuffers.h"
#include "flatbuffers/idl.h"
#include "flatbuffers/reflection.h"

using flatbuffers::FlatBufferBuilder;
using flatbuffers::Offset;
using flatbuffers::Parser;
using flatbuffers::String;
using flatbuffers::Table;
using flatbuffers::uoffset_t;
using flatbuffers::Vector;
using flatbuffers::VectorOfAny;
using serialized::CreateDelete;
using serialized::CreatePatch;
using serialized::CreatePut;
using serialized::CreateRequest;
using serialized::Delete;
using serialized::FinishRequestBuffer;
using serialized::Operation;
using serialized::Patch;
using serialized::Put;

namespace {

// Forward declarations for the mutual recursion between patch parsing and the
// generic operation parser (a patch's updates are themselves operations).
std::optional<Offset<void>> parseFlatbufferOperation(
    FlatBufferBuilder& fbb, std::string_view* requestString,
    Operation operation, const reflection::Schema* schema,
    const reflection::Object* startObject = nullptr);
std::optional<Operation> parseFlatbufferOperationEnum(
    std::string_view* requestString);

const reflection::Enum* getUnionEnum(const reflection::Schema* schema,
                                     const reflection::Field* unionField) {
  return schema->enums()->Get(unionField->type()->index());
}

bool parseFlatbufferOffsetPath_ParseUnionName(
    const reflection::Schema* schema, const reflection::Field* field,
    std::string_view* path, std::vector<uint16_t>* outOffsets,
    const reflection::Object** outObject) {
  if (not advanceOver(*path, ".")) {
    return false;
  }
  const std::string_view unionName = path->substr(0, findAnyOf(*path, ".[ "));
  if (unionName.empty()) {
    return false;
  }
  path->remove_prefix(unionName.size());

  const auto* unionEnum = getUnionEnum(schema, field);
  auto unionValue =
      std::ranges::find_if(*unionEnum->values(), [&unionName](auto enumValue) {
        return enumValue->name()->string_view() == unionName;
      });
  if (unionValue == unionEnum->values()->end()) {
    return false;
  }
  outOffsets->push_back(unionValue->value());
  *outObject = schema->objects()->Get(unionValue->union_type()->index());
  return true;
}

// `writing` refuses the paths only a `put` cannot serve: a delete resolves
// the same text and removes a whole element, which is well defined where
// writing one is not.
std::optional<Offset<Vector<uint16_t>>> parseFlatbufferOffsetPath(
    FlatBufferBuilder& fbb, const reflection::Schema* schema,
    std::string_view* path, const reflection::Type** outType,
    const reflection::Object** outObject, bool* outElementInVector,
    const reflection::Object* startObject = nullptr, bool writing = false) {
  *outType = nullptr;
  // Paths inside a patch's nested operations are relative to the patched
  // subobject rather than the root table.
  *outObject = (startObject != nullptr) ? startObject : schema->root_table();
  *outElementInVector = false;

  advanceOver(*path, " ");
  if (path->empty()) {
    return std::nullopt;
  }

  auto offsets = std::vector<uint16_t>{};
  do {
    *outElementInVector = false;
    if (*outObject == nullptr) {
      return std::nullopt;
    }

    const auto fieldName = path->substr(0, findAnyOf(*path, ".[ "));
    path->remove_prefix(fieldName.size());
    auto field =
        std::ranges::find_if(*(*outObject)->fields(), [&fieldName](auto field) {
          return field->name() and field->name()->string_view() == fieldName;
        });

    if (field == (*outObject)->fields()->end()) {
      return std::nullopt;
    }

    offsets.push_back(field->offset());

    auto type = *outType = field->type();
    switch (type->base_type()) {
      case reflection::Array:
      case reflection::Vector: {
        std::optional<int> arrayIndex;
        if (advanceOver(*path, "[") and (arrayIndex = advanceOverInt(*path))
            and advanceOver(*path, "]")) {
          // Getting a path to a specific element in the vector.
          *outElementInVector = true;
          offsets.push_back(*arrayIndex);
          auto elementBaseType = field->type()->element();
          if (elementBaseType == reflection::Obj) {
            const auto* elementDef = schema->objects()->Get(type->index());
            if (writing and elementDef->is_struct()) {
              // A struct element is inline, and nothing builds one: the apply
              // generates no offset for it while the table builder still
              // consumes one, which reads a neighbouring field's offset.
              return std::nullopt;
            }
            *outObject = elementDef;
          } else if (elementBaseType == reflection::Union) {
            if (not parseFlatbufferOffsetPath_ParseUnionName(
                    schema, *field, path, &offsets, outObject)) {
              return std::nullopt;
            }
          } else {
            *outObject = nullptr;
          }
        } else {
          // Getting a path to the vector itself. Only a vector of TABLES
          // names an object. `index` is set for a vector of enums and for a
          // vector of unions too, where it names the ENUM, so reading it as
          // an object index answers an unrelated object or runs off the end
          // of the vector. A vector of unions has no whole-vector payload
          // anyway: the types of its elements live in a second vector that a
          // JSON array cannot carry.
          const auto* elementDef =
              (type->element() == reflection::Obj)
                  ? schema->objects()->Get(type->index())
                  : nullptr;
          if (writing
              and (type->element() == reflection::Union
                   or (elementDef != nullptr and elementDef->is_struct()))) {
            // A vector of unions has no whole-vector payload, and a vector of
            // structs holds its elements inline with nothing to build one
            // from. Neither is written here.
            return std::nullopt;
          }
          *outObject = elementDef;
        }
        break;
      }
      case reflection::Obj: {
        *outObject = schema->objects()->Get(type->index());
        break;
      }
      case reflection::Union: {
        if (not parseFlatbufferOffsetPath_ParseUnionName(schema, *field, path,
                                                         &offsets, outObject)) {
          return std::nullopt;
        }
        break;
      }
      default: {
        *outObject = nullptr;
      }
    }
  } while (not path->empty() and advanceOver(*path, "."));
  return fbb.CreateVector(offsets);
}

std::optional<Offset<Put>> parseFlatbufferPut_ParseVector(
    FlatBufferBuilder& fbb, std::string_view* requestString,
    const reflection::Schema* schema, const reflection::Object* typeObject,
    Offset<Vector<uint16_t>>& offsets) {
  // Parsing a vector.
  auto parser = Parser{};
  parser.opts.require_json_eof = false;
  if ((typeObject == nullptr) or not parser.Deserialize(schema)
      or not parser.SetRootType(typeObject->name()->c_str())) {
    return std::nullopt;
  }

  if (not advanceOver(*requestString, " ")
      or not advanceOver(*requestString, "[")) {
    return std::nullopt;
  }
  auto flexBuilder = flexbuffers::Builder{};
  auto start = flexBuilder.StartVector();
  if (not requestString->starts_with("]")) {
    do {
      if (not parser.ParseJson(requestString->data())) {
        return std::nullopt;
      }
      // Step over the text in the input json that was parsed.
      requestString->remove_prefix(parser.BytesConsumed());

      auto span = parser.builder_.GetBufferSpan();
      flexBuilder.Blob(span.data(), span.size());
    } while (advanceOver(*requestString, ","));
  }
  if (not advanceOver(*requestString, "]")) {
    return std::nullopt;
  }
  flexBuilder.EndVector(start, false, false);
  flexBuilder.Finish();
  return CreatePut(fbb, offsets, fbb.CreateVector(flexBuilder.GetBuffer()));
}

std::optional<Offset<Put>> parseFlatbufferPut_ParseTable(
    FlatBufferBuilder& fbb, std::string_view* requestString,
    const reflection::Schema* schema, const reflection::Object* typeObject,
    Offset<Vector<uint16_t>>& offsets) {
  auto parser = Parser{};
  // Allow trailing content so a caller (e.g. a patch's update list) can keep
  // parsing after the payload.
  parser.opts.require_json_eof = false;
  if ((typeObject == nullptr) or not parser.Deserialize(schema)
      or not parser.SetRootType(typeObject->name()->c_str())
      or not parser.ParseJson(requestString->data())) {
    return std::nullopt;
  }
  // Advance past the parsed payload so a caller (e.g. a patch's update list)
  // can continue parsing after it.
  requestString->remove_prefix(parser.BytesConsumed());
  return CreatePut(fbb, offsets,
                   fbb.CreateVector(parser.builder_.GetBufferPointer(),
                                    parser.builder_.GetSize()));
}

// Whether a flexbuffer payload is the kind of value the field it is destined
// for can hold. Only the mismatches apply can neither detect nor represent are
// rejected: a string where a number belongs reads as 0, and a struct needs a
// map to name its members by.
bool payloadTypeAccepts(reflection::BaseType payloadType,
                        flexbuffers::Reference value) {
  switch (payloadType) {
    case reflection::String:
      return value.IsString();
    case reflection::Obj:
      // Reached only for a struct; a table is parsed against the schema.
      return value.IsMap();
    case reflection::Vector:
    case reflection::Array:
      return value.IsVector() or value.IsTypedVector();
    case reflection::None:
    case reflection::UType:
    case reflection::Union:
      return true;
    default:
      return not value.IsString() and not value.IsMap();
  }
}

std::optional<Offset<Put>> parseFlatbufferPut_ParsePrimitive(
    FlatBufferBuilder& fbb, std::string_view* requestString,
    Offset<Vector<uint16_t>>& offsets, reflection::BaseType payloadType) {
  auto parser = Parser{};
  if (not parser.ParseFlexBuffer(requestString->data(), nullptr,
                                 &parser.flex_builder_)) {
    return std::nullopt;
  }
  // A scalar field takes a number. An enum is a scalar, and its NAME is not
  // accepted: apply reads a string as a number, which answers 0 for every
  // name, so accepting one here would write a wrong value rather than refuse
  // a wrong request.
  if (not payloadTypeAccepts(payloadType,
                             flexbuffers::GetRoot(
                                 parser.flex_builder_.GetBuffer().data(),
                                 parser.flex_builder_.GetBuffer().size()))) {
    return std::nullopt;
  }
  // Advance past the parsed payload so a caller (e.g. a patch's update list)
  // can continue parsing after it.
  requestString->remove_prefix(parser.BytesConsumed());
  return CreatePut(fbb, offsets,
                   fbb.CreateVector(parser.flex_builder_.GetBuffer().data(),
                                    parser.flex_builder_.GetBuffer().size()));
}

std::optional<Offset<Put>> parseFlatbufferPut(
    FlatBufferBuilder& fbb, std::string_view* requestString,
    const reflection::Schema* schema,
    const reflection::Object* startObject = nullptr) {
  const reflection::Type* type = nullptr;
  const reflection::Object* typeObject = nullptr;
  bool elementInVector = false;
  auto offsets =
      parseFlatbufferOffsetPath(fbb, schema, requestString, &type, &typeObject,
                                &elementInVector, startObject, /*writing=*/true);
  if (not offsets) {
    return std::nullopt;
  }
  assert(not elementInVector or type->base_type() == reflection::Vector
         or type->base_type() == reflection::Array);

  // Whether the payload needs parsing AGAINST the schema, which is exactly
  // when the path resolved to an object: a table, a union member, or a vector
  // of either. Reading `type->index()` instead would say yes for an
  // enum-typed scalar as well, since its type names the enum -- and the path
  // parser leaves no object for one, so the payload was handed to the table
  // parser with nothing to parse against and the whole request was refused.
  // Everything else, scalars and enums and vectors of them alike, is a
  // flexbuffer payload.
  if (typeObject != nullptr) {
    // NOLINTNEXTLINE(bugprone-branch-clone)
    if (type->base_type() == reflection::Vector and not elementInVector) {
      return parseFlatbufferPut_ParseVector(fbb, requestString, schema,
                                            typeObject, *offsets);
    }
    if (not typeObject->is_struct()) {
      return parseFlatbufferPut_ParseTable(fbb, requestString, schema,
                                           typeObject, *offsets);
    }
    // A struct is carried as a flexbuffer map, not as a table: it has no
    // vtable to build, and the apply writes its members into a fixed-size
    // image by name. The table parser would need it as a root type, which a
    // struct cannot be.
  }
  // An element's payload is one of the element type; anything else is a
  // payload of the field's own type, including a whole vector or array.
  const auto payloadType = elementInVector ? type->element() : type->base_type();
  return parseFlatbufferPut_ParsePrimitive(fbb, requestString, *offsets,
                                           payloadType);
}

// Parses `<path> { <op> ; <op> ; ... }`, where each <op> is a put/patch/delete
// whose path is relative to the object <path> points at. An empty path ("{"
// immediately) patches the base object directly.
std::optional<Offset<Patch>> parseFlatbufferPatch(
    FlatBufferBuilder& fbb, std::string_view* requestString,
    const reflection::Schema* schema,
    const reflection::Object* startObject = nullptr) {
  const auto* baseObject =
      (startObject != nullptr) ? startObject : schema->root_table();

  advanceOver(*requestString, " ");

  std::optional<Offset<Vector<uint16_t>>> offsets;
  const reflection::Object* targetObject = baseObject;
  if (requestString->starts_with("{")) {
    offsets = fbb.CreateVector(std::vector<uint16_t>{});
  } else {
    const reflection::Type* type = nullptr;
    bool elementInVector = false;
    offsets =
        parseFlatbufferOffsetPath(fbb, schema, requestString, &type,
                                  &targetObject, &elementInVector, baseObject);
    if (not offsets or targetObject == nullptr) {
      return std::nullopt;  // a patch target must be a table
    }
  }

  advanceOver(*requestString, " ");
  if (not advanceOver(*requestString, "{")) {
    return std::nullopt;
  }

  auto types = std::vector<Operation>{};
  auto values = std::vector<Offset<void>>{};
  advanceOver(*requestString, " ");
  if (not requestString->starts_with("}")) {
    do {
      advanceOver(*requestString, " ");
      auto operation = parseFlatbufferOperationEnum(requestString);
      if (not operation) {
        return std::nullopt;
      }
      auto value = parseFlatbufferOperation(fbb, requestString, *operation,
                                            schema, targetObject);
      if (not value) {
        return std::nullopt;
      }
      types.push_back(*operation);
      values.push_back(*value);
      advanceOver(*requestString, " ");
    } while (advanceOver(*requestString, ";"));
    advanceOver(*requestString, " ");
  }
  if (not advanceOver(*requestString, "}")) {
    return std::nullopt;
  }

  return CreatePatch(fbb, *offsets, fbb.CreateVector(types),
                     fbb.CreateVector(values));
}

std::optional<Offset<Delete>> parseFlatbufferDelete(
    FlatBufferBuilder& fbb, std::string_view* requestString,
    const reflection::Schema* schema,
    const reflection::Object* startObject = nullptr) {
  const reflection::Type* type = nullptr;
  const reflection::Object* object = nullptr;
  bool elementInVector = false;
  auto offsets =
      parseFlatbufferOffsetPath(fbb, schema, requestString, &type, &object,
                                &elementInVector, startObject);
  if (not offsets) {
    return std::nullopt;
  }
  return CreateDelete(fbb, *offsets);
}

std::optional<Offset<void>> parseFlatbufferOperation(
    FlatBufferBuilder& fbb, std::string_view* requestString,
    Operation operation, const reflection::Schema* schema,
    const reflection::Object* startObject) {
  switch (operation) {
    case Operation::Put: {
      auto result = parseFlatbufferPut(fbb, requestString, schema, startObject);
      if (not result) {
        return std::nullopt;
      }
      return result->Union();
    }
    case Operation::Patch: {
      auto result =
          parseFlatbufferPatch(fbb, requestString, schema, startObject);
      if (not result) {
        return std::nullopt;
      }
      return result->Union();
    }
    case Operation::Delete: {
      auto result =
          parseFlatbufferDelete(fbb, requestString, schema, startObject);
      if (not result) {
        return std::nullopt;
      }
      return result->Union();
    }
    default: {
      return std::nullopt;
    }
  }
}

std::optional<Operation> parseFlatbufferOperationEnum(
    std::string_view* requestString) {
  if (advanceOver(*requestString, "put")) {  // NOLINT(bugprone-branch-clone)
    return Operation::Put;
  }
  if (advanceOver(*requestString, "patch")) {
    return Operation::Patch;
  } else if (advanceOver(*requestString, "delete")) {
    return Operation::Delete;
  } else {
    return std::nullopt;
  }
}

}  // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage)
std::optional<fbrequest::ByteBuffer> fbrequest::parseFlatbufferRequest(
    const reflection::Schema* schema, std::string_view requestString) {
  auto operation = parseFlatbufferOperationEnum(&requestString);
  if (not operation) {
    return std::nullopt;
  }

  auto fbb = FlatBufferBuilder{};
  auto operationOffset =
      parseFlatbufferOperation(fbb, &requestString, *operation, schema);
  if (not operationOffset) {
    return std::nullopt;
  }
  auto requestOffset = CreateRequest(fbb, *operation, *operationOffset);

  FinishRequestBuffer(fbb, requestOffset);
  auto* buffer = fbb.GetBufferPointer();
  auto size = fbb.GetSize();

  return ByteBuffer{buffer, buffer + size};
}
