#include <iostream>
#include <ranges>
#include <span>

#include "fbrequest/FlatbufferRequest.hpp"
#include "fbrequest/FlatbufferRequest_generated.h"
#include "flatbuffers/flatbuffers.h"
#include "flatbuffers/idl.h"
#include "flatbuffers/reflection.h"
#include "gtest/gtest.h"
#include "test_schema_generated.h"
#include "testing/FlatbufferMatcher.hpp"

using namespace ::matcher_test;
using namespace fbrequest;

struct TestArgs {
  TestArgs(std::string _initial, std::string _request, std::string _final,
           bool /*_debug*/ = false)
      : initial(std::move(_initial)),
        request(std::move(_request)),
        final(std::move(_final)) {}

  std::string initial;
  std::string request;
  std::string final;
};

namespace {
std::ostream& operator<<(std::ostream& out, const TestArgs& args) {
  out << "TestArgs(R\"(" << args.initial << "\"), R\"(" << args.request
      << "\"), R\"(" << args.final << "\"))";
  return out;
}
}  // namespace

class RequestTestBase : public ::testing::TestWithParam<TestArgs> {
 protected:
  RequestTestBase()
      : schema_((flatbuffers::LoadFile("test_schema.bfbs", true, &bfbs_),
                 reflection::GetSchema(bfbs_.data()))),
        emptyBuffer_((parser_.Deserialize(schema_),
                      parser_.builder_.Finish(
                          matcher_test::CreateTestSchema(parser_.builder_)),
                      parser_.builder_.Release())),
        defaultTestSchema_(GetTestSchema(emptyBuffer_.data())) {}

  void SetUp() override { parser_.builder_.Reset(); }

  flatbuffers::DetachedBuffer applyRequestStr(const char* requestStr) {
    auto& builder = parser_.builder_;
    auto request = parseFlatbufferRequest(schema_, requestStr);
    EXPECT_TRUE(request.has_value())
        << "Failed to parse request: " << requestStr;

    builder.Finish(applyRequest(
        builder, schema_, flatbuffers::GetAnyRoot(emptyBuffer_.data()),
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        flatbuffers::GetRoot<serialized::Request>(request->data())));

    auto verifier =
        flatbuffers::Verifier(builder.GetBufferPointer(), builder.GetSize());
    assert(matcher_test::VerifyTestSchemaBuffer(verifier));

    return builder.Release();
  }

  std::vector<uint8_t> GetBuffer(const std::string& json) {
    std::vector<uint8_t> result;

    flatbuffers::Parser parser;
    if (!parser.Deserialize(schema_)) {
      std::cout << "Error Deserialize";
      return result;
    }

    if (!parser.Parse(json.c_str())) {
      std::cout << "Error parsing JSON " << parser.error_;
      return result;
    }

    auto buffer = parser.builder_.Release();
    result.resize(buffer.size());
    std::memcpy(result.data(), buffer.data(), buffer.size());

    return result;
  }

  flatbuffers::Parser parser_;
  std::string bfbs_;
  const reflection::Schema* schema_;

  flatbuffers::DetachedBuffer emptyBuffer_;
  const TestSchema* defaultTestSchema_{nullptr};
};

class UpdateTableTest : public RequestTestBase {};
class DeleteTableTest : public RequestTestBase {};
class PatchTableTest : public RequestTestBase {};

INSTANTIATE_TEST_SUITE_P(
    UpdateTableTest, UpdateTableTest,
    ::testing::Values(
        // clang-format off
        // Set scalars on empty table.
        TestArgs(R"({})", "put i8   99", R"({"i8":   99})"),
        TestArgs(R"({})", "put ui8  99", R"({"ui8":  99})"),
        TestArgs(R"({})", "put i16  99", R"({"i16":  99})"),
        TestArgs(R"({})", "put ui16 99", R"({"ui16": 99})"),
        TestArgs(R"({})", "put i32  99", R"({"i32":  99})"),
        TestArgs(R"({})", "put ui32 99", R"({"ui32": 99})"),
        TestArgs(R"({})", "put i64  99", R"({"i64":  99})"),
        TestArgs(R"({})", "put ui64 99", R"({"ui64": 99})"),
        TestArgs(R"({})", "put f32  99", R"({"f32":  99.0})"),
        TestArgs(R"({})", "put f64  99", R"({"f64":  99.0})"),

        // Set scalar on a table containing another value.
        TestArgs(R"({"i32":   10})", "put i8   99", R"({"i8":   99, "i32": 10})"),
        TestArgs(R"({"i32":   10})", "put ui8  99", R"({"ui8":  99, "i32": 10})"),
        TestArgs(R"({"i32":   10})", "put i16  99", R"({"i16":  99, "i32": 10})"),
        TestArgs(R"({"i32":   10})", "put ui16 99", R"({"ui16": 99, "i32": 10})"),
        TestArgs(R"({"i64":   10})", "put i32  99", R"({"i32":  99, "i64": 10})"),
        TestArgs(R"({"i32":   10})", "put ui32 99", R"({"ui32": 99, "i32": 10})"),
        TestArgs(R"({"i32":   10})", "put i64  99", R"({"i64":  99, "i32": 10})"),
        TestArgs(R"({"i32":   10})", "put ui64 99", R"({"ui64": 99, "i32": 10})"),
        TestArgs(R"({"i32":   10})", "put f32  99", R"({"f32":  99.0, "i32": 10})"),
        TestArgs(R"({"i32":   10})", "put f64  99", R"({"f64":  99.0, "i32": 10})"),
        
        // Set a scalar on a table where it is already set.
        TestArgs(R"({"i8":    10})", "put i8   99", R"({"i8":   99})"),
        TestArgs(R"({"ui8":   10})", "put ui8  99", R"({"ui8":  99})"),
        TestArgs(R"({"i16":   10})", "put i16  99", R"({"i16":  99})"),
        TestArgs(R"({"ui16":  10})", "put ui16 99", R"({"ui16": 99})"),
        TestArgs(R"({"i32":   10})", "put i32  99", R"({"i32":  99})"),
        TestArgs(R"({"ui32":  10})", "put ui32 99", R"({"ui32": 99})"),
        TestArgs(R"({"i64":   10})", "put i64  99", R"({"i64":  99})"),
        TestArgs(R"({"ui64":  10})", "put ui64 99", R"({"ui64": 99})"),
        TestArgs(R"({"f32": 10.0})", "put f32  99", R"({"f32":  99.0})"),
        TestArgs(R"({"f64": 10.0})", "put f64  99", R"({"f64":  99.0})"),

        // Set a scalar on a subobject.
        TestArgs(R"({})", "put object.i8   99", R"({"object": {"i8":   99}})"),
        TestArgs(R"({})", "put object.ui8  99", R"({"object": {"ui8":  99}})"),
        TestArgs(R"({})", "put object.i16  99", R"({"object": {"i16":  99}})"),
        TestArgs(R"({})", "put object.ui16 99", R"({"object": {"ui16": 99}})"),
        TestArgs(R"({})", "put object.i32  99", R"({"object": {"i32":  99}})"),
        TestArgs(R"({})", "put object.ui32 99", R"({"object": {"ui32": 99}})"),
        TestArgs(R"({})", "put object.i64  99", R"({"object": {"i64":  99}})"),
        TestArgs(R"({})", "put object.ui64 99", R"({"object": {"ui64": 99}})"),
        TestArgs(R"({})", "put object.f32  99", R"({"object": {"f32":  99}})"),
        TestArgs(R"({})", "put object.f64  99", R"({"object": {"f64":  99}})"),

        TestArgs(R"({})", "put object.object.i8   99", R"({"object": {"object": {"i8":   99}}})"),
        TestArgs(R"({})", "put object.object.ui8  99", R"({"object": {"object": {"ui8":  99}}})"),
        TestArgs(R"({})", "put object.object.i16  99", R"({"object": {"object": {"i16":  99}}})"),
        TestArgs(R"({})", "put object.object.ui16 99", R"({"object": {"object": {"ui16": 99}}})"),
        TestArgs(R"({})", "put object.object.i32  99", R"({"object": {"object": {"i32":  99}}})"),
        TestArgs(R"({})", "put object.object.ui32 99", R"({"object": {"object": {"ui32": 99}}})"),
        TestArgs(R"({})", "put object.object.i64  99", R"({"object": {"object": {"i64":  99}}})"),
        TestArgs(R"({})", "put object.object.ui64 99", R"({"object": {"object": {"ui64": 99}}})"),
        TestArgs(R"({})", "put object.object.f32  99", R"({"object": {"object": {"f32":  99}}})"),
        TestArgs(R"({})", "put object.object.f64  99", R"({"object": {"object": {"f64":  99}}})"),

        // Set a scalar on a subobject with other scalar value.
        TestArgs(R"({"object": {"i32": 10}})", "put object.i8   99", R"({"object":{"i8":   99,   "i32": 10}})"),
        TestArgs(R"({"object": {"i32": 10}})", "put object.ui8  99", R"({"object":{"ui8":  99,   "i32": 10}})"),
        TestArgs(R"({"object": {"i32": 10}})", "put object.i16  99", R"({"object":{"i16":  99,   "i32": 10}})"),
        TestArgs(R"({"object": {"i32": 10}})", "put object.ui16 99", R"({"object":{"ui16": 99,   "i32": 10}})"),
        TestArgs(R"({"object": {"i64": 10}})", "put object.i32  99", R"({"object":{"i32":  99,   "i64": 10}})"),
        TestArgs(R"({"object": {"i32": 10}})", "put object.ui32 99", R"({"object":{"ui32": 99,   "i32": 10}})"),
        TestArgs(R"({"object": {"i32": 10}})", "put object.i64  99", R"({"object":{"i64":  99,   "i32": 10}})"),
        TestArgs(R"({"object": {"i32": 10}})", "put object.ui64 99", R"({"object":{"ui64": 99,   "i32": 10}})"),
        TestArgs(R"({"object": {"i32": 10}})", "put object.f32  99", R"({"object":{"f32":  99.0, "i32": 10}})"),
        TestArgs(R"({"object": {"i32": 10}})", "put object.f64  99", R"({"object":{"f64":  99.0, "i32": 10}})"),

        // Set a scalar on a subobject union.
        TestArgs(R"({})", "put number.Integer.value 99",       R"({"number_type": "Integer", "number": {"value": 99}})"),
        TestArgs(R"({})", "put number.FloatingPoint.value 99", R"({"number_type": "FloatingPoint", "number": {"value": 99.0}})"),

        // A put into a STORED union member keeps the member's other fields.
        // The member's fields have to be copied out of the member, not out of
        // the table holding the union.
        TestArgs(R"({"number_type": "Fraction", "number": {"numerator": 3, "denominator": 4}})",
                 "put number.Fraction.numerator 9",
                 R"({"number_type": "Fraction", "number": {"numerator": 9, "denominator": 4}})"),
        TestArgs(R"({"number_type": "Fraction", "number": {"numerator": 3, "denominator": 4}})",
                 "put number.Fraction.denominator 9",
                 R"({"number_type": "Fraction", "number": {"numerator": 3, "denominator": 9}})"),

        // Switching member carries nothing across: the stored member is a
        // source only while it is the member being written.
        TestArgs(R"({"number_type": "Fraction", "number": {"numerator": 3, "denominator": 4}})",
                 "put number.Integer.value 7",
                 R"({"number_type": "Integer", "number": {"value": 7}})"),

        // Put non-scalar object on an empty table.
        TestArgs(R"({})", R"(put string "hello")",               R"({"string": "hello"})"),
        TestArgs(R"({})", R"(put object {"i32": 99})",           R"({"object": {"i32": 99}})"),
        TestArgs(R"({})", R"(put number.Integer {"value": 99})", R"({"number_type": "Integer", "number": {"value": 99}})"),

        // Put a non-scalar object on a root table containing that value.
        TestArgs(R"({"string": "goodbye"})",                               R"(put string "hello")",                       R"({"string": "hello"})"),
        TestArgs(R"({"object": {"i8": 99}})",                              R"(put object {"i32": 99})",                   R"({"object": {"i32": 99}})"),
        TestArgs(R"({"number_type": "Integer", "number": {"value": 10}})", R"(put number.Integer {"value": 99})",         R"({"number_type": "Integer", "number": {"value": 99}})"),
        TestArgs(R"({"number_type": "Integer", "number": {"value": 10}})", R"(put number.FloatingPoint {"value": 99.0})", R"({"number_type": "FloatingPoint", "number": {"value": 99.0}})"),

        // Put a non-scalar object on a root table containing other values.
        TestArgs(R"({"string": "goodbye", object: {"i8": 99}, "number_type": "Integer", "number": {"value": 10}})", R"(put string                     "hello")", R"({"string": "hello",   object: {"i8":  99}, "number_type": "Integer",       "number": {"value":   10}})"),
        TestArgs(R"({"string": "goodbye", object: {"i8": 99}, "number_type": "Integer", "number": {"value": 10}})", R"(put object                 {"i32": 99})", R"({"string": "goodbye", object: {"i32": 99}, "number_type": "Integer",       "number": {"value":   10}})"),
        TestArgs(R"({"string": "goodbye", object: {"i8": 99}, "number_type": "Integer", "number": {"value": 10}})", R"(put number.Integer       {"value": 99})", R"({"string": "goodbye", object: {"i8":  99}, "number_type": "Integer",       "number": {"value":   99}})"),
        TestArgs(R"({"string": "goodbye", object: {"i8": 99}, "number_type": "Integer", "number": {"value": 10}})", R"(put number.FloatingPoint {"value": 99})", R"({"string": "goodbye", object: {"i8":  99}, "number_type": "FloatingPoint", "number": {"value": 99.0}})"),

        // Vectors
        TestArgs(R"({                       })", R"(put i32_vector [99])",     R"({"i32_vector": [99]})"),
        TestArgs(R"({"i32_vector":        []})", R"(put i32_vector [99])",     R"({"i32_vector": [99]})"),
        TestArgs(R"({"i32_vector":      [10]})", R"(put i32_vector [99])",     R"({"i32_vector": [99]})"),
        TestArgs(R"({                       })", R"(put i32_vector [99, 99])", R"({"i32_vector": [99, 99]})"),
        TestArgs(R"({"i32_vector":        []})", R"(put i32_vector [99, 99])", R"({"i32_vector": [99, 99]})"),
        TestArgs(R"({"i32_vector":      [10]})", R"(put i32_vector [99, 99])", R"({"i32_vector": [99, 99]})"),
        TestArgs(R"({"i32_vector": [1,2,3,4]})", R"(put i32_vector [99, 99])", R"({"i32_vector": [99, 99]})"),

        TestArgs(R"({                                  })", R"(put string_vector ["z"])",      R"({"string_vector": ["z"]})"),
        TestArgs(R"({"string_vector":                []})", R"(put string_vector ["z"])",      R"({"string_vector": ["z"]})"),
        TestArgs(R"({"string_vector":             ["a"]})", R"(put string_vector ["z"])",      R"({"string_vector": ["z"]})"),
        TestArgs(R"({                                  })", R"(put string_vector ["z", "z"])", R"({"string_vector": ["z", "z"]})"),
        TestArgs(R"({"string_vector":                []})", R"(put string_vector ["z", "z"])", R"({"string_vector": ["z", "z"]})"),
        TestArgs(R"({"string_vector":             ["a"]})", R"(put string_vector ["z", "z"])", R"({"string_vector": ["z", "z"]})"),
        TestArgs(R"({"string_vector": ["a","b","c","d"]})", R"(put string_vector ["z", "z"])", R"({"string_vector": ["z", "z"]})"),

        TestArgs(R"({                                                      })", R"(put object_vector [{"i8":9}])",           R"({"object_vector": [{"i8":9}]})"),
        TestArgs(R"({"object_vector":                                    []})", R"(put object_vector [{"i8":9}])",           R"({"object_vector": [{"i8":9}]})"),
        TestArgs(R"({"object_vector":                                    []})", R"(put i32 99)",                             R"({"i32": 99, "object_vector": []})"),
        TestArgs(R"({"object_vector":                            [{"i8":1}]})", R"(put object_vector [{"i8":9}])",           R"({"object_vector": [{"i8":9}]})"),
        TestArgs(R"({                                                      })", R"(put object_vector [{"i8":9}, {"i8":9}])", R"({"object_vector": [{"i8":9}, {"i8":9}]})"),
        TestArgs(R"({"object_vector":                                    []})", R"(put object_vector [{"i8":9}, {"i8":9}])", R"({"object_vector": [{"i8":9}, {"i8":9}]})"),
        TestArgs(R"({"object_vector":                            [{"i8":1}]})", R"(put object_vector [{"i8":9}, {"i8":9}])", R"({"object_vector": [{"i8":9}, {"i8":9}]})"),
        TestArgs(R"({"object_vector": [{"i8":1},{"i8":1},{"i8":1},{"i8":1}]})", R"(put object_vector [{"i8":9}, {"i8":9}])", R"({"object_vector": [{"i8":9}, {"i8":9}]})"),

        TestArgs(R"({                  })", R"(put i32_vector[0] 99)", R"({"i32_vector": [99]})"),
        TestArgs(R"({"i32_vector":   []})", R"(put i32_vector[0] 99)", R"({"i32_vector": [99]})"),
        TestArgs(R"({"i32_vector": [10]})", R"(put i32_vector[0] 99)", R"({"i32_vector": [99]})"),
        TestArgs(R"({                  })", R"(put i32_vector[1] 99)", R"({"i32_vector": [0, 99]})"),
        TestArgs(R"({"i32_vector":   []})", R"(put i32_vector[1] 99)", R"({"i32_vector": [0, 99]})"),
        TestArgs(R"({"i32_vector": [10]})", R"(put i32_vector[1] 99)", R"({"i32_vector": [10, 99]})"),
        TestArgs(R"({"i32_vector": [10]})", R"(put i32_vector[2] 99)", R"({"i32_vector": [10, 0, 99]})"),
      
        TestArgs(R"({"i32_vector": [1,2,3,4]})", R"(put i32_vector[0] 9)", R"({"i32_vector": [9,2,3,4]})"),
        TestArgs(R"({"i32_vector": [1,2,3,4]})", R"(put i32_vector[1] 9)", R"({"i32_vector": [1,9,3,4]})"),
        TestArgs(R"({"i32_vector": [1,2,3,4]})", R"(put i32_vector[2] 9)", R"({"i32_vector": [1,2,9,4]})"),
        TestArgs(R"({"i32_vector": [1,2,3,4]})", R"(put i32_vector[3] 9)", R"({"i32_vector": [1,2,3,9]})"),
        TestArgs(R"({"i32_vector": [1,2,3,4]})", R"(put i32_vector[4] 9)", R"({"i32_vector": [1,2,3,4,9]})"),
        TestArgs(R"({"i32_vector": [1,2,3,4]})", R"(put i32_vector[5] 9)", R"({"i32_vector": [1,2,3,4,0,9]})"),
        TestArgs(R"({"i32_vector": [1,2,3,4]})", R"(put i32_vector[6] 9)", R"({"i32_vector": [1,2,3,4,0,0,9]})"),
      
        TestArgs(R"({"i8_vector": [1,2,3,4]})", R"(put i8_vector[0] 9)", R"({"i8_vector": [9,2,3,4]})"),
        TestArgs(R"({"i8_vector": [1,2,3,4]})", R"(put i8_vector[1] 9)", R"({"i8_vector": [1,9,3,4]})"),
        TestArgs(R"({"i8_vector": [1,2,3,4]})", R"(put i8_vector[2] 9)", R"({"i8_vector": [1,2,9,4]})"),
        TestArgs(R"({"i8_vector": [1,2,3,4]})", R"(put i8_vector[3] 9)", R"({"i8_vector": [1,2,3,9]})"),
        TestArgs(R"({"i8_vector": [1,2,3,4]})", R"(put i8_vector[4] 9)", R"({"i8_vector": [1,2,3,4,9]})"),
        TestArgs(R"({"i8_vector": [1,2,3,4]})", R"(put i8_vector[5] 9)", R"({"i8_vector": [1,2,3,4,0,9]})"),
        TestArgs(R"({"i8_vector": [1,2,3,4]})", R"(put i8_vector[6] 9)", R"({"i8_vector": [1,2,3,4,0,0,9]})"),

        TestArgs(R"({"string_vector": ["a","b","c","d"]})", R"(put string_vector[0] "z")", R"({"string_vector": ["z","b","c","d"]})"),
        TestArgs(R"({"string_vector": ["a","b","c","d"]})", R"(put string_vector[1] "z")", R"({"string_vector": ["a","z","c","d"]})"),
        TestArgs(R"({"string_vector": ["a","b","c","d"]})", R"(put string_vector[2] "z")", R"({"string_vector": ["a","b","z","d"]})"),
        TestArgs(R"({"string_vector": ["a","b","c","d"]})", R"(put string_vector[3] "z")", R"({"string_vector": ["a","b","c","z"]})"),
        TestArgs(R"({"string_vector": ["a","b","c","d"]})", R"(put string_vector[4] "z")", R"({"string_vector": ["a","b","c","d","z"]})"),
        TestArgs(R"({"string_vector": ["a","b","c","d"]})", R"(put string_vector[5] "z")", R"({"string_vector": ["a","b","c","d","","z"]})"),
        TestArgs(R"({"string_vector": ["a","b","c","d"]})", R"(put string_vector[6] "z")", R"({"string_vector": ["a","b","c","d","","","z"]})"),

        TestArgs(R"({"object_vector": [{"i8":1},{"i8":2},{"i8":3},{"i8":4}]})", R"(put object_vector[0] {"i8":9})", R"({"object_vector": [{"i8":9},{"i8":2},{"i8":3},{"i8":4}]})"),
        TestArgs(R"({"object_vector": [{"i8":1},{"i8":2},{"i8":3},{"i8":4}]})", R"(put object_vector[1] {"i8":9})", R"({"object_vector": [{"i8":1},{"i8":9},{"i8":3},{"i8":4}]})"),
        TestArgs(R"({"object_vector": [{"i8":1},{"i8":2},{"i8":3},{"i8":4}]})", R"(put object_vector[2] {"i8":9})", R"({"object_vector": [{"i8":1},{"i8":2},{"i8":9},{"i8":4}]})"),
        TestArgs(R"({"object_vector": [{"i8":1},{"i8":2},{"i8":3},{"i8":4}]})", R"(put object_vector[3] {"i8":9})", R"({"object_vector": [{"i8":1},{"i8":2},{"i8":3},{"i8":9}]})"),
        TestArgs(R"({"object_vector": [{"i8":1},{"i8":2},{"i8":3},{"i8":4}]})", R"(put object_vector[4] {"i8":9})", R"({"object_vector": [{"i8":1},{"i8":2},{"i8":3},{"i8":4},{"i8":9}]})"),
        TestArgs(R"({"object_vector": [{"i8":1},{"i8":2},{"i8":3},{"i8":4}]})", R"(put object_vector[5] {"i8":9})", R"({"object_vector": [{"i8":1},{"i8":2},{"i8":3},{"i8":4},{},{"i8":9}]})"),
        TestArgs(R"({"object_vector": [{"i8":1},{"i8":2},{"i8":3},{"i8":4}]})", R"(put object_vector[6] {"i8":9})", R"({"object_vector": [{"i8":1},{"i8":2},{"i8":3},{"i8":4},{},{},{"i8":9}]})"),

        TestArgs(R"({"number_vector_type": ["Integer", "Integer", "Integer", "Integer"], "number_vector": [{"value":1},{"value":2},{"value":3},{"value":4}]})", R"(put number_vector[0].Integer {"value":9})", R"({"number_vector_type": ["Integer", "Integer", "Integer", "Integer"],                                  "number_vector": [{"value":9},{"value":2},{"value":3},{"value":4}]})"),
        TestArgs(R"({"number_vector_type": ["Integer", "Integer", "Integer", "Integer"], "number_vector": [{"value":1},{"value":2},{"value":3},{"value":4}]})", R"(put number_vector[1].Integer {"value":9})", R"({"number_vector_type": ["Integer", "Integer", "Integer", "Integer"],                                  "number_vector": [{"value":1},{"value":9},{"value":3},{"value":4}]})"),
        TestArgs(R"({"number_vector_type": ["Integer", "Integer", "Integer", "Integer"], "number_vector": [{"value":1},{"value":2},{"value":3},{"value":4}]})", R"(put number_vector[2].Integer {"value":9})", R"({"number_vector_type": ["Integer", "Integer", "Integer", "Integer"],                                  "number_vector": [{"value":1},{"value":2},{"value":9},{"value":4}]})"),
        TestArgs(R"({"number_vector_type": ["Integer", "Integer", "Integer", "Integer"], "number_vector": [{"value":1},{"value":2},{"value":3},{"value":4}]})", R"(put number_vector[3].Integer {"value":9})", R"({"number_vector_type": ["Integer", "Integer", "Integer", "Integer"],                                  "number_vector": [{"value":1},{"value":2},{"value":3},{"value":9}]})"),
        TestArgs(R"({"number_vector_type": ["Integer", "Integer", "Integer", "Integer"], "number_vector": [{"value":1},{"value":2},{"value":3},{"value":4}]})", R"(put number_vector[4].Integer {"value":9})", R"({"number_vector_type": ["Integer", "Integer", "Integer", "Integer", "Integer"],                       "number_vector": [{"value":1},{"value":2},{"value":3},{"value":4},{"value":9}]})"),
        TestArgs(R"({"number_vector_type": ["Integer", "Integer", "Integer", "Integer"], "number_vector": [{"value":1},{"value":2},{"value":3},{"value":4}]})", R"(put number_vector[5].Integer {"value":9})", R"({"number_vector_type": ["Integer", "Integer", "Integer", "Integer", "Integer", "Integer"],            "number_vector": [{"value":1},{"value":2},{"value":3},{"value":4},{},{"value":9}]})"),
        TestArgs(R"({"number_vector_type": ["Integer", "Integer", "Integer", "Integer"], "number_vector": [{"value":1},{"value":2},{"value":3},{"value":4}]})", R"(put number_vector[6].Integer {"value":9})", R"({"number_vector_type": ["Integer", "Integer", "Integer", "Integer", "Integer", "Integer", "Integer"], "number_vector": [{"value":1},{"value":2},{"value":3},{"value":4},{},{},{"value":9}]})"),

        TestArgs(R"({"number_vector_type": ["Integer", "Integer", "Integer", "Integer"], "number_vector": [{"value":1},{"value":2},{"value":3},{"value":4}]})", R"(put number_vector[0].FloatingPoint {"value":9.0})", R"({"number_vector_type": ["FloatingPoint", "Integer", "Integer", "Integer"],                                  "number_vector": [{"value":9},{"value":2},{"value":3},{"value":4}]})"),
        TestArgs(R"({"number_vector_type": ["Integer", "Integer", "Integer", "Integer"], "number_vector": [{"value":1},{"value":2},{"value":3},{"value":4}]})", R"(put number_vector[1].FloatingPoint {"value":9.0})", R"({"number_vector_type": ["Integer", "FloatingPoint", "Integer", "Integer"],                                  "number_vector": [{"value":1},{"value":9},{"value":3},{"value":4}]})"),
        TestArgs(R"({"number_vector_type": ["Integer", "Integer", "Integer", "Integer"], "number_vector": [{"value":1},{"value":2},{"value":3},{"value":4}]})", R"(put number_vector[2].FloatingPoint {"value":9.0})", R"({"number_vector_type": ["Integer", "Integer", "FloatingPoint", "Integer"],                                  "number_vector": [{"value":1},{"value":2},{"value":9},{"value":4}]})"),
        TestArgs(R"({"number_vector_type": ["Integer", "Integer", "Integer", "Integer"], "number_vector": [{"value":1},{"value":2},{"value":3},{"value":4}]})", R"(put number_vector[3].FloatingPoint {"value":9.0})", R"({"number_vector_type": ["Integer", "Integer", "Integer", "FloatingPoint"],                                  "number_vector": [{"value":1},{"value":2},{"value":3},{"value":9}]})"),
        TestArgs(R"({"number_vector_type": ["Integer", "Integer", "Integer", "Integer"], "number_vector": [{"value":1},{"value":2},{"value":3},{"value":4}]})", R"(put number_vector[4].FloatingPoint {"value":9.0})", R"({"number_vector_type": ["Integer", "Integer", "Integer", "Integer", "FloatingPoint"],                       "number_vector": [{"value":1},{"value":2},{"value":3},{"value":4},{"value":9}]})"),
        TestArgs(R"({"number_vector_type": ["Integer", "Integer", "Integer", "Integer"], "number_vector": [{"value":1},{"value":2},{"value":3},{"value":4}]})", R"(put number_vector[5].FloatingPoint {"value":9.0})", R"({"number_vector_type": ["Integer", "Integer", "Integer", "Integer", "Integer", "FloatingPoint"],            "number_vector": [{"value":1},{"value":2},{"value":3},{"value":4},{},{"value":9}]})"),
        TestArgs(R"({"number_vector_type": ["Integer", "Integer", "Integer", "Integer"], "number_vector": [{"value":1},{"value":2},{"value":3},{"value":4}]})", R"(put number_vector[6].FloatingPoint {"value":9.0})", R"({"number_vector_type": ["Integer", "Integer", "Integer", "Integer", "Integer", "Integer", "FloatingPoint"], "number_vector": [{"value":1},{"value":2},{"value":3},{"value":4},{},{},{"value":9}]})")
        //  clang-format on
        ));

TEST_P(UpdateTableTest, PutScalarInRoot) {
  const auto& args = GetParam();
  flatbuffers::FlatBufferBuilder fbb;
  auto request = parseFlatbufferRequest(schema_, args.request);
  ASSERT_TRUE(request.has_value()) << true
                                   << args.initial << '\n'
                                   << args.request << '\n'
                                   << args.final;

  auto startingValue =
      flatbuffers::TypedBuffer(schema_, GetBuffer(args.initial));
  fbb.Finish(applyRequest(fbb, schema_,
                          flatbuffers::GetAnyRoot(startingValue.buffer().data()),
                          // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
                          flatbuffers::GetRoot<serialized::Request>(request->data())));
  auto updated = fbb.Release();
  auto expected = GetBuffer(args.final);

  EXPECT_FLATBUFFERS_EQ(schema_, updated, expected) << true
                                                    << args.initial << '\n'
                                                    << args.request << '\n'
                                                    << args.final;
}

INSTANTIATE_TEST_SUITE_P(
    DeleteTableTest, DeleteTableTest,
    ::testing::Values(
        // clang-format off
        // Delete a scalar, leaving the others.
        TestArgs(R"({"i8": 5, "i32": 10})", "delete i8",  R"({"i32": 10})"),
        TestArgs(R"({"i32": 10, "i8": 5})", "delete i32", R"({"i8": 5})"),
        TestArgs(R"({"f32": 1.5, "i32": 3})", "delete f32", R"({"i32": 3})"),
        // Delete the only field -> empty table.
        TestArgs(R"({"i8": 5})", "delete i8", R"({})"),
        // Delete a field that is not present -> no-op.
        TestArgs(R"({"i32": 10})", "delete i8", R"({"i32": 10})"),
        // Delete a string.
        TestArgs(R"({"string": "hi", "i32": 1})", "delete string", R"({"i32": 1})"),
        // Delete a scalar inside a subobject.
        TestArgs(R"({"object": {"i8": 5, "i32": 10}})", "delete object.i8", R"({"object": {"i32": 10}})"),
        TestArgs(R"({"object": {"i8": 5}})",           "delete object.i8", R"({"object": {}})"),
        TestArgs(R"({"object": {"object": {"i8": 5, "i32": 3}}})", "delete object.object.i8", R"({"object": {"object": {"i32": 3}}})"),
        // Delete a whole subobject.
        TestArgs(R"({"object": {"i8": 5}, "i32": 1})", "delete object", R"({"i32": 1})"),
        // Delete a whole scalar vector.
        TestArgs(R"({"i32_vector": [1,2,3], "i32": 1})", "delete i32_vector", R"({"i32": 1})"),
        // Delete a scalar vector element.
        TestArgs(R"({"i32_vector": [1,2,3]})", "delete i32_vector[0]", R"({"i32_vector": [2,3]})"),
        TestArgs(R"({"i32_vector": [1,2,3]})", "delete i32_vector[1]", R"({"i32_vector": [1,3]})"),
        TestArgs(R"({"i32_vector": [1,2,3]})", "delete i32_vector[2]", R"({"i32_vector": [1,2]})"),
        TestArgs(R"({"i32_vector": [5]})",     "delete i32_vector[0]", R"({"i32_vector": []})"),
        TestArgs(R"({"i8_vector": [1,2,3,4]})", "delete i8_vector[2]",  R"({"i8_vector": [1,2,4]})"),
        // Delete a string vector element.
        TestArgs(R"({"string_vector": ["a","b","c"]})", "delete string_vector[1]", R"({"string_vector": ["a","c"]})"),
        // Delete an object vector element, and a field inside one.
        TestArgs(R"({"object_vector": [{"i8":1},{"i8":2},{"i8":3}]})", "delete object_vector[1]",    R"({"object_vector": [{"i8":1},{"i8":3}]})"),
        TestArgs(R"({"object_vector": [{"i8":1,"i32":9},{"i8":2}]})",  "delete object_vector[0].i8", R"({"object_vector": [{"i32":9},{"i8":2}]})"),
        // Delete a union field (drops both the union and its type).
        TestArgs(R"({"number_type": "Integer", "number": {"value": 5}, "i32": 1})", "delete number.Integer", R"({"i32": 1})"),
        // Delete a field inside a union member (union stays).
        TestArgs(R"({"number_type": "Integer", "number": {"value": 5}})", "delete number.Integer.value", R"({"number_type": "Integer", "number": {}})"),
        // A union survives when a sibling field is deleted.
        TestArgs(R"({"number_type": "Integer", "number": {"value": 5}, "i32": 10})", "delete i32", R"({"number_type": "Integer", "number": {"value": 5}})"),
        // A union vector survives when a sibling field is deleted, and can be
        // deleted whole (dropping its parallel type vector).
        TestArgs(R"({"number_vector_type": ["Integer","Integer"], "number_vector": [{"value":1},{"value":2}], "i32": 5})", "delete i32",            R"({"number_vector_type": ["Integer","Integer"], "number_vector": [{"value":1},{"value":2}]})"),
        TestArgs(R"({"number_vector_type": ["Integer","Integer"], "number_vector": [{"value":1},{"value":2}], "i32": 5})", "delete number_vector", R"({"i32": 5})"),
        // Deleting an element of an absent vector is a no-op (must not crash).
        TestArgs(R"({"i32": 1})", "delete i32_vector[0]",           R"({"i32": 1})"),
        TestArgs(R"({"i32": 1})", "delete string_vector[0]",       R"({"i32": 1})"),
        TestArgs(R"({"i32": 1})", "delete object_vector[0]",       R"({"i32": 1})"),
        TestArgs(R"({"i32": 1})", "delete number_vector[0].Integer", R"({"i32": 1})"),
        // (Struct field / struct-vector deletes are exercised in StructDeleteTest
        // below -- the TestingMatchers comparator can't compare struct fields.)
        // Delete a whole union-vector element (drops both parallel vectors).
        TestArgs(R"({"number_vector_type": ["Integer","Integer","Integer"], "number_vector": [{"value":1},{"value":2},{"value":3}]})", "delete number_vector[1].Integer", R"({"number_vector_type": ["Integer","Integer"], "number_vector": [{"value":1},{"value":3}]})"),
        // Recurse into a union-vector element and delete a field inside it.
        TestArgs(R"({"number_vector_type": ["Integer"], "number_vector": [{"value":5}]})", "delete number_vector[0].Integer.value", R"({"number_vector_type": ["Integer"], "number_vector": [{}]})")
        // clang-format on
        ));

TEST_P(DeleteTableTest, Delete) {
  const auto& args = GetParam();
  flatbuffers::FlatBufferBuilder fbb;
  auto request = parseFlatbufferRequest(schema_, args.request);
  ASSERT_TRUE(request.has_value())
      << "Failed to parse request: " << args.request;

  auto startingValue =
      flatbuffers::TypedBuffer(schema_, GetBuffer(args.initial));
  fbb.Finish(applyRequest(
      fbb, schema_, flatbuffers::GetAnyRoot(startingValue.buffer().data()),
      // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
      flatbuffers::GetRoot<serialized::Request>(request->data())));
  auto updated = fbb.Release();
  auto expected = GetBuffer(args.final);

  EXPECT_FLATBUFFERS_EQ(schema_, updated, expected) << args.initial << '\n'
                                                    << args.request << '\n'
                                                    << args.final;
}

INSTANTIATE_TEST_SUITE_P(
    PatchTableTest, PatchTableTest,
    ::testing::Values(
        // clang-format off
        // A single nested put on the root.
        TestArgs(R"({})", R"(patch { put i8 99 })", R"({"i8": 99})"),
        // Multiple independent updates in one patch.
        TestArgs(R"({})", R"(patch { put i8 99 ; put i16 88 })", R"({"i8": 99, "i16": 88})"),
        TestArgs(R"({})", R"(patch { put string "hi" ; put i8 5 })", R"({"string": "hi", "i8": 5})"),
        TestArgs(R"({})", R"(patch { put object {"i32": 7} ; put i8 5 })", R"({"object": {"i32": 7}, "i8": 5})"),
        // Put + delete together.
        TestArgs(R"({"i8": 1, "i16": 2})", R"(patch { put i8 99 ; delete i16 })", R"({"i8": 99})"),
        TestArgs(R"({"i32_vector": [1,2,3]})", R"(patch { delete i32_vector[1] ; put i8 9 })", R"({"i8": 9, "i32_vector": [1,3]})"),
        // Patch a subobject (created if absent, merged if present).
        TestArgs(R"({})", R"(patch object { put i8 99 })", R"({"object": {"i8": 99}})"),
        TestArgs(R"({"object": {"i32": 5}})", R"(patch object { put i8 99 })", R"({"object": {"i8": 99, "i32": 5}})"),
        TestArgs(R"({"object": {"i8": 1, "i32": 5}})", R"(patch object { put i8 99 ; delete i32 })", R"({"object": {"i8": 99}})"),
        // Nested patch.
        TestArgs(R"({})", R"(patch object { patch object { put i8 99 } })", R"({"object": {"object": {"i8": 99}}})"),
        TestArgs(R"({"object": {"object": {"i32": 3}}})", R"(patch object { patch object { put i8 9 } })", R"({"object": {"object": {"i8": 9, "i32": 3}}})"),
        // An empty patch is a no-op (top-level and nested).
        TestArgs(R"({"object": {"i8": 5}})", R"(patch object { })", R"({"object": {"i8": 5}})"),
        TestArgs(R"({"i8": 5})", R"(patch { patch { } })", R"({"i8": 5})"),
        TestArgs(R"({})", R"(patch object { put i8 9 ; patch object { } })", R"({"object": {"i8": 9}})")
        // clang-format on
        ));

TEST_P(PatchTableTest, Patch) {
  const auto& args = GetParam();
  flatbuffers::FlatBufferBuilder fbb;
  auto request = parseFlatbufferRequest(schema_, args.request);
  ASSERT_TRUE(request.has_value())
      << "Failed to parse request: " << args.request;

  auto startingValue =
      flatbuffers::TypedBuffer(schema_, GetBuffer(args.initial));
  fbb.Finish(applyRequest(
      fbb, schema_, flatbuffers::GetAnyRoot(startingValue.buffer().data()),
      // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
      flatbuffers::GetRoot<serialized::Request>(request->data())));
  auto updated = fbb.Release();
  auto expected = GetBuffer(args.final);

  EXPECT_FLATBUFFERS_EQ(schema_, updated, expected) << args.initial << '\n'
                                                    << args.request << '\n'
                                                    << args.final;
}

// Struct fields/vectors can't be compared by TestingMatchers (its comparator
// misreads inline struct bytes as table offsets), so these verify the struct
// delete fixes directly through the generated reflection accessors.
namespace {
class BufferRequestTest : public ::testing::Test {
 protected:
  BufferRequestTest()
      : schema_((flatbuffers::LoadFile("test_schema.bfbs", true, &bfbs_),
                 reflection::GetSchema(bfbs_.data()))) {}

  std::vector<uint8_t> GetBuffer(const std::string& json) {
    flatbuffers::Parser parser;
    EXPECT_TRUE(parser.Deserialize(schema_));
    EXPECT_TRUE(parser.Parse(json.c_str())) << parser.error_;
    auto buffer = parser.builder_.Release();
    return {buffer.data(), buffer.data() + buffer.size()};
  }

  /// Applies `request` to `initial` and verifies the result, so a test that
  /// reads a field off the answer is reading a buffer known to be well formed.
  flatbuffers::DetachedBuffer applyRequestTo(
      const std::vector<uint8_t>& initial, const char* request) {
    flatbuffers::FlatBufferBuilder fbb;
    auto parsed = parseFlatbufferRequest(schema_, request);
    EXPECT_TRUE(parsed.has_value()) << request;
    fbb.Finish(applyRequest(
        fbb, schema_, flatbuffers::GetAnyRoot(initial.data()),
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        flatbuffers::GetRoot<serialized::Request>(parsed->data())));
    auto result = fbb.Release();
    auto verifier = flatbuffers::Verifier(result.data(), result.size());
    EXPECT_TRUE(matcher_test::VerifyTestSchemaBuffer(verifier)) << request;
    return result;
  }

  std::string bfbs_;
  const reflection::Schema* schema_;
};

class StructDeleteTest : public BufferRequestTest {};

const char* kStructA =
    R"({"b":false,"i8":1,"ui8":0,"i16":0,"ui16":0,"i32":0,"ui32":0,"i64":0,)"
    R"("ui64":0,"f32":0.0,"f64":0.0,"i32_array":[0,0,0,0,0,0,0,0,0,0]})";
const char* kStructB =
    R"({"b":true,"i8":2,"ui8":0,"i16":0,"ui16":0,"i32":0,"ui32":0,"i64":0,)"
    R"("ui64":0,"f32":0.0,"f64":0.0,"i32_array":[0,0,0,0,0,0,0,0,0,0]})";
}  // namespace

TEST_F(StructDeleteTest, DeletesStructVectorElement) {
  auto initial = GetBuffer(std::string(R"({"struct_vector": [)") + kStructA
                           + "," + kStructB + "]}");
  auto result = applyRequestTo(initial, "delete struct_vector[0]");

  flatbuffers::Verifier verifier(result.data(), result.size());
  ASSERT_TRUE(matcher_test::VerifyTestSchemaBuffer(verifier));
  const auto* ts = matcher_test::GetTestSchema(result.data());
  ASSERT_NE(ts->struct_vector(), nullptr);
  ASSERT_EQ(ts->struct_vector()->size(), 1u);
  // Element A was removed; B (i8 == 2) remains.
  EXPECT_EQ(ts->struct_vector()->Get(0)->i8(), 2);
}

TEST_F(StructDeleteTest, DeleteFieldInsideStructIsNoOp) {
  auto initial = GetBuffer(std::string(R"({"scalars": )") + kStructA + "}");
  auto result = applyRequestTo(initial, "delete scalars.i8");

  flatbuffers::Verifier verifier(result.data(), result.size());
  ASSERT_TRUE(matcher_test::VerifyTestSchemaBuffer(verifier));
  const auto* ts = matcher_test::GetTestSchema(result.data());
  ASSERT_NE(ts->scalars(), nullptr);
  // The struct is indivisible, so the delete is a no-op: i8 is still 1.
  EXPECT_EQ(ts->scalars()->i8(), 1);
}

// An enum-typed field is a SCALAR whose reflection Type also carries an
// `index`, naming the enum. A parser deciding "is the payload a table" from
// `index` alone sends these to the table parser, which has no object to parse
// against and refuses the whole request.
class EnumFieldTest : public BufferRequestTest {};

TEST_F(EnumFieldTest, PutsAnEnumScalarByOrdinal) {
  auto initial = GetBuffer(R"({"i32": 10})");
  auto result = applyRequestTo(initial, "put color 4");
  const auto* ts = matcher_test::GetTestSchema(result.data());
  EXPECT_EQ(ts->color(), matcher_test::Color::Green);
  // The request names one field and must leave the rest alone.
  EXPECT_EQ(ts->i32(), 10);
}

TEST_F(EnumFieldTest, PutsAnEnumVectorElementByOrdinal) {
  auto initial = GetBuffer(R"({"color_vector": ["Red", "Red", "Red"]})");
  auto result = applyRequestTo(initial, "put color_vector[1] 5");
  const auto* ts = matcher_test::GetTestSchema(result.data());
  ASSERT_NE(ts->color_vector(), nullptr);
  ASSERT_EQ(ts->color_vector()->size(), 3U);
  EXPECT_EQ(ts->color_vector()->Get(0), matcher_test::Color::Red);
  EXPECT_EQ(ts->color_vector()->Get(1), matcher_test::Color::Blue);
  EXPECT_EQ(ts->color_vector()->Get(2), matcher_test::Color::Red);
}

TEST_F(EnumFieldTest, PutsAWholeEnumVector) {
  auto initial = GetBuffer(R"({"i32": 10})");
  auto result = applyRequestTo(initial, "put color_vector [0, 4, 5]");
  const auto* ts = matcher_test::GetTestSchema(result.data());
  ASSERT_NE(ts->color_vector(), nullptr);
  ASSERT_EQ(ts->color_vector()->size(), 3U);
  EXPECT_EQ(ts->color_vector()->Get(1), matcher_test::Color::Green);
}

TEST_F(EnumFieldTest, DeletesAnEnumScalar) {
  auto initial = GetBuffer(R"({"color": "Blue", "i32": 10})");
  auto result = applyRequestTo(initial, "delete color");
  const auto* ts = matcher_test::GetTestSchema(result.data());
  EXPECT_EQ(ts->color(), matcher_test::Color::Red);
  EXPECT_EQ(ts->i32(), 10);
}

// A struct is stored INLINE in its parent, with no vtable and no offset of its
// own, so every field of it is always present and the whole of it is rewritten
// whenever any part is. These pin that a put reaches one.
class StructPutTest : public BufferRequestTest {};

TEST_F(StructPutTest, PutsAMemberOfAStoredStruct) {
  auto initial = GetBuffer(std::string(R"({"scalars": )") + kStructA + "}");
  auto result = applyRequestTo(initial, "put scalars.i8 7");
  const auto* ts = matcher_test::GetTestSchema(result.data());
  ASSERT_NE(ts->scalars(), nullptr);
  EXPECT_EQ(ts->scalars()->i8(), 7);
}

TEST_F(StructPutTest, PutsAMemberAndLeavesTheStructsOtherMembers) {
  auto initial = GetBuffer(std::string(R"({"scalars": )") + kStructB + "}");
  auto result = applyRequestTo(initial, "put scalars.i8 7");
  const auto* ts = matcher_test::GetTestSchema(result.data());
  ASSERT_NE(ts->scalars(), nullptr);
  EXPECT_EQ(ts->scalars()->i8(), 7);
  // kStructB carries b == true, which the request does not name.
  EXPECT_TRUE(ts->scalars()->b());
}

TEST_F(StructPutTest, PutsAMemberOfAStructTheBufferDoesNotStore) {
  auto initial = GetBuffer(R"({"i32": 10})");
  auto result = applyRequestTo(initial, "put scalars.i8 7");
  const auto* ts = matcher_test::GetTestSchema(result.data());
  ASSERT_NE(ts->scalars(), nullptr);
  EXPECT_EQ(ts->scalars()->i8(), 7);
  // Everything the request did not name starts from the struct's zeroes.
  EXPECT_EQ(ts->scalars()->i32(), 0);
  EXPECT_EQ(ts->i32(), 10);
}

TEST_F(StructPutTest, PutsAWholeStruct) {
  auto initial = GetBuffer(R"({"i32": 10})");
  auto result =
      applyRequestTo(initial, (std::string("put scalars ") + kStructB).c_str());
  const auto* ts = matcher_test::GetTestSchema(result.data());
  ASSERT_NE(ts->scalars(), nullptr);
  EXPECT_EQ(ts->scalars()->i8(), 2);
  EXPECT_TRUE(ts->scalars()->b());
}

TEST_F(StructPutTest, PutsAnElementOfAFixedLengthArrayInAStruct) {
  auto initial = GetBuffer(std::string(R"({"scalars": )") + kStructA + "}");
  auto result = applyRequestTo(initial, "put scalars.i32_array[3] 9");
  const auto* ts = matcher_test::GetTestSchema(result.data());
  ASSERT_NE(ts->scalars(), nullptr);
  EXPECT_EQ(ts->scalars()->i32_array()->Get(3), 9);
  EXPECT_EQ(ts->scalars()->i32_array()->Get(2), 0);
}

TEST_F(StructPutTest, PutsAWholeFixedLengthArray) {
  auto initial = GetBuffer(std::string(R"({"scalars": )") + kStructA + "}");
  auto result = applyRequestTo(initial, "put scalars.i32_array [7, 8]");
  const auto* ts = matcher_test::GetTestSchema(result.data());
  ASSERT_NE(ts->scalars(), nullptr);
  EXPECT_EQ(ts->scalars()->i32_array()->Get(0), 7);
  EXPECT_EQ(ts->scalars()->i32_array()->Get(1), 8);
  // A fixed-length array cannot shrink, so the elements the payload does not
  // reach keep what they had.
  EXPECT_EQ(ts->scalars()->i32_array()->Get(2), 0);
  EXPECT_EQ(ts->scalars()->i32_array()->size(), 10U);
}

TEST_F(StructPutTest, LeavesAFixedLengthArrayAloneWhenThePayloadIsTooLong) {
  // A fixed-length array cannot grow either. There is no error channel in an
  // apply, so the request writes nothing rather than writing part of itself
  // or running off the end of the struct.
  auto initial = GetBuffer(std::string(R"({"scalars": )") + kStructA + "}");
  auto result = applyRequestTo(
      initial, "put scalars.i32_array [1,2,3,4,5,6,7,8,9,10,11]");
  const auto* ts = matcher_test::GetTestSchema(result.data());
  ASSERT_NE(ts->scalars(), nullptr);
  EXPECT_EQ(ts->scalars()->i32_array()->Get(0), 0);
  EXPECT_EQ(ts->scalars()->i32_array()->size(), 10U);
}

TEST_F(StructPutTest, PutsAnArrayElementOfAStructTheBufferDoesNotStore) {
  auto initial = GetBuffer(R"({"i32": 10})");
  auto result = applyRequestTo(initial, "put scalars.i32_array[2] 5");
  const auto* ts = matcher_test::GetTestSchema(result.data());
  ASSERT_NE(ts->scalars(), nullptr);
  EXPECT_EQ(ts->scalars()->i32_array()->Get(2), 5);
  EXPECT_EQ(ts->scalars()->i32_array()->Get(1), 0);
  EXPECT_EQ(ts->scalars()->i8(), 0);
}

TEST_F(StructPutTest, PutsPartOfAStructAndKeepsTheRest) {
  // A map naming some members: the others keep what the buffer stored, which
  // is what makes a whole-struct put usable as an edit rather than a replace.
  auto initial = GetBuffer(std::string(R"({"scalars": )") + kStructB + "}");
  auto result = applyRequestTo(initial, R"(put scalars {"i16": 40})");
  const auto* ts = matcher_test::GetTestSchema(result.data());
  ASSERT_NE(ts->scalars(), nullptr);
  EXPECT_EQ(ts->scalars()->i16(), 40);
  EXPECT_EQ(ts->scalars()->i8(), 2);
  EXPECT_TRUE(ts->scalars()->b());
}

// Requests the library refuses rather than serving wrongly. Each of these
// reached an apply that wrote a wrong value, built a corrupt buffer, or read
// off the end of one.
class RefusedPutTest : public BufferRequestTest {};

TEST_F(RefusedPutTest, RefusesAnEnumByName) {
  // Apply reads a string as a number, which answers 0 for every name, so a
  // name accepted here would silently write Red. An ordinal is the spelling.
  EXPECT_FALSE(
      parseFlatbufferRequest(schema_, R"(put color "Blue")").has_value());
  EXPECT_FALSE(parseFlatbufferRequest(schema_, "put color Blue").has_value());
  EXPECT_TRUE(parseFlatbufferRequest(schema_, "put color 5").has_value());
}

TEST_F(RefusedPutTest, RefusesAStringWhereANumberBelongs) {
  EXPECT_FALSE(parseFlatbufferRequest(schema_, R"(put i32 "ten")").has_value());
  EXPECT_TRUE(parseFlatbufferRequest(schema_, "put i32 10").has_value());
}

TEST_F(RefusedPutTest, RefusesAStructPayloadThatIsNotAMap) {
  // It used to create a struct of zeroes: the payload named no member, and
  // the image was pushed anyway.
  EXPECT_FALSE(parseFlatbufferRequest(schema_, "put scalars 5").has_value());
}

TEST_F(RefusedPutTest, RefusesWritingAnElementOfAVectorOfStructs) {
  // The apply builds no offset for an inline element while the table builder
  // still consumes one, which reads a neighbouring field's offset.
  EXPECT_FALSE(
      parseFlatbufferRequest(schema_, "put struct_vector[0].i8 5").has_value());
  EXPECT_FALSE(
      parseFlatbufferRequest(schema_, R"(put struct_vector [{"i8": 1}])")
          .has_value());
}

TEST_F(RefusedPutTest, StillDeletesAnElementOfAVectorOfStructs) {
  // A delete removes a whole element, which is well defined where writing one
  // is not, so the refusal above must not reach it.
  EXPECT_TRUE(
      parseFlatbufferRequest(schema_, "delete struct_vector[0]").has_value());
}

TEST_F(RefusedPutTest, RefusesAWholeVectorOfUnions) {
  // A union vector pairs with a vector of discriminators that a JSON array
  // cannot carry, and its type index names the ENUM rather than an object.
  EXPECT_FALSE(
      parseFlatbufferRequest(schema_, R"(put number_vector [{"value": 1}])")
          .has_value());
}

// A struct member that is itself a struct, and an array whose elements are
// structs.
class NestedStructTest : public BufferRequestTest {};

TEST_F(NestedStructTest, PutsAMemberOfAStructInsideAStruct) {
  auto result = applyRequestTo(GetBuffer(R"({"i32": 10})"),
                               "put nested.inner.a 3");
  const auto* ts = matcher_test::GetTestSchema(result.data());
  ASSERT_NE(ts->nested(), nullptr);
  EXPECT_EQ(ts->nested()->inner().a(), 3);
  EXPECT_EQ(ts->nested()->tail(), 0);
}

TEST_F(NestedStructTest, PutsAnArrayElementInsideANestedStruct) {
  auto result = applyRequestTo(GetBuffer(R"({"i32": 10})"),
                               "put nested.inner.arr[1] 55");
  const auto* ts = matcher_test::GetTestSchema(result.data());
  ASSERT_NE(ts->nested(), nullptr);
  EXPECT_EQ(ts->nested()->inner().arr()->Get(1), 55);
  EXPECT_EQ(ts->nested()->inner().arr()->Get(0), 0);
}

TEST_F(NestedStructTest, PutsAMemberAfterANestedStructWithoutDisturbingIt) {
  auto result = applyRequestTo(
      GetBuffer(R"({"i32": 10})"),
      R"(put nested {"inner": {"a": 4}, "tail": 77})");
  const auto* ts = matcher_test::GetTestSchema(result.data());
  ASSERT_NE(ts->nested(), nullptr);
  EXPECT_EQ(ts->nested()->inner().a(), 4);
  EXPECT_EQ(ts->nested()->tail(), 77);
}

TEST_F(NestedStructTest, RefusesAnElementOfAnArrayOfStructs) {
  // The same rule as a vector of structs: the element is inline and nothing
  // builds one.
  EXPECT_FALSE(
      parseFlatbufferRequest(schema_, "put nested.structs[0].a 2").has_value());
}

TEST_F(NestedStructTest, WritesNothingIntoAnArrayOfStructsNamedInAMap) {
  // The stride for a struct element is its byte size, which `GetTypeSize`
  // does not answer, so nothing writes one. The struct keeps what it had
  // rather than being written at the wrong stride.
  auto initial = GetBuffer(R"({"nested": {"inner": {"a": 1, "arr": [0,0,0]},)"
                           R"( "structs": [{"a": 8, "arr": [0,0,0]},)"
                           R"( {"a": 9, "arr": [0,0,0]}], "tail": 3}})");
  auto result =
      applyRequestTo(initial, R"(put nested {"structs": [{"a": 2}]})");
  const auto* ts = matcher_test::GetTestSchema(result.data());
  ASSERT_NE(ts->nested(), nullptr);
  EXPECT_EQ(ts->nested()->structs()->Get(0)->a(), 8);
  EXPECT_EQ(ts->nested()->tail(), 3);
}

TEST_F(NestedStructTest, LeavesAnAbsentStructAbsentWhenNothingIsWritten) {
  // A refused write must not leave a struct of zeroes where there was none.
  auto result = applyRequestTo(GetBuffer(R"({"i32": 10})"),
                               R"(put nested {"structs": [{"a": 2}]})");
  const auto* ts = matcher_test::GetTestSchema(result.data());
  EXPECT_EQ(ts->nested(), nullptr);
  EXPECT_EQ(ts->i32(), 10);
}

TEST_F(NestedStructTest, LeavesAnAbsentStructAbsentWhenAnArrayIsTooLong) {
  auto result = applyRequestTo(GetBuffer(R"({"i32": 10})"),
                               "put scalars.i32_array [1,2,3,4,5,6,7,8,9,10,11]");
  const auto* ts = matcher_test::GetTestSchema(result.data());
  EXPECT_EQ(ts->scalars(), nullptr);
  EXPECT_EQ(ts->i32(), 10);
}
