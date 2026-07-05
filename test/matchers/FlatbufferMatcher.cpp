// Copyright 2022 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "testing/FlatbufferMatcher.hpp"

#include <string>
#include <vector>

#include "flatbuffers/flatbuffers.h"
#include "flatbuffers/idl.h"
#include "flatbuffers/reflection.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "testing/FlexbufferMatcher.hpp"
#include "testing/MatcherInternal.hpp"

using ::testing::MatchResultListener;
using ::testing::PrintToString;

namespace flatbuffers {

using CompareFieldFunc = bool (*)(const reflection::Schema& schema,
                                  const reflection::Object& object_def,
                                  const reflection::Field& field_def,
                                  const Table* expected_table,
                                  const Table* arg_table,
                                  const std::string& path,
                                  MatchResultListener* listener);

namespace {

const reflection::Field& GetTypeField(const reflection::Object& object_def,
                                      const reflection::Field& field_def) {
  return *object_def.fields()->LookupByKey(
      (field_def.name()->str() + UnionTypeFieldSuffix()).c_str());
}

const reflection::Object& GetUnionVectorType(
    const reflection::Schema& schema, const reflection::Object& object_def,
    const reflection::Field& field_def, const Table* table, uint8_t index) {
  const auto& enum_def = *schema.enums()->Get(field_def.type()->index());
  const auto& types_field_def = GetTypeField(object_def, field_def);
  auto* union_types = GetFieldV<uint8_t>(*table, types_field_def);
  const auto& enum_value =
      *enum_def.values()->LookupByKey(union_types->Get(index));
  return *schema.objects()->Get(enum_value.union_type()->index());
}

}  // namespace

std::ostream& operator<<(std::ostream& out, const TypedBuffer& typed_buffer) {
  // First ensure there is a buffer at all.
  if (typed_buffer.buffer().empty()) {
    out << "empty flatbuffer";
    return out;
  }

  if (typed_buffer.schema() == nullptr) {
    out << "no schema provided";
    return out;
  }

  // Then make sure the buffer is valid.
  if (!Verify(*typed_buffer.schema(), *typed_buffer.schema()->root_table(),
              typed_buffer.buffer().data(), typed_buffer.buffer().size())) {
    out << "invalid flatbuffer";
    return out;
  }

  Parser parser;
  parser.Deserialize(typed_buffer.schema());
  parser.opts.strict_json = true;
  std::string json;
  const char* error = GenText(parser, typed_buffer.buffer().data(), &json);
  if (error != nullptr) {
    out << "(Error generating JSON: " << error << ")";
    return out;
  }
  out << json;
  return out;
}

namespace {

std::string PrintTableToString(const reflection::Schema& schema,
                               const reflection::Object& object_def,
                               const Table* table) {
  Parser parser;
  parser.Deserialize(&schema);
  parser.opts.strict_json = true;
  std::string json;
  const char* error =
      GenTextFromTable(parser, table, object_def.name()->str(), &json);
  if (error != nullptr) {
    json = std::string("(Error generating JSON: ") + error + ")";
  }
  return json;
}

template <typename T>
std::string PrintScalarVectorToString(const Vector<T>* scalar_vector) {
  std::string result = "[";
  for (uoffset_t i = 0; i < scalar_vector->size(); i++) {
    if (i > 0) {
      result += ", ";
    }
    result += PrintToString(scalar_vector->Get(i));
  }
  result += "]";
  return result;
}

std::string PrintStringVectorToString(
    const Vector<Offset<String>>* string_vector) {
  std::string result = "[";
  for (uoffset_t i = 0; i < string_vector->size(); i++) {
    if (i > 0) {
      result += ", ";
    }
    result += PrintToString(string_vector->Get(i)->str());
  }
  result += "]";
  return result;
}

std::string PrintObjectVectorToString(
    const reflection::Schema& schema, const reflection::Object& object_def,
    const Vector<Offset<Table>>* object_vector) {
  std::string result = "[";
  for (uoffset_t i = 0; i < object_vector->size(); i++) {
    if (i > 0) {
      result += ", ";
    }
    result += PrintTableToString(schema, object_def, object_vector->Get(i));
  }
  result += "]";
  return result;
}

std::string PrintUnionVectorToString(
    const reflection::Schema& schema, const reflection::Enum& enum_def,
    const Vector<uint8_t>* union_types,
    const Vector<Offset<Table>>* union_vector) {
  std::string result = "[";
  for (uoffset_t i = 0; i < union_vector->size(); i++) {
    const auto* enum_value =
        enum_def.values()->LookupByKey(union_types->Get(i));
    const auto& union_object_def =
        *schema.objects()->Get(enum_value->union_type()->index());
    if (i > 0) {
      result += ", ";
    }
    result +=
        PrintTableToString(schema, union_object_def, union_vector->Get(i));
  }
  result += "]";
  return result;
}

template <typename T, typename Func>
bool CompareNullness(bool expected_present, bool arg_present,
                     const T& expected_value, const T& arg_value,
                     const std::string& path, MatchResultListener* listener,
                     Func lazily_print_to_string) {
  RETURN_IF_NOT_TRUE_MSG(!expected_present || arg_present,
                         lazily_print_to_string(expected_value),
                         PrintToString(nullptr), path, listener);
  RETURN_IF_NOT_TRUE_MSG(expected_present || !arg_present,
                         PrintToString(nullptr),
                         lazily_print_to_string(arg_value), path, listener);
  return true;
}

template <typename T>
bool CompareVectorSizes(const Vector<T>* expected_vec, const Vector<T>* arg_vec,
                        const std::string& path,
                        MatchResultListener* listener) {
  RETURN_IF_NOT_TRUE_MSG(
      expected_vec->size() == arg_vec->size(),
      "vector of size " + PrintToString(expected_vec->size()),
      "vector of size " + PrintToString(arg_vec->size()), path, listener);
  return true;
}

template <typename T>
bool CompareScalarFieldsI(const reflection::Schema& /*schema*/,
                          const reflection::Object& /*object_def*/,
                          const reflection::Field& field_def,
                          const Table* expected_table, const Table* arg_table,
                          const std::string& path,
                          MatchResultListener* listener) {
  auto expected_value = GetFieldI<T>(*expected_table, field_def);
  auto arg_value = GetFieldI<T>(*arg_table, field_def);
  RETURN_IF_NOT_TRUE_MSG(expected_value == arg_value,
                         PrintToString(expected_value),
                         PrintToString(arg_value), path, listener);
  return true;
}

template <typename T>
bool CompareScalarFieldsF(const reflection::Schema& /*schema*/,
                          const reflection::Object& /*object_def*/,
                          const reflection::Field& field_def,
                          const Table* expected_table, const Table* arg_table,
                          const std::string& path,
                          MatchResultListener* listener) {
  auto expected_value = GetFieldF<T>(*expected_table, field_def);
  auto arg_value = GetFieldF<T>(*arg_table, field_def);
  RETURN_IF_NOT_TRUE_MSG(expected_value == arg_value,
                         PrintToString(expected_value),
                         PrintToString(arg_value), path, listener);
  return true;
}

bool CompareStrings(const reflection::Schema& /*schema*/,
                    const reflection::Object& /*object_def*/,
                    const reflection::Field& field_def,
                    const Table* expected_table, const Table* arg_table,
                    const std::string& path, MatchResultListener* listener) {
  const String* expected_string = GetFieldS(*expected_table, field_def);
  const String* arg_string = GetFieldS(*arg_table, field_def);
  RETURN_IF_NOT_TRUE(CompareNullness(
      expected_string, arg_string, expected_string, arg_string, path, listener,
      [](const String* string) { return PrintToString(string->str()); }));
  if ((expected_string != nullptr) && (arg_string != nullptr)) {
    RETURN_IF_NOT_TRUE_MSG(
        expected_string->string_view() == arg_string->string_view(),
        PrintToString(expected_string->str()), PrintToString(arg_string->str()),
        path, listener);
  }
  return true;
}

bool CompareObjects(const reflection::Schema& schema,
                    const reflection::Object& /*object_def*/,
                    const reflection::Field& field_def,
                    const Table* expected_table, const Table* arg_table,
                    const std::string& path, MatchResultListener* listener) {
  const auto& subobject_def = *schema.objects()->Get(field_def.type()->index());

  auto* expected_subtable = GetFieldT(*expected_table, field_def);
  auto* arg_subtable = GetFieldT(*arg_table, field_def);
  RETURN_IF_NOT_TRUE(CompareNullness(
      expected_subtable, arg_subtable, expected_subtable, arg_subtable, path,
      listener, [&schema, &subobject_def](const Table* subtable) {
        return PrintTableToString(schema, subobject_def, subtable);
      }));
  if ((expected_subtable != nullptr) && (arg_subtable != nullptr)) {
    RETURN_IF_NOT_TRUE(CompareTables(schema, subobject_def, expected_subtable,
                                     arg_subtable, path, listener));
  }
  return true;
}

bool CompareUnionTypes(const reflection::Object& expected_subobject_def,
                       const reflection::Object& arg_subobject_def,
                       const std::string& path,
                       testing::MatchResultListener* listener) {
  RETURN_IF_NOT_TRUE_MSG(expected_subobject_def.name()->string_view()
                             == arg_subobject_def.name()->string_view(),
                         PrintToString(expected_subobject_def.name()->str()),
                         PrintToString(arg_subobject_def.name()->str()), path,
                         listener);
  return true;
}

bool CompareUnions(const reflection::Schema& schema,
                   const reflection::Object& object_def,
                   const reflection::Field& field_def,
                   const Table* expected_table, const Table* arg_table,
                   const std::string& path, MatchResultListener* listener) {
  auto* expected_subtable = GetFieldT(*expected_table, field_def);
  auto* arg_subtable = GetFieldT(*arg_table, field_def);
  RETURN_IF_NOT_TRUE(CompareNullness(
      expected_subtable, arg_subtable, expected_table, arg_table, path,
      listener, [&schema, &object_def, &field_def](const Table* table) {
        auto& subobject_def =
            GetUnionType(schema, object_def, field_def, *table);
        return PrintTableToString(schema, subobject_def,
                                  GetFieldT(*table, field_def));
      }));
  if ((expected_subtable != nullptr) && (arg_subtable != nullptr)) {
    const auto& expected_subobject_def =
        GetUnionType(schema, object_def, field_def, *expected_table);
    const auto& arg_subobject_def =
        GetUnionType(schema, object_def, field_def, *arg_table);
    RETURN_IF_NOT_TRUE(
        CompareUnionTypes(expected_subobject_def, arg_subobject_def,
                          path + UnionTypeFieldSuffix(), listener));
    RETURN_IF_NOT_TRUE(CompareTables(schema, expected_subobject_def,
                                     expected_subtable, arg_subtable, path,
                                     listener));
  }
  return true;
}

template <typename T>
bool CompareScalarVectors(const reflection::Schema& /*schema*/,
                          const reflection::Object& /*object_def*/,
                          const reflection::Field& field_def,
                          const Table* expected_table, const Table* arg_table,
                          const std::string& path,
                          MatchResultListener* listener) {
  auto expected_vec = GetFieldV<T>(*expected_table, field_def);
  auto arg_vec = GetFieldV<T>(*arg_table, field_def);
  RETURN_IF_NOT_TRUE(CompareNullness(
      expected_vec, arg_vec, expected_vec, arg_vec, path, listener,
      [](const Vector<T>* vec) { return PrintScalarVectorToString(vec); }));
  if (expected_vec && arg_vec) {
    RETURN_IF_NOT_TRUE(
        CompareVectorSizes(expected_vec, arg_vec, path, listener));
    for (uoffset_t i = 0; i < expected_vec->size(); i++) {
      RETURN_IF_NOT_TRUE_MSG(expected_vec->Get(i) == arg_vec->Get(i),
                             PrintToString(expected_vec->Get(i)),
                             PrintToString(arg_vec->Get(i)),
                             path + "[" + PrintToString(i) + "]", listener);
    }
  }
  return true;
}

bool CompareStringVectors(const reflection::Schema& /*schema*/,
                          const reflection::Object& /*object_def*/,
                          const reflection::Field& field_def,
                          const Table* expected_table, const Table* arg_table,
                          const std::string& path,
                          MatchResultListener* listener) {
  auto* expected_vec = GetFieldV<Offset<String>>(*expected_table, field_def);
  auto* arg_vec = GetFieldV<Offset<String>>(*arg_table, field_def);
  RETURN_IF_NOT_TRUE(CompareNullness(expected_vec, arg_vec, expected_vec,
                                     arg_vec, path, listener,
                                     [](const Vector<Offset<String>>* vec) {
                                       return PrintStringVectorToString(vec);
                                     }));
  if ((expected_vec != nullptr) && (arg_vec != nullptr)) {
    RETURN_IF_NOT_TRUE(
        CompareVectorSizes(expected_vec, arg_vec, path, listener));
    for (uoffset_t i = 0; i < expected_vec->size(); i++) {
      RETURN_IF_NOT_TRUE_MSG(
          expected_vec->Get(i)->string_view() == arg_vec->Get(i)->string_view(),
          PrintToString(expected_vec->Get(i)->str()),
          PrintToString(arg_vec->Get(i)->str()),
          path + "[" + PrintToString(i) + "]", listener);
    }
  }
  return true;
}

bool CompareObjectVectors(const reflection::Schema& schema,
                          const reflection::Object& /*object_def*/,
                          const reflection::Field& field_def,
                          const Table* expected_table, const Table* arg_table,
                          const std::string& path,
                          MatchResultListener* listener) {
  auto* expected_vec = GetFieldV<Offset<Table>>(*expected_table, field_def);
  auto* arg_vec = GetFieldV<Offset<Table>>(*arg_table, field_def);
  const auto& subobject_def = *schema.objects()->Get(field_def.type()->index());
  RETURN_IF_NOT_TRUE(CompareNullness(
      expected_vec, arg_vec, expected_vec, arg_vec, path, listener,
      [&schema, &subobject_def](const Vector<Offset<Table>>* vec) {
        return PrintObjectVectorToString(schema, subobject_def, vec);
      }));
  if ((expected_vec != nullptr) && (arg_vec != nullptr)) {
    RETURN_IF_NOT_TRUE(
        CompareVectorSizes(expected_vec, arg_vec, path, listener));
    for (uoffset_t i = 0; i < expected_vec->size(); i++) {
      RETURN_IF_NOT_TRUE(CompareTables(
          schema, subobject_def, expected_vec->Get(i), arg_vec->Get(i),
          path + "[" + PrintToString(i) + "]", listener));
    }
  }
  return true;
}

bool CompareUnionVectors(const reflection::Schema& schema,
                         const reflection::Object& object_def,
                         const reflection::Field& field_def,
                         const Table* expected_table, const Table* arg_table,
                         const std::string& path,
                         MatchResultListener* listener) {
  auto* expected_vec = GetFieldV<Offset<Table>>(*expected_table, field_def);
  auto* arg_vec = GetFieldV<Offset<Table>>(*arg_table, field_def);
  RETURN_IF_NOT_TRUE(CompareNullness(
      expected_vec, arg_vec, expected_table, arg_table, path, listener,
      [&schema, &object_def, &field_def](const Table* table) {
        auto& enum_def = *schema.enums()->Get(field_def.type()->index());
        auto& types_field_def = GetTypeField(object_def, field_def);
        auto* union_types = GetFieldV<uint8_t>(*table, types_field_def);
        auto* union_vector = GetFieldV<Offset<Table>>(*table, field_def);
        return PrintUnionVectorToString(schema, enum_def, union_types,
                                        union_vector);
      }));
  if ((expected_vec != nullptr) && (arg_vec != nullptr)) {
    RETURN_IF_NOT_TRUE(
        CompareVectorSizes(expected_vec, arg_vec, path, listener));

    for (uoffset_t i = 0; i < expected_vec->size(); i++) {
      const auto& expected_subobject_def =
          GetUnionVectorType(schema, object_def, field_def, expected_table, i);
      const auto& arg_subobject_def =
          GetUnionVectorType(schema, object_def, field_def, arg_table, i);
      RETURN_IF_NOT_TRUE(CompareUnionTypes(
          expected_subobject_def, arg_subobject_def,
          path + UnionTypeFieldSuffix() + "[" + PrintToString(i) + "]",
          listener));
      RETURN_IF_NOT_TRUE(CompareTables(
          schema, expected_subobject_def, expected_vec->Get(i), arg_vec->Get(i),
          path + "[" + PrintToString(i) + "]", listener));
    }
  }

  return true;
}

bool CompareNoOp(const reflection::Schema& /*schema*/,
                 const reflection::Object& /*object_def*/,
                 const reflection::Field& /*field_def*/,
                 const Table* /*expected_table*/, const Table* /*arg_table*/,
                 const std::string& /*path*/,
                 MatchResultListener* /*listener*/) {
  return true;
}

bool CompareVectors(const reflection::Schema& schema,
                    const reflection::Object& object_def,
                    const reflection::Field& field_def,
                    const Table* expected_table, const Table* arg_table,
                    const std::string& path, MatchResultListener* listener) {
  static const CompareFieldFunc kCompareFuncs[reflection::MaxBaseType] = {
      /* None   */ nullptr,
      /* UType  */ CompareNoOp,
      /* Bool   */ CompareScalarVectors<bool>,
      /* Byte   */ CompareScalarVectors<int8_t>,
      /* UByte  */ CompareScalarVectors<uint8_t>,
      /* Short  */ CompareScalarVectors<int16_t>,
      /* UShort */ CompareScalarVectors<uint16_t>,
      /* Int    */ CompareScalarVectors<int32_t>,
      /* UInt   */ CompareScalarVectors<uint32_t>,
      /* Long   */ CompareScalarVectors<int64_t>,
      /* ULong  */ CompareScalarVectors<uint64_t>,
      /* Float  */ CompareScalarVectors<float>,
      /* Double */ CompareScalarVectors<double>,
      /* String */ CompareStringVectors,
      /* Vector */ nullptr,  // Nested Vectors are not supported.
      /* Obj    */ CompareObjectVectors,
      /* Union  */ CompareUnionVectors,
      /* Array  */ nullptr,
  };
  const auto& compare_func = kCompareFuncs[field_def.type()->element()];
  assert(compare_func != nullptr && "Unsupported vector element type");
  return compare_func(schema, object_def, field_def, expected_table, arg_table,
                      path, listener);
}

}  // namespace

// Dispatch table mapping FlatBuffer base types to their field comparison
// functions. Indexed by reflection::BaseType. Entries that are nullptr
// represent unsupported types that should not appear in well-formed schemas.
const CompareFieldFunc kFieldCompareFuncs[reflection::MaxBaseType] = {
    /* None   */ nullptr,
    /* UType  */ CompareNoOp,
    /* Bool   */ CompareScalarFieldsI<bool>,
    /* Byte   */ CompareScalarFieldsI<int8_t>,
    /* UByte  */ CompareScalarFieldsI<uint8_t>,
    /* Short  */ CompareScalarFieldsI<int16_t>,
    /* UShort */ CompareScalarFieldsI<uint16_t>,
    /* Int    */ CompareScalarFieldsI<int32_t>,
    /* UInt   */ CompareScalarFieldsI<uint32_t>,
    /* Long   */ CompareScalarFieldsI<int64_t>,
    /* ULong  */ CompareScalarFieldsI<uint64_t>,
    /* Float  */ CompareScalarFieldsF<float>,
    /* Double */ CompareScalarFieldsF<double>,
    /* String */ CompareStrings,
    /* Vector */ CompareVectors,
    /* Obj    */ CompareObjects,
    /* Union  */ CompareUnions,
    /* Array  */ nullptr,
};

bool CompareTables(const reflection::Schema& schema,
                   const reflection::Object& object_def,
                   const Table* expected_table, const Table* arg_table,
                   const std::string& path, MatchResultListener* listener) {
  const auto* field_defs = object_def.fields();
  for (const auto* it : *field_defs) {
    const auto& field_def = *it;
    const auto& compare_func =
        kFieldCompareFuncs[field_def.type()->base_type()];
    assert(compare_func != nullptr && "Unsupported field base type");
    RETURN_IF_NOT_TRUE(
        compare_func(schema, object_def, field_def, expected_table, arg_table,
                     path + "." + field_def.name()->c_str(), listener));
  }
  return true;
}

}  // namespace flatbuffers
