#include <algorithm>
#include <optional>
#include <span>
#include <string>

#include "ElementIndex.hpp"
#include "PayloadValue.hpp"
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

// Resolves the `.Member` after a union field or a union-vector element,
// pushing the member's value. A path that names no member resolves to the
// union whole, which only a delete takes: it clears the union, or removes the
// element. Nothing says which member a written payload is.
bool parseFlatbufferOffsetPath_ParseUnionName(
    const reflection::Schema* schema, const reflection::Field* field,
    std::string_view* path, std::vector<uint32_t>* outOffsets,
    const reflection::Object** outObject, bool writing) {
  if (not advanceOver(*path, ".")) {
    *outObject = nullptr;
    return not writing;
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
  // NONE is a member of every union and holds no table: there is no object
  // for the path to continue into or for a payload to be parsed against. A
  // struct member is refused too: the apply reads every member as a table.
  const auto* memberType = unionValue->union_type();
  if (memberType == nullptr or memberType->base_type() != reflection::Obj) {
    return false;
  }
  const auto* memberDef = schema->objects()->Get(memberType->index());
  if (memberDef->is_struct()) {
    return false;
  }
  outOffsets->push_back(static_cast<uint32_t>(unionValue->value()));
  *outObject = memberDef;
  return true;
}

// Whether `field` is a union's discriminator, or a vector of unions' vector of
// them. The apply writes either only from the union beside it, so a request
// naming one would be reported applied with nothing written. A union is
// switched by `put <union>.<member> ...` and cleared by `delete <union>`.
bool isUnionDiscriminator(const reflection::Field* field) {
  const auto* type = field->type();
  return type->base_type() == reflection::UType
         or (type->base_type() == reflection::Vector
             and type->element() == reflection::UType);
}

// Whether element `index` of the vector or array `type`, whose elements are
// of `elementDef` where they are tables or structs, is one a request can
// name: an array cannot grow, so it holds only the elements its length gives,
// and a `put` (`writing`) may not grow a vector past what a flatbuffer holds.
bool elementReachable(const reflection::Type* type,
                      const reflection::Object* elementDef, uint32_t index,
                      bool writing) {
  if (type->base_type() == reflection::Array) {
    return index < type->fixed_length();
  }
  if (not writing) {
    return true;
  }
  return (elementDef != nullptr and elementDef->is_struct())
             ? structVectorCanGrowTo(elementDef, index)
             : vectorCanGrowTo(type->element(), index);
}

// `writing` refuses the paths only a `put` cannot serve, for a put and for a
// patch, whose operations may be puts: a delete resolves the same text and
// removes a whole element, which is well defined where writing one is not.
// `outInsideStruct` is set where the path names a member of a struct, which
// a delete cannot remove.
std::optional<Offset<Vector<uint32_t>>> parseFlatbufferOffsetPath(
    FlatBufferBuilder& fbb, const reflection::Schema* schema,
    std::string_view* path, const reflection::Type** outType,
    const reflection::Object** outObject, bool* outElementInVector,
    const reflection::Object* startObject = nullptr, bool writing = false,
    bool* outInsideStruct = nullptr) {
  *outType = nullptr;
  // Paths inside a patch's nested operations are relative to the patched
  // subobject rather than the root table.
  *outObject = (startObject != nullptr) ? startObject : schema->root_table();
  *outElementInVector = false;

  advanceOver(*path, " ");
  if (path->empty()) {
    return std::nullopt;
  }

  auto offsets = std::vector<uint32_t>{};
  do {
    *outElementInVector = false;
    if (*outObject == nullptr) {
      return std::nullopt;
    }
    if ((*outObject)->is_struct() and outInsideStruct != nullptr) {
      *outInsideStruct = true;
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

    if (isUnionDiscriminator(*field)) {
      return std::nullopt;
    }
    offsets.push_back(field->offset());

    auto type = *outType = field->type();
    switch (type->base_type()) {
      case reflection::Array:
      case reflection::Vector: {
        if (advanceOver(*path, "[")) {
          // Getting a path to a specific element in the vector. A bracket
          // that does not hold a whole index is refused rather than read as
          // the vector itself.
          const auto arrayIndex = advanceOverIndex(*path);
          if (not arrayIndex or not advanceOver(*path, "]")) {
            return std::nullopt;
          }
          const auto elementBaseType = type->element();
          const auto* elementDef = (elementBaseType == reflection::Obj)
                                       ? schema->objects()->Get(type->index())
                                       : nullptr;
          const auto isArray = type->base_type() == reflection::Array;
          if (not elementReachable(type, elementDef, *arrayIndex, writing)) {
            // An array cannot grow, so the element is not there; a vector
            // that grew to reach it would not fit in a flatbuffer.
            return std::nullopt;
          }
          *outElementInVector = true;
          offsets.push_back(*arrayIndex);
          if (elementDef != nullptr) {
            if (writing and isArray and elementDef->is_struct()) {
              // The struct writer computes no stride for an element of an
              // array of structs.
              return std::nullopt;
            }
            *outObject = elementDef;
          } else if (elementBaseType == reflection::Union) {
            if (not parseFlatbufferOffsetPath_ParseUnionName(
                    schema, *field, path, &offsets, outObject, writing)) {
              return std::nullopt;
            }
          } else {
            *outObject = nullptr;
          }
        } else {
          // Getting a path to the vector itself. Only a vector of tables or
          // structs names an object. `index` is set for a vector of enums and
          // for a vector of unions too, where it names the ENUM, so reading it
          // as an object index answers an unrelated object or runs off the
          // end of the vector.
          const auto* elementDef = (type->element() == reflection::Obj)
                                       ? schema->objects()->Get(type->index())
                                       : nullptr;
          const auto arrayOfStructs =
              type->base_type() == reflection::Array and elementDef != nullptr;
          if (writing
              and (type->element() == reflection::Union or arrayOfStructs)) {
            // A vector of unions has no whole-vector payload: the types of
            // its elements live in a second vector that a JSON array cannot
            // carry. An array of structs is not written, as its elements
            // are not.
            return std::nullopt;
          }
          if (path->starts_with(".")) {
            // The element type is what a whole-vector payload is parsed as,
            // not an object the path steps into: the next step would be read
            // as an element index.
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
        if (not parseFlatbufferOffsetPath_ParseUnionName(
                schema, *field, path, &offsets, outObject, writing)) {
          return std::nullopt;
        }
        break;
      }
      default: {
        *outObject = nullptr;
      }
    }
  } while (not path->empty() and advanceOver(*path, "."));
  if (path->starts_with("[")) {
    // An index into something that is not a vector or an array, or a second
    // one into an element: left here, it would be parsed as the payload.
    return std::nullopt;
  }
  return fbb.CreateVector(offsets);
}

std::optional<Offset<Put>> parseFlatbufferPut_ParseVector(
    FlatBufferBuilder& fbb, std::string_view* requestString,
    const reflection::Schema* schema, const reflection::Object* typeObject,
    Offset<Vector<uint32_t>>& offsets) {
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
    Offset<Vector<uint32_t>>& offsets) {
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

std::optional<Offset<Put>> parseFlatbufferPut_ParsePrimitive(
    FlatBufferBuilder& fbb, std::string_view* requestString,
    const reflection::Schema* schema, Offset<Vector<uint32_t>>& offsets,
    const reflection::Type* type, bool elementInVector) {
  auto flexBuilder = flexbuffers::Builder{};
  // Advances past the payload so a caller (e.g. a patch's update list) can
  // continue parsing after it.
  if (not parseFlexValue(*requestString, flexBuilder)) {
    return std::nullopt;
  }
  flexBuilder.Finish();
  const auto& payload = flexBuilder.GetBuffer();
  // The payload has to be a value of the kind its target holds, in its
  // range, and a struct or an array in one has to take it whole. An enum is
  // a scalar, and its NAME is not accepted: apply reads a string as a
  // number, which answers 0 for every name.
  if (not payloadFits(schema, type, elementInVector,
                      flexbuffers::GetRoot(payload))) {
    return std::nullopt;
  }
  return CreatePut(fbb, offsets, fbb.CreateVector(payload));
}

std::optional<Offset<Put>> parseFlatbufferPut(
    FlatBufferBuilder& fbb, std::string_view* requestString,
    const reflection::Schema* schema,
    const reflection::Object* startObject = nullptr) {
  const reflection::Type* type = nullptr;
  const reflection::Object* typeObject = nullptr;
  bool elementInVector = false;
  auto offsets = parseFlatbufferOffsetPath(fbb, schema, requestString, &type,
                                           &typeObject, &elementInVector,
                                           startObject, /*writing=*/true);
  if (not offsets) {
    return std::nullopt;
  }
  assert(not elementInVector or type->base_type() == reflection::Vector
         or type->base_type() == reflection::Array);

  // The payload is parsed AGAINST the schema exactly when the path resolved
  // to a table: a table, a union member, or a vector of either. Reading
  // `type->index()` instead would say yes for an enum-typed scalar as well,
  // since its type names the enum, and the path parser leaves no object for
  // one to be parsed against. Everything else -- scalars, enums, strings,
  // structs, and vectors of any of them -- is a flexbuffer payload. A struct
  // is carried as a flexbuffer map: it has no vtable to build, the apply
  // writes its members into a fixed-size image by name, and the table parser
  // would need it as a root type, which a struct cannot be.
  if (typeObject != nullptr and not typeObject->is_struct()) {
    // NOLINTNEXTLINE(bugprone-branch-clone)
    if (type->base_type() == reflection::Vector and not elementInVector) {
      return parseFlatbufferPut_ParseVector(fbb, requestString, schema,
                                            typeObject, *offsets);
    }
    return parseFlatbufferPut_ParseTable(fbb, requestString, schema, typeObject,
                                         *offsets);
  }
  // An element's payload is one of the element type; anything else is a
  // payload of the field's own type, including a whole vector or array.
  return parseFlatbufferPut_ParsePrimitive(fbb, requestString, schema, *offsets,
                                           type, elementInVector);
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

  std::optional<Offset<Vector<uint32_t>>> offsets;
  const reflection::Object* targetObject = baseObject;
  if (requestString->starts_with("{")) {
    offsets = fbb.CreateVector(std::vector<uint32_t>{});
  } else {
    const reflection::Type* type = nullptr;
    bool elementInVector = false;
    offsets =
        parseFlatbufferOffsetPath(fbb, schema, requestString, &type,
                                  &targetObject, &elementInVector, baseObject,
                                  /*writing=*/true);
    // A patch target must be a table, or a struct: a whole vector names its
    // element type, but the patch's operations would step into the vector
    // with their own first step read as an element index.
    const auto wholeVector = type != nullptr
                             and type->base_type() == reflection::Vector
                             and not elementInVector;
    if (not offsets or targetObject == nullptr or wholeVector) {
      return std::nullopt;
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
  bool insideStruct = false;
  auto offsets = parseFlatbufferOffsetPath(
      fbb, schema, requestString, &type, &object, &elementInVector, startObject,
      /*writing=*/false, &insideStruct);
  // A struct's members are all always stored, so there is no member, nor
  // element of an array in one, to remove.
  if (not offsets or insideStruct) {
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
  // The flatbuffers JSON parser takes a C string and reads to its
  // terminator, which a view does not promise; this copy supplies one.
  const auto terminated = std::string{requestString};
  requestString = terminated;
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
  // Text after the operation would name an edit nobody makes.
  requestString.remove_prefix(std::min(
      requestString.find_first_not_of(" \t\r\n"), requestString.size()));
  if (not requestString.empty()) {
    return std::nullopt;
  }
  auto requestOffset = CreateRequest(fbb, *operation, *operationOffset);

  FinishRequestBuffer(fbb, requestOffset);
  auto* buffer = fbb.GetBufferPointer();
  auto size = fbb.GetSize();

  return ByteBuffer{buffer, buffer + size};
}
