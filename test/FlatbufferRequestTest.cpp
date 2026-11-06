#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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

// Applies `request` to `source` and finishes `fbb` with the result, failing
// the test unless the apply is Applied. Where it is not, `fbb` is finished
// with a copy of `source`, so a caller reading fields reads a whole buffer.
void finishApplied(flatbuffers::FlatBufferBuilder& fbb,
                   const reflection::Schema* schema,
                   const flatbuffers::Table* source,
                   const serialized::Request* request) {
  const auto result = tryApplyRequest(fbb, schema, source, request);
  EXPECT_EQ(result.outcome, ApplyOutcome::Applied);
  if (result.outcome == ApplyOutcome::Applied) {
    fbb.Finish(result.root);
    return;
  }
  fbb.Finish(flatbuffers::CopyTable(fbb, *schema, *schema->root_table(),
                                    *source, false));
}
}  // namespace

namespace fbrequest {
void PrintTo(ApplyOutcome outcome, std::ostream* out) {
  switch (outcome) {
    case ApplyOutcome::Applied:
      *out << "Applied";
      return;
    case ApplyOutcome::NotStored:
      *out << "NotStored";
      return;
    case ApplyOutcome::Refused:
      *out << "Refused";
      return;
  }
  *out << static_cast<int>(outcome);
}
}  // namespace fbrequest

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
    if (not request.has_value()) {
      ADD_FAILURE() << "Failed to parse request: " << requestStr;
      copyTable(builder, schema_, emptyBuffer_.data(), false);
      return builder.Release();
    }

    finishApplied(builder, schema_,
                  flatbuffers::GetAnyRoot(emptyBuffer_.data()),
                  flatbuffers::GetRoot<serialized::Request>(request->data()));

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

// clang-format off
INSTANTIATE_TEST_SUITE_P(
    UpdateTableTest, UpdateTableTest,
    ::testing::Values(
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

        // Payloads holding 8-byte scalars. A payload, or a table nested in one as a
        // flexbuffer blob, is carried at whatever alignment its container gives it,
        // which an 8-byte (or, for a blob, a 4-byte) read in place can misalign.
        TestArgs(R"({})", R"(put object_vector [{"i64": 9223372036854775807}, {"i8": 1, "f64": 0.1}, {"ui64": 18446744073709551615}])", R"({"object_vector": [{"i64": 9223372036854775807}, {"i8": 1, "f64": 0.1}, {"ui64": 18446744073709551615}]})"),
        TestArgs(R"({})", R"(put object {"i64": 9223372036854775807, "f64": 0.1})", R"({"object": {"i64": 9223372036854775807, "f64": 0.1}})"),
        TestArgs(R"({})", "put i64 9223372036854775807", R"({"i64": 9223372036854775807})"),
        TestArgs(R"({})", "put f64 0.1",                 R"({"f64": 0.1})"),
        TestArgs(R"({})", "put i64_vector [9223372036854775807, -9223372036854775808]", R"({"i64_vector": [9223372036854775807, -9223372036854775808]})"),

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
        TestArgs(R"({"number_vector_type": ["Integer", "Integer", "Integer", "Integer"], "number_vector": [{"value":1},{"value":2},{"value":3},{"value":4}]})", R"(put number_vector[6].FloatingPoint {"value":9.0})", R"({"number_vector_type": ["Integer", "Integer", "Integer", "Integer", "Integer", "Integer", "FloatingPoint"], "number_vector": [{"value":1},{"value":2},{"value":3},{"value":4},{},{},{"value":9}]})"),

        // An element of a vector the buffer does not store creates the
        // vector, padded the way a stored vector grows past its end: empty
        // strings, empty tables, and the first member for a union.
        TestArgs(R"({"i32": 1})", R"(put string_vector[0] "z")",            R"({"i32": 1, "string_vector": ["z"]})"),
        TestArgs(R"({"i32": 1})", R"(put string_vector[2] "z")",            R"({"i32": 1, "string_vector": ["", "", "z"]})"),
        TestArgs(R"({"i32": 1})", R"(put object_vector[0] {"i8":9})",       R"({"i32": 1, "object_vector": [{"i8":9}]})"),
        TestArgs(R"({"i32": 1})", R"(put object_vector[1].i8 9)",           R"({"i32": 1, "object_vector": [{}, {"i8":9}]})"),
        TestArgs(R"({"i32": 1})", R"(put number_vector[0].Integer {"value":9})", R"({"i32": 1, "number_vector_type": ["Integer"], "number_vector": [{"value":9}]})"),
        TestArgs(R"({"i32": 1})", R"(put number_vector[1].FloatingPoint.value 9)", R"({"i32": 1, "number_vector_type": ["Integer", "FloatingPoint"], "number_vector": [{}, {"value":9.0}]})"),

        // The same, inside a table the buffer does not store either.
        TestArgs(R"({"i32": 1})", R"(put object.i32_vector[1] 9)",          R"({"i32": 1, "object": {"i32_vector": [0, 9]}})"),
        TestArgs(R"({"i32": 1})", R"(put object.color_vector[0] 4)",        R"({"i32": 1, "object": {"color_vector": ["Green"]}})"),
        TestArgs(R"({"i32": 1})", R"(put object.string_vector[1] "z")",     R"({"i32": 1, "object": {"string_vector": ["", "z"]}})"),
        TestArgs(R"({"i32": 1})", R"(put object.object_vector[0].i8 9)",    R"({"i32": 1, "object": {"object_vector": [{"i8":9}]}})"),
        TestArgs(R"({"i32": 1})", R"(put object.number_vector[0].Integer.value 9)", R"({"i32": 1, "object": {"number_vector_type": ["Integer"], "number_vector": [{"value":9}]}})"),
        TestArgs(R"({"i32": 1})", R"(put object.object.object_vector[1] {"i8":9})", R"({"i32": 1, "object": {"object": {"object_vector": [{}, {"i8":9}]}}})"),

        // Switching an element's member carries nothing across, as for a
        // single union.
        TestArgs(R"({"number_vector_type": ["Integer"], "number_vector": [{"value":1}]})", R"(put number_vector[0].Fraction.denominator 5)", R"({"number_vector_type": ["Fraction"], "number_vector": [{"denominator":5}]})")
        ));
// clang-format on

TEST_P(UpdateTableTest, PutScalarInRoot) {
  const auto& args = GetParam();
  flatbuffers::FlatBufferBuilder fbb;
  auto request = parseFlatbufferRequest(schema_, args.request);
  ASSERT_TRUE(request.has_value()) << true << args.initial << '\n'
                                   << args.request << '\n'
                                   << args.final;

  auto startingValue =
      flatbuffers::TypedBuffer(schema_, GetBuffer(args.initial));
  finishApplied(fbb, schema_,
                flatbuffers::GetAnyRoot(startingValue.buffer().data()),
                // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
                flatbuffers::GetRoot<serialized::Request>(request->data()));
  auto updated = fbb.Release();
  auto expected = GetBuffer(args.final);

  EXPECT_FLATBUFFERS_EQ(schema_, updated, expected)
      << true << args.initial << '\n'
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
        // (The comparator compares a struct's presence but not its contents,
        // so struct-vector deletes are exercised in StructDeleteTest below.)
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
  finishApplied(fbb, schema_,
                flatbuffers::GetAnyRoot(startingValue.buffer().data()),
                // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
                flatbuffers::GetRoot<serialized::Request>(request->data()));
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
        TestArgs(R"({})", R"(patch { put object {"i64": 9223372036854775807, "f64": 0.1} ; put f64 0.1 })", R"({"object": {"i64": 9223372036854775807, "f64": 0.1}, "f64": 0.1})"),
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
  finishApplied(fbb, schema_,
                flatbuffers::GetAnyRoot(startingValue.buffer().data()),
                // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
                flatbuffers::GetRoot<serialized::Request>(request->data()));
  auto updated = fbb.Release();
  auto expected = GetBuffer(args.final);

  EXPECT_FLATBUFFERS_EQ(schema_, updated, expected) << args.initial << '\n'
                                                    << args.request << '\n'
                                                    << args.final;
}

// A delete of something the buffer does not store is NotStored, and builds
// nothing.
class NotStoredDeleteTest : public RequestTestBase {};

INSTANTIATE_TEST_SUITE_P(
    NotStoredDeleteTest, NotStoredDeleteTest,
    ::testing::Values(
        // clang-format off
        // A field the buffer does not store.
        TestArgs(R"({"i32": 10})", "delete i8",          ""),
        TestArgs(R"({"i32": 10})", "delete string",      ""),
        TestArgs(R"({"i32": 10})", "delete object",      ""),
        TestArgs(R"({"i32": 10})", "delete number",      ""),
        TestArgs(R"({"i32": 10})", "delete scalars",     ""),
        TestArgs(R"({"i32": 10})", "delete i32_vector",  ""),
        // An element of a vector the buffer does not store.
        TestArgs(R"({"i32": 1})", "delete i32_vector[0]",            ""),
        TestArgs(R"({"i32": 1})", "delete string_vector[0]",         ""),
        TestArgs(R"({"i32": 1})", "delete object_vector[0]",         ""),
        TestArgs(R"({"i32": 1})", "delete object_vector[0].i8",      ""),
        TestArgs(R"({"i32": 1})", "delete number_vector[0]",         ""),
        TestArgs(R"({"i32": 1})", "delete number_vector[0].Integer", ""),
        TestArgs(R"({"i32": 1})", "delete struct_vector[0]",         ""),
        // An element past the end of a stored vector.
        TestArgs(R"({"i32_vector": [1, 2]})",                  "delete i32_vector[2]",        ""),
        TestArgs(R"({"i8_vector": [1, 2]})",                   "delete i8_vector[9]",         ""),
        TestArgs(R"({"string_vector": ["a"]})",                "delete string_vector[1]",     ""),
        TestArgs(R"({"object_vector": [{"i8": 1}]})",          "delete object_vector[1]",     ""),
        TestArgs(R"({"object_vector": [{"i8": 1}]})",          "delete object_vector[1].i8",  ""),
        TestArgs(R"({"number_vector_type": ["Integer"], "number_vector": [{"value": 1}]})", "delete number_vector[1]",               ""),
        TestArgs(R"({"number_vector_type": ["Integer"], "number_vector": [{"value": 1}]})", "delete number_vector[1].Integer.value", ""),
        // Anything inside a table or union the buffer does not store.
        TestArgs(R"({"i32": 1})", "delete object.i8",                   ""),
        TestArgs(R"({"i32": 1})", "delete object.i32_vector[0]",        ""),
        TestArgs(R"({"i32": 1})", "delete object.object_vector[0].i8",  ""),
        TestArgs(R"({"i32": 1})", "delete number.Integer",              ""),
        TestArgs(R"({"i32": 1})", "delete number.Integer.value",        ""),
        TestArgs(R"({"i32": 1})", "delete object.number.Integer.value", ""),
        // A field a stored table does not store.
        TestArgs(R"({"object": {"i32": 1}})", "delete object.i8",                    ""),
        TestArgs(R"({"object_vector": [{"i32": 1}]})", "delete object_vector[0].i8", ""),
        TestArgs(R"({"number_type": "Integer", "number": {}})", "delete number.Integer.value", ""),
        // A member the union does not hold, or a field of one.
        TestArgs(R"({"number_type": "Integer", "number": {"value": 5}})", "delete number.Fraction",           ""),
        TestArgs(R"({"number_type": "Integer", "number": {"value": 5}})", "delete number.Fraction.numerator", ""),
        TestArgs(R"({"number_vector_type": ["Integer"], "number_vector": [{"value": 5}]})", "delete number_vector[0].Fraction",           ""),
        TestArgs(R"({"number_vector_type": ["Integer"], "number_vector": [{"value": 5}]})", "delete number_vector[0].Fraction.numerator", ""),
        // A patch is all or nothing: one operation that is NotStored is the
        // patch's outcome, whatever the operations around it did.
        TestArgs(R"({"i8": 1})", "patch { put i8 2 ; delete i16 }",      ""),
        TestArgs(R"({"i8": 1})", "patch { delete i16 ; put i8 2 }",      ""),
        TestArgs(R"({"i32": 1})", "patch object { delete i8 }",          ""),
        TestArgs(R"({"i8": 1})", "patch { patch object { delete i8 } }", ""),
        TestArgs(R"({"i8": 1})", "patch { delete i8 ; delete i8 }",      "")
        // clang-format on
        ));

TEST_P(NotStoredDeleteTest, BuildsNothing) {
  const auto& args = GetParam();
  auto request = parseFlatbufferRequest(schema_, args.request);
  ASSERT_TRUE(request.has_value())
      << "Failed to parse request: " << args.request;
  const auto initial = GetBuffer(args.initial);
  flatbuffers::FlatBufferBuilder fbb;
  const auto result = tryApplyRequest(
      fbb, schema_, flatbuffers::GetAnyRoot(initial.data()),
      // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
      flatbuffers::GetRoot<serialized::Request>(request->data()));
  EXPECT_EQ(result.outcome, ApplyOutcome::NotStored) << args.request;
  EXPECT_TRUE(result.root.IsNull()) << args.request;
  EXPECT_EQ(fbb.GetSize(), 0U) << args.request;

  // applyRequest answers the same with a null root.
  flatbuffers::FlatBufferBuilder legacy;
  EXPECT_TRUE(
      applyRequest(legacy, schema_, flatbuffers::GetAnyRoot(initial.data()),
                   // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
                   flatbuffers::GetRoot<serialized::Request>(request->data()))
          .IsNull())
      << args.request;
  EXPECT_EQ(legacy.GetSize(), 0U) << args.request;
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
      const std::vector<uint8_t>& initial, std::string_view request) {
    flatbuffers::FlatBufferBuilder fbb;
    auto parsed = parseFlatbufferRequest(schema_, request);
    if (not parsed.has_value()) {
      // The caller reads fields off the answer, so it gets the initial
      // buffer back unchanged rather than nothing to read.
      ADD_FAILURE() << "Failed to parse request: " << request;
      copyTable(fbb, schema_, initial.data(), false);
      return fbb.Release();
    }
    finishApplied(fbb, schema_, flatbuffers::GetAnyRoot(initial.data()),
                  flatbuffers::GetRoot<serialized::Request>(parsed->data()));
    auto result = fbb.Release();
    auto verifier = flatbuffers::Verifier(result.data(), result.size());
    EXPECT_TRUE(matcher_test::VerifyTestSchemaBuffer(verifier)) << request;
    return result;
  }

  /// What applying `request`, a serialized Request, to `initial` comes to.
  /// Checks that nothing is built for an outcome other than Applied, and that
  /// an Applied result verifies.
  ApplyOutcome outcomeOfRequest(const std::vector<uint8_t>& initial,
                                const uint8_t* request) {
    flatbuffers::FlatBufferBuilder fbb;
    const auto result =
        tryApplyRequest(fbb, schema_, flatbuffers::GetAnyRoot(initial.data()),
                        flatbuffers::GetRoot<serialized::Request>(request));
    if (result.outcome != ApplyOutcome::Applied) {
      EXPECT_TRUE(result.root.IsNull());
      EXPECT_EQ(fbb.GetSize(), 0U);
      return result.outcome;
    }
    fbb.Finish(result.root);
    auto verifier =
        flatbuffers::Verifier(fbb.GetBufferPointer(), fbb.GetSize());
    EXPECT_TRUE(matcher_test::VerifyTestSchemaBuffer(verifier));
    return result.outcome;
  }

  /// What applying the request `text` to `initial` comes to, as
  /// `outcomeOfRequest` checks it. The request must parse.
  ApplyOutcome outcomeOf(const std::vector<uint8_t>& initial,
                         std::string_view text) {
    auto parsed = parseFlatbufferRequest(schema_, text);
    if (not parsed.has_value()) {
      ADD_FAILURE() << "Failed to parse request: " << text;
      return ApplyOutcome::Refused;
    }
    return outcomeOfRequest(initial, parsed->data());
  }

  bool parses(std::string_view request) {
    return parseFlatbufferRequest(schema_, request).has_value();
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

TEST_F(StructDeleteTest, RefusesADeleteInsideAStruct) {
  // A struct's members are all always stored: there is no member, nor
  // element of an array in one, for a delete to remove.
  for (const auto* request : {
           "delete scalars.i8",
           "delete object.scalars.i8",
           "delete scalars.i32_array[3]",
           "delete nested.inner.a",
           "delete nested.inner.arr[0]",
           "delete nested.structs[1]",
           "delete nested.structs[1].a",
           "delete struct_vector[0].i8",
           "patch scalars { delete i8 }",
           "patch nested { delete tail }",
       }) {
    EXPECT_FALSE(parses(request)) << request;
  }
  EXPECT_TRUE(parses("delete scalars"));
  EXPECT_TRUE(parses("delete struct_vector[0]"));
}

TEST_F(StructDeleteTest, FindsNothingStoredForAStructVectorElementPastItsEnd) {
  auto initial =
      GetBuffer(std::string(R"({"struct_vector": [)") + kStructA + "]}");
  EXPECT_EQ(outcomeOf(initial, "delete struct_vector[1]"),
            ApplyOutcome::NotStored);
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

TEST_F(RefusedPutTest, RefusesAUnionMemberThatHoldsNoTable) {
  // NONE is a member of every union, and names no object for the path to
  // continue into or the payload to be parsed against.
  EXPECT_FALSE(
      parseFlatbufferRequest(schema_, "put number.NONE {}").has_value());
  EXPECT_FALSE(
      parseFlatbufferRequest(schema_, "put number.NONE.value 1").has_value());
  EXPECT_FALSE(parseFlatbufferRequest(schema_, "put number_vector[0].NONE {}")
                   .has_value());
  EXPECT_FALSE(
      parseFlatbufferRequest(schema_, "delete number.NONE").has_value());
  EXPECT_FALSE(
      parseFlatbufferRequest(schema_, "patch number.NONE { put value 1 }")
          .has_value());
  EXPECT_TRUE(
      parseFlatbufferRequest(schema_, "delete number.Integer").has_value());
}

TEST_F(RefusedPutTest, RefusesAWholeVectorOfUnions) {
  // A union vector pairs with a vector of discriminators that a JSON array
  // cannot carry, and its type index names the ENUM rather than an object.
  EXPECT_FALSE(
      parseFlatbufferRequest(schema_, R"(put number_vector [{"value": 1}])")
          .has_value());
}

// The parser refuses a request that cannot make the edit it names, but a
// request is a buffer that can come from anywhere. The apply answers Refused
// for one, builds nothing, and leaves the buffer it was given alone.
class HandBuiltTest : public BufferRequestTest {
 protected:
  ApplyOutcome putOutcome(const std::vector<uint8_t>& initial,
                          const std::vector<uint32_t>& path,
                          const std::vector<uint8_t>& payload = {}) {
    flatbuffers::FlatBufferBuilder requestBuilder;
    requestBuilder.Finish(serialized::CreateRequest(
        requestBuilder, serialized::Operation::Put,
        serialized::CreatePut(requestBuilder, requestBuilder.CreateVector(path),
                              requestBuilder.CreateVector(payload))
            .Union()));
    return outcomeOfRequest(initial, requestBuilder.GetBufferPointer());
  }

  ApplyOutcome deleteOutcome(const std::vector<uint8_t>& initial,
                             const std::vector<uint32_t>& path) {
    flatbuffers::FlatBufferBuilder requestBuilder;
    requestBuilder.Finish(serialized::CreateRequest(
        requestBuilder, serialized::Operation::Delete,
        serialized::CreateDelete(requestBuilder,
                                 requestBuilder.CreateVector(path))
            .Union()));
    return outcomeOfRequest(initial, requestBuilder.GetBufferPointer());
  }

  // A buffer storing `string`, `object`, `i16` and `i32`, and a `number`
  // whose discriminant is `member` and whose value is an Integer of 3.
  static std::vector<uint8_t> withUnion(uint8_t member) {
    flatbuffers::FlatBufferBuilder fbb;
    const auto string = fbb.CreateString("kept");
    auto objectBuilder = matcher_test::TestSchemaBuilder{fbb};
    objectBuilder.add_i8(3);
    const auto object = objectBuilder.Finish();
    const auto integer = matcher_test::CreateInteger(fbb, 3);
    auto builder = matcher_test::TestSchemaBuilder{fbb};
    builder.add_string(string);
    builder.add_object(object);
    builder.add_i16(9);
    builder.add_i32(7);
    builder.add_number_type(static_cast<matcher_test::Number>(member));
    builder.add_number(integer.Union());
    fbb.Finish(builder.Finish());
    return {fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize()};
  }

  // A flexbuffer payload holding `json`, as the parser would carry it.
  static std::vector<uint8_t> flexPayload(const char* json) {
    flatbuffers::Parser parser;
    EXPECT_TRUE(parser.ParseFlexBuffer(json, nullptr, &parser.flex_builder_))
        << json;
    return parser.flex_builder_.GetBuffer();
  }

  uint16_t fieldOffset(const char* name) {
    return schema_->root_table()->fields()->LookupByKey(name)->offset();
  }

  static void expectSiblingsKept(const matcher_test::TestSchema* ts) {
    ASSERT_NE(ts->string(), nullptr);
    EXPECT_EQ(ts->string()->string_view(), "kept");
    ASSERT_NE(ts->object(), nullptr);
    EXPECT_EQ(ts->object()->i8(), 3);
    EXPECT_EQ(ts->i32(), 7);
  }
};

constexpr auto kNone = uint8_t{0};
constexpr auto kUndeclared = uint8_t{99};
constexpr auto kInteger = static_cast<uint8_t>(matcher_test::Number::Integer);

TEST_F(HandBuiltTest, RefusesAPutOfAUnionMemberThatHoldsNoTable) {
  for (const auto& initial :
       {GetBuffer(R"({"string": "kept", "object": {"i8": 3}, "i32": 7})"),
        withUnion(kInteger)}) {
    for (const auto& path :
         {std::vector<uint32_t>{fieldOffset("number"), kNone},
          std::vector<uint32_t>{fieldOffset("number"), kUndeclared},
          std::vector<uint32_t>{fieldOffset("number"), kNone, 4},
          std::vector<uint32_t>{fieldOffset("number"), kUndeclared, 4}}) {
      EXPECT_EQ(putOutcome(initial, path), ApplyOutcome::Refused);
    }
  }
}

// A payload stored under NONE, or under a member the union does not declare,
// has no layout the schema gives, so no copy of it can come out as it went
// in. A request that would copy one is refused; one that replaces or removes
// the union never reads it.
TEST_F(HandBuiltTest, RefusesToCopyAStoredUnionWithNoTableToReadItAs) {
  for (const auto member : {kNone, kUndeclared}) {
    const auto initial = withUnion(member);
    for (const auto* request :
         {"put i8 5", "delete i16", "patch { put i8 5 }", "patch {}"}) {
      EXPECT_EQ(outcomeOf(initial, request), ApplyOutcome::Refused) << request;
    }
    auto result = applyRequestTo(initial, "delete number");
    const auto* cleared = matcher_test::GetTestSchema(result.data());
    EXPECT_EQ(cleared->number_type(), matcher_test::Number::NONE);
    EXPECT_EQ(cleared->number(), nullptr);
    expectSiblingsKept(cleared);

    result = applyRequestTo(initial, "put number.Fraction.numerator 4");
    const auto* switched = matcher_test::GetTestSchema(result.data());
    ASSERT_NE(switched->number_as_Fraction(), nullptr);
    EXPECT_EQ(switched->number_as_Fraction()->numerator(), 4);
    expectSiblingsKept(switched);
  }
}

TEST_F(HandBuiltTest, RefusesAPutOfAMemberPastADiscriminantsRange) {
  // 257 truncated to a discriminant's 8 bits would be Integer.
  const auto initial =
      GetBuffer(R"({"string": "kept", "object": {"i8": 3}, "i32": 7})");
  EXPECT_EQ(putOutcome(initial, {fieldOffset("number"), 256U + kInteger}),
            ApplyOutcome::Refused);
}

TEST_F(HandBuiltTest, RefusesAPutOfAPathStoppingAtAUnion) {
  const auto initial = withUnion(kInteger);
  EXPECT_EQ(putOutcome(initial, {fieldOffset("number")}),
            ApplyOutcome::Refused);
  EXPECT_EQ(putOutcome(initial, {fieldOffset("number_vector"), 0}),
            ApplyOutcome::Refused);
}

TEST_F(HandBuiltTest, RefusesAUnionVectorPutNamingNoTable) {
  const auto initial = GetBuffer(
      R"({"string": "kept", "object": {"i8": 3}, "i32": 7, )"
      R"("number_vector_type": ["Integer"], "number_vector": [{"value": 4}]})");
  for (const auto& path :
       {std::vector<uint32_t>{fieldOffset("number_vector"), 0, kNone, 4},
        std::vector<uint32_t>{fieldOffset("number_vector"), 1, kNone, 4},
        std::vector<uint32_t>{fieldOffset("number_vector"), 1, 256U + kInteger,
                              4}}) {
    EXPECT_EQ(putOutcome(initial, path), ApplyOutcome::Refused);
  }
}

TEST_F(HandBuiltTest, RefusesADeleteOfAMemberPastADiscriminantsRange) {
  const auto initial = GetBuffer(
      R"({"string": "kept", "object": {"i8": 3}, "i32": 7, )"
      R"("number_type": "Integer", "number": {"value": 5}, )"
      R"("number_vector_type": ["Integer"], "number_vector": [{"value": 4}]})");
  EXPECT_EQ(deleteOutcome(initial, {fieldOffset("number"), 256U + kInteger}),
            ApplyOutcome::Refused);
  EXPECT_EQ(deleteOutcome(initial,
                          {fieldOffset("number_vector"), 0, 256U + kInteger}),
            ApplyOutcome::Refused);
}

TEST_F(HandBuiltTest, RefusesAPutGrowingAVectorPastWhatAFlatbufferHolds) {
  const auto initial = GetBuffer(
      R"({"string": "kept", "object": {"i8": 3}, "i32": 7, )"
      R"("i32_vector": [1], "string_vector": ["a"], "object_vector": [{}]})");
  for (const auto* name : {"i32_vector", "string_vector", "object_vector"}) {
    EXPECT_EQ(
        putOutcome(initial, {fieldOffset(name), 4000000000U}, flexPayload("1")),
        ApplyOutcome::Refused)
        << name;
  }
}

TEST_F(HandBuiltTest, RefusesAPathTheSchemaDoesNotResolve) {
  const auto initial = withUnion(kInteger);
  // 2 is no field's vtable offset; vtable offsets start at 4.
  EXPECT_EQ(putOutcome(initial, {2}, flexPayload("1")), ApplyOutcome::Refused);
  EXPECT_EQ(deleteOutcome(initial, {2}), ApplyOutcome::Refused);
  EXPECT_EQ(deleteOutcome(initial, {}), ApplyOutcome::Refused);
  EXPECT_EQ(deleteOutcome(initial, {fieldOffset("number_type")}),
            ApplyOutcome::Refused);
  EXPECT_EQ(deleteOutcome(initial, {fieldOffset("i32"), 0}),
            ApplyOutcome::Refused);
}

TEST_F(HandBuiltTest, RefusesADeleteInsideAStruct) {
  const auto initial = GetBuffer(std::string(R"({"scalars": )") + kStructA
                                 + R"(, "struct_vector": [)" + kStructA + "]}");
  const auto* scalarsDef =
      schema_->objects()->LookupByKey("matcher_test.ScalarStruct");
  ASSERT_NE(scalarsDef, nullptr);
  const auto i8 = scalarsDef->fields()->LookupByKey("i8")->offset();
  EXPECT_EQ(deleteOutcome(initial, {fieldOffset("scalars"), i8}),
            ApplyOutcome::Refused);
  EXPECT_EQ(deleteOutcome(initial, {fieldOffset("struct_vector"), 0, i8}),
            ApplyOutcome::Refused);
  // Refused whether or not the buffer stores the struct.
  EXPECT_EQ(deleteOutcome(GetBuffer("{}"), {fieldOffset("scalars"), i8}),
            ApplyOutcome::Refused);
}

TEST_F(HandBuiltTest, RefusesAPayloadThatIsNotWhatItsFieldHolds) {
  const auto initial = withUnion(kInteger);
  // Not a flexbuffer at all, and none.
  EXPECT_EQ(putOutcome(initial, {fieldOffset("i32")}, {1, 2, 3}),
            ApplyOutcome::Refused);
  EXPECT_EQ(putOutcome(initial, {fieldOffset("string")}),
            ApplyOutcome::Refused);
  // A flexbuffer of the wrong kind.
  EXPECT_EQ(putOutcome(initial, {fieldOffset("string")}, flexPayload("5")),
            ApplyOutcome::Refused);
  EXPECT_EQ(putOutcome(initial, {fieldOffset("i32")}, flexPayload(R"("5")")),
            ApplyOutcome::Refused);
  EXPECT_EQ(
      putOutcome(initial, {fieldOffset("i32_vector")}, flexPayload(R"({})")),
      ApplyOutcome::Refused);
  EXPECT_EQ(
      putOutcome(initial, {fieldOffset("object_vector")}, flexPayload("[1]")),
      ApplyOutcome::Refused);
  EXPECT_EQ(putOutcome(initial, {fieldOffset("i32")}, flexPayload("5")),
            ApplyOutcome::Applied);
}

TEST_F(HandBuiltTest, RefusesAStructPayloadThatWritesNothing) {
  const auto initial = GetBuffer(R"({"i32": 10})");
  for (const auto* json : {"{}", R"({"zzz": 2})", R"({"i8": 1, "zzz": 2})",
                           R"({"i8": "x"})", R"({"i32_array": []})", "5"}) {
    EXPECT_EQ(putOutcome(initial, {fieldOffset("scalars")}, flexPayload(json)),
              ApplyOutcome::Refused)
        << json;
  }
  EXPECT_EQ(putOutcome(initial, {fieldOffset("scalars")},
                       flexPayload(R"({"i8": 1})")),
            ApplyOutcome::Applied);
}

TEST_F(HandBuiltTest, RefusesATablePayloadThatIsNotThatTable) {
  const auto initial = withUnion(kInteger);
  auto truncated = GetBuffer(R"({"i32": 7, "string": "abc"})");
  truncated.resize(truncated.size() / 2);
  for (const auto& path : {
           std::vector<uint32_t>{},
           std::vector<uint32_t>{fieldOffset("object")},
           std::vector<uint32_t>{fieldOffset("number"), kInteger},
           std::vector<uint32_t>{fieldOffset("object_vector"), 0},
           std::vector<uint32_t>{fieldOffset("number_vector"), 0, kInteger},
       }) {
    for (const auto& payload :
         {std::vector<uint8_t>{}, flexPayload(R"({"i32": 1})"), truncated}) {
      EXPECT_EQ(putOutcome(initial, path, payload), ApplyOutcome::Refused)
          << path.size() << " " << payload.size();
    }
  }
  // A blob in a whole vector of tables is read as a table too.
  flexbuffers::Builder blobs;
  blobs.Vector([&] { blobs.Blob(truncated.data(), truncated.size()); });
  blobs.Finish();
  EXPECT_EQ(
      putOutcome(initial, {fieldOffset("object_vector")}, blobs.GetBuffer()),
      ApplyOutcome::Refused);
}

TEST_F(HandBuiltTest, RefusesARequestCarryingNoOperation) {
  const auto initial = withUnion(kInteger);
  for (const auto type :
       {serialized::Operation::Put, serialized::Operation::Patch,
        serialized::Operation::Delete, serialized::Operation::NONE}) {
    flatbuffers::FlatBufferBuilder requestBuilder;
    auto request = serialized::RequestBuilder{requestBuilder};
    request.add_operation_type(type);
    requestBuilder.Finish(request.Finish());
    EXPECT_EQ(outcomeOfRequest(initial, requestBuilder.GetBufferPointer()),
              ApplyOutcome::Refused);
  }
}

TEST_F(HandBuiltTest,
       RefusesAPatchHoldingARefusedOperationBesideANotStoredOne) {
  // Refused wins whichever comes first: a caller that keeps its source for
  // NotStored must not be told a request no buffer could serve was one.
  const auto initial = withUnion(kInteger);
  const auto notStored = std::vector<uint32_t>{fieldOffset("b")};
  const auto refused = std::vector<uint32_t>{fieldOffset("number"), kNone};
  for (const auto& order :
       {std::vector{notStored, refused}, std::vector{refused, notStored}}) {
    flatbuffers::FlatBufferBuilder requestBuilder;
    auto types = std::vector<serialized::Operation>{};
    auto values = std::vector<flatbuffers::Offset<void>>{};
    for (const auto& path : order) {
      types.push_back(serialized::Operation::Delete);
      values.push_back(serialized::CreateDelete(
                           requestBuilder, requestBuilder.CreateVector(path))
                           .Union());
    }
    const auto patch = serialized::CreatePatch(
        requestBuilder, requestBuilder.CreateVector(std::vector<uint32_t>{}),
        requestBuilder.CreateVector(types),
        requestBuilder.CreateVector(values));
    requestBuilder.Finish(serialized::CreateRequest(
        requestBuilder, serialized::Operation::Patch, patch.Union()));
    EXPECT_EQ(outcomeOfRequest(initial, requestBuilder.GetBufferPointer()),
              ApplyOutcome::Refused);
  }
}

TEST_F(HandBuiltTest, RefusesADeleteOfAnElementOfAnArrayOfStructs) {
  const auto initial =
      GetBuffer(R"({"nested": {"inner": {"a": 1, "arr": [0,0,0]},)"
                R"( "structs": [{"a": 8, "arr": [0,0,0]},)"
                R"( {"a": 9, "arr": [0,0,0]}], "tail": 3}})");
  const auto structs = schema_->objects()
                           ->LookupByKey("matcher_test.Nested")
                           ->fields()
                           ->LookupByKey("structs")
                           ->offset();
  EXPECT_EQ(deleteOutcome(initial, {fieldOffset("nested"), structs, 1}),
            ApplyOutcome::Refused);
}

TEST_F(HandBuiltTest, AddsNothingToABuilderThatAlreadyHoldsData) {
  // The caller's builder may hold its own data, which an apply that is not
  // Applied leaves exactly as it was.
  const auto initial = GetBuffer(R"({"i32": 1})");
  flatbuffers::FlatBufferBuilder requestBuilder;
  requestBuilder.Finish(serialized::CreateRequest(
      requestBuilder, serialized::Operation::Put,
      serialized::CreatePut(requestBuilder,
                            requestBuilder.CreateVector(std::vector<uint32_t>{
                                fieldOffset("number"), kNone}),
                            requestBuilder.CreateVector(std::vector<uint8_t>{}))
          .Union()));
  const auto notStored = parseFlatbufferRequest(schema_, "delete i8");
  ASSERT_TRUE(notStored.has_value());
  for (const auto* request : std::array<const uint8_t*, 2>{
           notStored->data(), requestBuilder.GetBufferPointer()}) {
    flatbuffers::FlatBufferBuilder fbb;
    fbb.CreateString("the caller's own");
    const auto before = fbb.GetSize();
    const auto result =
        tryApplyRequest(fbb, schema_, flatbuffers::GetAnyRoot(initial.data()),
                        flatbuffers::GetRoot<serialized::Request>(request));
    EXPECT_NE(result.outcome, ApplyOutcome::Applied);
    EXPECT_EQ(fbb.GetSize(), before);
  }
}

// A struct member that is itself a struct, and an array whose elements are
// structs.
class PayloadAlignmentTest : public BufferRequestTest {
 protected:
  /// Applies `request` to an empty table with the request buffer moved to
  /// `shift` bytes past an 8-byte boundary, which moves its payload with it.
  ///
  /// @param request The request text to parse and apply.
  /// @param shift Bytes past an 8-byte boundary to place the request at; a
  ///     multiple of 4, which is all a request buffer itself needs.
  /// @return The verified result of the apply.
  flatbuffers::DetachedBuffer applyShifted(const char* request, size_t shift) {
    auto parsed = parseFlatbufferRequest(schema_, request);
    const auto initial = GetBuffer("{}");
    flatbuffers::FlatBufferBuilder fbb;
    if (not parsed.has_value()) {
      ADD_FAILURE() << "Failed to parse request: " << request;
      copyTable(fbb, schema_, initial.data(), false);
      return fbb.Release();
    }
    auto words = std::vector<uint64_t>((parsed->size() + shift + 7) / 8);
    auto* shifted = reinterpret_cast<uint8_t*>(words.data()) + shift;
    std::memcpy(shifted, parsed->data(), parsed->size());

    finishApplied(fbb, schema_, flatbuffers::GetAnyRoot(initial.data()),
                  flatbuffers::GetRoot<serialized::Request>(shifted));
    auto result = fbb.Release();
    auto verifier = flatbuffers::Verifier(result.data(), result.size());
    EXPECT_TRUE(matcher_test::VerifyTestSchemaBuffer(verifier)) << request;
    return result;
  }
};

TEST_F(PayloadAlignmentTest, ReadsATablePayloadAtEitherFourByteOffset) {
  for (const size_t shift : {0, 4}) {
    auto result = applyShifted(
        R"(put object {"i64": 9223372036854775807, "f64": 0.1})", shift);
    const auto* ts = matcher_test::GetTestSchema(result.data());
    ASSERT_NE(ts->object(), nullptr) << shift;
    EXPECT_EQ(ts->object()->i64(), 9223372036854775807) << shift;
    EXPECT_EQ(ts->object()->f64(), 0.1) << shift;
  }
}

TEST_F(PayloadAlignmentTest, ReadsAFlexbufferPayloadAtEitherFourByteOffset) {
  for (const size_t shift : {0, 4}) {
    auto result = applyShifted("put f64 0.1", shift);
    EXPECT_EQ(matcher_test::GetTestSchema(result.data())->f64(), 0.1) << shift;
  }
}

TEST_F(PayloadAlignmentTest, ReadsEveryTableOfAVectorPayload) {
  for (const size_t shift : {0, 4}) {
    auto result = applyShifted(
        R"(put object_vector [{"i8": 1}, {"i64": 9223372036854775807}, )"
        R"({"i8": 2, "f64": 0.1}])",
        shift);
    const auto* vector =
        matcher_test::GetTestSchema(result.data())->object_vector();
    ASSERT_NE(vector, nullptr) << shift;
    ASSERT_EQ(vector->size(), 3U) << shift;
    EXPECT_EQ(vector->Get(0)->i8(), 1) << shift;
    EXPECT_EQ(vector->Get(1)->i64(), 9223372036854775807) << shift;
    EXPECT_EQ(vector->Get(2)->i8(), 2) << shift;
    EXPECT_EQ(vector->Get(2)->f64(), 0.1) << shift;
  }
}

class UnterminatedRequestTest : public BufferRequestTest {};

// Each view stops short of text that, read on to the terminator, would parse
// to something else.
TEST_F(UnterminatedRequestTest, ParsesOnlyTheViewOfAScalarPut) {
  const auto text = std::string_view{"put i32 12"};
  auto result = applyRequestTo(GetBuffer("{}"), text.substr(0, 9));
  EXPECT_EQ(matcher_test::GetTestSchema(result.data())->i32(), 1);
}

TEST_F(UnterminatedRequestTest, RefusesATablePutTheViewCutsShort) {
  const auto text = std::string_view{R"(put object {"i32": 7})"};
  EXPECT_FALSE(parseFlatbufferRequest(schema_, text.substr(0, text.size() - 1))
                   .has_value());
}

TEST_F(UnterminatedRequestTest, ReadsNoFurtherThanAHeapViewEnds) {
  // Sized exactly, so ASan reports a read past the view's last byte.
  const auto text = std::string_view{"put i8 [1"};
  const auto bytes = std::vector<char>(text.begin(), text.end());
  EXPECT_FALSE(parseFlatbufferRequest(
                   schema_, std::string_view{bytes.data(), bytes.size()})
                   .has_value());
}

class NestedStructTest : public BufferRequestTest {};

TEST_F(NestedStructTest, PutsAMemberOfAStructInsideAStruct) {
  auto result =
      applyRequestTo(GetBuffer(R"({"i32": 10})"), "put nested.inner.a 3");
  const auto* ts = matcher_test::GetTestSchema(result.data());
  ASSERT_NE(ts->nested(), nullptr);
  EXPECT_EQ(ts->nested()->inner().a(), 3);
  EXPECT_EQ(ts->nested()->tail(), 0);
}

TEST_F(NestedStructTest, PutsAnArrayElementInsideANestedStruct) {
  auto result =
      applyRequestTo(GetBuffer(R"({"i32": 10})"), "put nested.inner.arr[1] 55");
  const auto* ts = matcher_test::GetTestSchema(result.data());
  ASSERT_NE(ts->nested(), nullptr);
  EXPECT_EQ(ts->nested()->inner().arr()->Get(1), 55);
  EXPECT_EQ(ts->nested()->inner().arr()->Get(0), 0);
}

TEST_F(NestedStructTest, PutsAMemberAfterANestedStructWithoutDisturbingIt) {
  auto result = applyRequestTo(GetBuffer(R"({"i32": 10})"),
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

// A struct put writes what its payload names, or is refused: one that would
// write nothing, or drop part of what it was given, is not served.
TEST_F(StructPutTest, RefusesAStructPayloadThatWritesNothing) {
  for (const auto* request : {
           "put scalars {}",
           R"(put scalars {"zzz": 2})",
           "put scalars.i32_array []",
           "put nested.inner {}",
           R"(put nested {"inner": {}})",
           R"(put nested {"inner": {"arr": []}})",
           "patch { put scalars {} }",
       }) {
    EXPECT_FALSE(parses(request)) << request;
  }
}

TEST_F(StructPutTest, RefusesAMapNamingAMemberTheStructDoesNotDeclare) {
  // An unknown key is a misspelt member, which the caller should hear about
  // rather than have dropped.
  EXPECT_FALSE(parses(R"(put scalars {"i16": 40, "zzz": 1})"));
  EXPECT_FALSE(parses(R"(put scalars {"zzz": 1, "i16": 40})"));
  EXPECT_FALSE(parses(R"(put nested {"inner": {"a": 1, "zzz": 2}})"));
  EXPECT_TRUE(parses(R"(put scalars {"i16": 40})"));
}

TEST_F(StructPutTest, RefusesAnArrayLongerThanItsDeclaredLength) {
  EXPECT_FALSE(parses("put scalars.i32_array [1,2,3,4,5,6,7,8,9,10,11]"));
  EXPECT_FALSE(parses("put nested.inner.arr [1,2,3,4]"));
  EXPECT_FALSE(parses(R"(put nested {"inner": {"arr": [1,2,3,4]}})"));
  EXPECT_TRUE(parses("put scalars.i32_array [1,2,3,4,5,6,7,8,9,10]"));
}

TEST_F(StructPutTest, RefusesAMapWhoseMemberValueWritesNothing) {
  // The members beside the empty one are not written on their own.
  for (const auto* request : {
           R"(put nested {"inner": {}, "tail": 77})",
           R"(put nested {"inner": {"arr": []}, "tail": 77})",
           R"(put nested {"inner": {"arr": [1,2,3,4]}, "tail": 77})",
           R"(put nested {"inner": {"zzz": 1}, "tail": 77})",
       }) {
    EXPECT_FALSE(parses(request)) << request;
  }
}

TEST_F(StructPutTest, RefusesAnArrayOfStructsNamedInAMap) {
  // The stride for a struct element is its byte size, which `GetTypeSize`
  // does not answer, so nothing writes one.
  EXPECT_FALSE(parses(R"(put nested {"structs": [{"a": 2}]})"));
  EXPECT_FALSE(parses(R"(put nested {"structs": [{"a": 2}], "tail": 3})"));
}

// A member named in a MAP is checked by the struct it is in: the payload is
// parsed before anything says which key goes to which type.
TEST_F(StructPutTest, RefusesAWrongKindValueForAScalarMemberInAMap) {
  for (const auto* request :
       {R"(put scalars {"i8": [1,2,3]})", R"(put scalars {"i8": "hello"})",
        R"(put scalars {"i8": "7"})", R"(put scalars {"i8": {"q": 1}})",
        R"(put nested {"inner": 5})", R"(put nested {"inner": {"arr": 5}})",
        R"(put nested {"inner": {"arr": {"a": 1}}})"}) {
    EXPECT_FALSE(parses(request)) << request;
  }
}

TEST_F(StructPutTest, StillTakesEveryScalarKindForAMemberInAMap) {
  // The check must not refuse the kinds a scalar member does take.
  auto result = applyRequestTo(
      GetBuffer(R"({"i32": 10})"),
      R"(put scalars {"b": true, "i8": 3, "f64": 1.5, "ui64": 9})");
  const auto* ts = matcher_test::GetTestSchema(result.data());
  ASSERT_NE(ts->scalars(), nullptr);
  EXPECT_TRUE(ts->scalars()->b());
  EXPECT_EQ(ts->scalars()->i8(), 3);
  EXPECT_DOUBLE_EQ(ts->scalars()->f64(), 1.5);
  EXPECT_EQ(ts->scalars()->ui64(), 9U);
}

// A path is resolved to the field, element or member its text names, or the
// request is refused: never to another element or member, nor to a field the
// apply does not write.
class PathResolutionTest : public BufferRequestTest {
 protected:
  /// A buffer storing `i32` as 7 and `i32_vector` as 0, 1, ..., `size` - 1.
  ///
  /// @param size Number of elements in the vector.
  std::vector<uint8_t> withCountingVector(size_t size) {
    auto json = std::string{R"({"i32": 7, "i32_vector": [)"};
    for (size_t i = 0; i < size; i++) {
      json += (i == 0 ? "" : ",") + std::to_string(i);
    }
    return GetBuffer(json + "]}");
  }
};

TEST_F(PathResolutionTest, PutsAnElementAboveSixteenBits) {
  auto result = applyRequestTo(GetBuffer(R"({"i32_vector": [1, 2]})"),
                               "put i32_vector[70000] 5");
  const auto* vector = matcher_test::GetTestSchema(result.data())->i32_vector();
  ASSERT_NE(vector, nullptr);
  ASSERT_EQ(vector->size(), 70001U);
  EXPECT_EQ(vector->Get(70000), 5);
  // 70000 truncated to 16 bits, which must stay untouched.
  EXPECT_EQ(vector->Get(4464), 0);
  EXPECT_EQ(vector->Get(1), 2);
}

TEST_F(PathResolutionTest, PutsAFieldOfAnElementAboveSixteenBits) {
  auto result =
      applyRequestTo(GetBuffer("{}"), "put object_vector[65537].i8 9");
  const auto* vector =
      matcher_test::GetTestSchema(result.data())->object_vector();
  ASSERT_NE(vector, nullptr);
  ASSERT_EQ(vector->size(), 65538U);
  EXPECT_EQ(vector->Get(65537)->i8(), 9);
  EXPECT_EQ(vector->Get(1)->i8(), 0);
}

TEST_F(PathResolutionTest, DeletesAnElementAboveSixteenBits) {
  auto result =
      applyRequestTo(withCountingVector(65540), "delete i32_vector[65537]");
  const auto* vector = matcher_test::GetTestSchema(result.data())->i32_vector();
  ASSERT_NE(vector, nullptr);
  ASSERT_EQ(vector->size(), 65539U);
  EXPECT_EQ(vector->Get(1), 1);
  EXPECT_EQ(vector->Get(65536), 65536);
  EXPECT_EQ(vector->Get(65537), 65538);
}

TEST_F(PathResolutionTest, RefusesAnIndexThatIsNotANonNegativeInteger) {
  // Unsigned, or a negative index would wrap to the top of the range.
  EXPECT_FALSE(parses("put i32_vector[-1] 5"));
  EXPECT_FALSE(parses("delete i32_vector[-1]"));
  EXPECT_FALSE(parses("put i32_vector[+1] 5"));
  EXPECT_FALSE(parses("put i32_vector[] 5"));
  EXPECT_FALSE(parses("put i32_vector[4294967296] 5"));
}

TEST_F(PathResolutionTest, RefusesAPutGrowingAVectorPastWhatAFlatbufferHolds) {
  // Each vector would need more bytes than a flatbuffer can address.
  EXPECT_FALSE(parses("put i32_vector[600000000] 5"));
  EXPECT_FALSE(parses(R"(put string_vector[200000000] "a")"));
  EXPECT_FALSE(parses("put object_vector[4000000000].i8 1"));
  // Exactly the most a flatbuffer holds as four-byte elements, with no room
  // for the table around them.
  EXPECT_FALSE(parses("put i32_vector[536870910] 5"));
  EXPECT_TRUE(parses("put i32_vector[100000000] 5"));
  // Removing one asks for nothing to be built.
  EXPECT_TRUE(parses("delete i32_vector[4000000000]"));
}

TEST_F(PathResolutionTest, PutsASparseUnionMember) {
  auto result =
      applyRequestTo(GetBuffer("{}"), R"(put sparse.Integer {"value": 9})");
  const auto* ts = matcher_test::GetTestSchema(result.data());
  ASSERT_EQ(ts->sparse_type(), matcher_test::Sparse::Integer);
  ASSERT_NE(ts->sparse_as_Integer(), nullptr);
  EXPECT_EQ(ts->sparse_as_Integer()->value(), 9);
}

TEST_F(PathResolutionTest, PutsAFieldOfAStoredSparseUnionMember) {
  auto result =
      applyRequestTo(GetBuffer(R"({"sparse_type": "Fraction", "sparse": )"
                               R"({"numerator": 1, "denominator": 2}})"),
                     "put sparse.Fraction.denominator 4");
  const auto* ts = matcher_test::GetTestSchema(result.data());
  ASSERT_EQ(ts->sparse_type(), matcher_test::Sparse::Fraction);
  ASSERT_NE(ts->sparse_as_Fraction(), nullptr);
  EXPECT_EQ(ts->sparse_as_Fraction()->numerator(), 1);
  EXPECT_EQ(ts->sparse_as_Fraction()->denominator(), 4);
}

TEST_F(PathResolutionTest, SwitchesASparseUnionMember) {
  auto result =
      applyRequestTo(GetBuffer(R"({"sparse_type": "Fraction", "sparse": )"
                               R"({"numerator": 1, "denominator": 2}})"),
                     "put sparse.Integer.value 9");
  const auto* ts = matcher_test::GetTestSchema(result.data());
  ASSERT_EQ(ts->sparse_type(), matcher_test::Sparse::Integer);
  ASSERT_NE(ts->sparse_as_Integer(), nullptr);
  EXPECT_EQ(ts->sparse_as_Integer()->value(), 9);
}

TEST_F(PathResolutionTest, DeletesThroughASparseUnionMember) {
  const auto initial = GetBuffer(R"({"i32": 7, "sparse_type": "Fraction", )"
                                 R"("sparse": {"numerator": 1, )"
                                 R"("denominator": 2}})");
  auto result = applyRequestTo(initial, "delete sparse.Fraction.numerator");
  const auto* ts = matcher_test::GetTestSchema(result.data());
  ASSERT_EQ(ts->sparse_type(), matcher_test::Sparse::Fraction);
  ASSERT_NE(ts->sparse_as_Fraction(), nullptr);
  EXPECT_EQ(ts->sparse_as_Fraction()->numerator(), 0);
  EXPECT_EQ(ts->sparse_as_Fraction()->denominator(), 2);

  result = applyRequestTo(initial, "delete sparse.Fraction");
  ts = matcher_test::GetTestSchema(result.data());
  EXPECT_EQ(ts->sparse_type(), matcher_test::Sparse::NONE);
  EXPECT_EQ(ts->sparse(), nullptr);
  EXPECT_EQ(ts->i32(), 7);
}

TEST_F(PathResolutionTest, PadsASparseUnionVectorWithItsFirstMember) {
  auto result =
      applyRequestTo(GetBuffer("{}"), "put sparse_vector[1].Integer.value 7");
  const auto* ts = matcher_test::GetTestSchema(result.data());
  ASSERT_NE(ts->sparse_vector_type(), nullptr);
  ASSERT_NE(ts->sparse_vector(), nullptr);
  ASSERT_EQ(ts->sparse_vector_type()->size(), 2U);
  EXPECT_EQ(ts->sparse_vector_type()->Get(0), matcher_test::Sparse::Fraction);
  EXPECT_EQ(ts->sparse_vector_type()->Get(1), matcher_test::Sparse::Integer);
  EXPECT_EQ(ts->sparse_vector()->GetAs<matcher_test::Integer>(1)->value(), 7);
}

TEST_F(PathResolutionTest, EditsAndDeletesSparseUnionVectorElements) {
  const auto initial =
      GetBuffer(R"({"sparse_vector_type": ["Integer", "Fraction"], )"
                R"("sparse_vector": [{"value": 1}, )"
                R"({"numerator": 3, "denominator": 4}]})");
  auto result =
      applyRequestTo(initial, "put sparse_vector[1].Fraction.numerator 8");
  const auto* ts = matcher_test::GetTestSchema(result.data());
  ASSERT_EQ(ts->sparse_vector_type()->Get(1), matcher_test::Sparse::Fraction);
  EXPECT_EQ(ts->sparse_vector()->GetAs<matcher_test::Fraction>(1)->numerator(),
            8);
  EXPECT_EQ(
      ts->sparse_vector()->GetAs<matcher_test::Fraction>(1)->denominator(), 4);
  EXPECT_EQ(ts->sparse_vector()->GetAs<matcher_test::Integer>(0)->value(), 1);

  result = applyRequestTo(initial, "delete sparse_vector[0].Integer.value");
  ts = matcher_test::GetTestSchema(result.data());
  ASSERT_EQ(ts->sparse_vector_type()->Get(0), matcher_test::Sparse::Integer);
  EXPECT_EQ(ts->sparse_vector()->GetAs<matcher_test::Integer>(0)->value(), 0);

  result = applyRequestTo(initial, "delete sparse_vector[0].Integer");
  ts = matcher_test::GetTestSchema(result.data());
  ASSERT_EQ(ts->sparse_vector_type()->size(), 1U);
  ASSERT_EQ(ts->sparse_vector()->size(), 1U);
  EXPECT_EQ(ts->sparse_vector_type()->Get(0), matcher_test::Sparse::Fraction);
  EXPECT_EQ(ts->sparse_vector()->GetAs<matcher_test::Fraction>(0)->numerator(),
            3);
}

TEST_F(PathResolutionTest, RefusesAPathNamingADiscriminator) {
  // A union's discriminator is written only from the union beside it, so a
  // request naming one would write nothing.
  EXPECT_FALSE(parses("put number_type 2"));
  EXPECT_FALSE(parses("put number_vector_type [1]"));
  EXPECT_FALSE(parses("put number_vector_type[0] 2"));
  EXPECT_FALSE(parses("delete number_type"));
  EXPECT_FALSE(parses("delete number_vector_type"));
  EXPECT_FALSE(parses("delete number_vector_type[0]"));
  EXPECT_FALSE(parses("put object.number_type 2"));
  EXPECT_FALSE(parses("patch { put number_type 1 }"));
  EXPECT_FALSE(parses("patch object { delete number_type }"));
}

TEST_F(PathResolutionTest, RefusesAPutOrPatchStoppingAtAUnion) {
  // Nothing says which member a payload is.
  EXPECT_FALSE(parses(R"(put number {"value": 1})"));
  EXPECT_FALSE(parses(R"(put number_vector[0] {"value": 1})"));
  EXPECT_FALSE(parses("patch number { put value 1 }"));
  EXPECT_FALSE(parses("patch number_vector[0] { put value 1 }"));
}

TEST_F(PathResolutionTest, DeletesAWholeUnion) {
  const auto initial = GetBuffer(
      R"({"i32": 7, "number_type": "Integer", "number": {"value": 5}})");
  auto result = applyRequestTo(initial, "delete number");
  const auto* ts = matcher_test::GetTestSchema(result.data());
  EXPECT_EQ(ts->number_type(), matcher_test::Number::NONE);
  EXPECT_EQ(ts->number(), nullptr);
  EXPECT_EQ(ts->i32(), 7);

  EXPECT_EQ(outcomeOf(GetBuffer(R"({"i32": 7})"), "delete number"),
            ApplyOutcome::NotStored);
}

TEST_F(PathResolutionTest, DeletesAWholeUnionVectorElement) {
  auto result = applyRequestTo(
      GetBuffer(R"({"number_vector_type": ["Integer", "Fraction"], )"
                R"("number_vector": [{"value": 1}, {"numerator": 3}]})"),
      "delete number_vector[0]");
  const auto* ts = matcher_test::GetTestSchema(result.data());
  ASSERT_NE(ts->number_vector_type(), nullptr);
  ASSERT_EQ(ts->number_vector_type()->size(), 1U);
  ASSERT_EQ(ts->number_vector()->size(), 1U);
  EXPECT_EQ(ts->number_vector_type()->Get(0), matcher_test::Number::Fraction);
  EXPECT_EQ(ts->number_vector()->GetAs<matcher_test::Fraction>(0)->numerator(),
            3);
}

TEST_F(PathResolutionTest, FindsNothingStoredForADeleteNamingAnotherMember) {
  // The text names a Fraction the buffer does not hold, so there is nothing
  // to remove; the Integer it does hold is not it.
  const auto initial = GetBuffer(
      R"({"number_type": "Integer", "number": {"value": 5}, )"
      R"("number_vector_type": ["Integer"], "number_vector": [{"value": 6}]})");
  for (const auto* request :
       {"delete number.Fraction", "delete number.Fraction.numerator",
        "delete number_vector[0].Fraction",
        "delete number_vector[0].Fraction.numerator"}) {
    EXPECT_EQ(outcomeOf(initial, request), ApplyOutcome::NotStored) << request;
  }
}

TEST_F(PathResolutionTest, RefusesAnIndexPastAFixedLengthArray) {
  // An array cannot grow, so the element named does not exist.
  EXPECT_FALSE(parses("put scalars.i32_array[10] 1"));
  EXPECT_FALSE(parses("put scalars.i32_array[99] 1"));
  EXPECT_FALSE(parses("put nested.inner.arr[3] 1"));
  EXPECT_FALSE(parses("delete scalars.i32_array[99]"));
  EXPECT_TRUE(parses("put scalars.i32_array[9] 1"));
}

TEST_F(PathResolutionTest, RefusesAnElementOfAnArrayOfStructs) {
  EXPECT_FALSE(parses("put nested.structs[0].a 2"));
  EXPECT_FALSE(parses(R"(put nested.structs[0] {"a": 2})"));
  EXPECT_FALSE(parses(R"(put nested.structs [{"a": 2}])"));
}

TEST_F(PathResolutionTest, RefusesAPathContinuingPastAWholeVector) {
  // A whole vector of tables or structs names its element type for the
  // payload, but a field after it would be read as an element index.
  EXPECT_FALSE(parses("put object_vector.i8 5"));
  EXPECT_FALSE(parses("delete object_vector.i8"));
  EXPECT_FALSE(parses("delete struct_vector.i8"));
  EXPECT_TRUE(parses("delete object_vector[0].i8"));
}

TEST_F(PathResolutionTest, RefusesAnIndexIntoWhatIsNotAVectorOrArray) {
  // A bracket left after the path would be parsed as the payload.
  EXPECT_FALSE(parses("put i8[0] 1"));
  EXPECT_FALSE(parses("delete i8[0]"));
  EXPECT_FALSE(parses("put i32_vector[0][1] 5"));
  EXPECT_FALSE(parses("put object[0].i8 5"));
}

TEST_F(PathResolutionTest, RefusesTextAfterTheRequest) {
  // Only the first operation is applied, so anything after it is refused.
  EXPECT_FALSE(parses("delete i8 i16"));
  EXPECT_FALSE(parses("put i8 5 6"));
  EXPECT_FALSE(parses("patch { put i8 1 } delete i16"));
  EXPECT_TRUE(parses("delete i8 "));
  EXPECT_TRUE(parses("put i8 5\n"));
}

// A union may hold a struct in a schema generated for C++ alone. The apply
// reads every member as a table, so a path into one is refused.
TEST(UnionStructMemberTest, RefusesAPathIntoAStructMember) {
  auto options = flatbuffers::IDLOptions{};
  options.lang_to_generate = flatbuffers::IDLOptions::kCpp;
  flatbuffers::Parser parser{options};
  ASSERT_TRUE(parser.Parse(R"fbs(
    table T { x: int32; }
    struct S { a: int32; }
    union U { T, S }
    table Root { u: U; }
    root_type Root;
  )fbs"))
      << parser.error_;
  parser.Serialize();
  const auto* schema =
      reflection::GetSchema(parser.builder_.GetBufferPointer());
  EXPECT_FALSE(parseFlatbufferRequest(schema, R"(put u.S {"a": 1})"));
  EXPECT_FALSE(parseFlatbufferRequest(schema, "put u.S.a 1"));
  EXPECT_FALSE(parseFlatbufferRequest(schema, "delete u.S.a"));
  EXPECT_TRUE(parseFlatbufferRequest(schema, "put u.T.x 1"));
  EXPECT_TRUE(parseFlatbufferRequest(schema, "delete u"));
}

// A stored struct member is copied as its bytes, not read as a table.
TEST(UnionStructMemberTest, KeepsAStoredStructMember) {
  auto options = flatbuffers::IDLOptions{};
  options.lang_to_generate = flatbuffers::IDLOptions::kCpp;
  flatbuffers::Parser parser{options};
  ASSERT_TRUE(parser.Parse(R"fbs(
    table T { x: int32; }
    struct S { a: int32; b: int64; }
    union U { T, S }
    table Root { u: U; i: int32; us: [U]; }
    root_type Root;
  )fbs"))
      << parser.error_;
  parser.Serialize();
  const auto bfbs = std::vector<uint8_t>(
      parser.builder_.GetBufferPointer(),
      parser.builder_.GetBufferPointer() + parser.builder_.GetSize());
  const auto* schema = reflection::GetSchema(bfbs.data());
  ASSERT_TRUE(parser.Parse(
      R"json({"u_type": "S", "u": {"a": 5, "b": 6}, "i": 1,
              "us_type": ["S", "T"], "us": [{"a": 7, "b": 8}, {"x": 9}]})json"))
      << parser.error_;
  const auto initial = std::vector<uint8_t>(
      parser.builder_.GetBufferPointer(),
      parser.builder_.GetBufferPointer() + parser.builder_.GetSize());

  for (const auto* text : {"put i 2", "delete i", "delete us[1].T.x"}) {
    auto request = parseFlatbufferRequest(schema, text);
    ASSERT_TRUE(request.has_value()) << text;
    flatbuffers::FlatBufferBuilder fbb;
    finishApplied(fbb, schema, flatbuffers::GetAnyRoot(initial.data()),
                  flatbuffers::GetRoot<serialized::Request>(request->data()));
    const auto& root = *flatbuffers::GetAnyRoot(fbb.GetBufferPointer());
    const auto* rootDef = schema->root_table();
    const auto* u = rootDef->fields()->LookupByKey("u");
    const auto* member = flatbuffers::GetFieldT(root, *u);
    ASSERT_NE(member, nullptr) << text;
    const auto* bytes = reinterpret_cast<const uint8_t*>(member);
    int32_t a = 0;
    int64_t b = 0;
    std::memcpy(&a, bytes, sizeof(a));
    std::memcpy(&b, bytes + 8, sizeof(b));
    EXPECT_EQ(a, 5) << text;
    EXPECT_EQ(b, 6) << text;

    const auto* us = rootDef->fields()->LookupByKey("us");
    const auto* elements = flatbuffers::GetFieldAnyV(root, *us);
    ASSERT_NE(elements, nullptr) << text;
    ASSERT_EQ(elements->size(), 2U) << text;
    const auto* first = reinterpret_cast<const uint8_t*>(
        flatbuffers::GetAnyVectorElemPointer<const flatbuffers::Table>(elements,
                                                                       0));
    std::memcpy(&a, first, sizeof(a));
    std::memcpy(&b, first + 8, sizeof(b));
    EXPECT_EQ(a, 7) << text;
    EXPECT_EQ(b, 8) << text;
  }
}

TEST_F(PathResolutionTest, RefusesAPatchOfWhatAPutCannotWrite) {
  // A patch's operations are puts as well as deletes, and its path is the
  // prefix of theirs, so it takes only a path a put could: one stopping at a
  // whole vector would have each operation's first step read as an index.
  EXPECT_FALSE(parses("patch struct_vector { put i8 1 }"));
  EXPECT_FALSE(parses("patch object_vector { put i8 1 }"));
  EXPECT_FALSE(parses("patch object_vector { delete i8 }"));
  EXPECT_FALSE(parses("patch struct_vector[0] { put i8 1 }"));
  EXPECT_FALSE(parses("patch object_vector[4000000000] { put i8 1 }"));
  EXPECT_TRUE(parses("patch object_vector[3] { put i8 1 }"));
}

// A scalar's value is checked against the scalar it is written as, wherever
// the scalar is: a field, an element of a vector, a member of a struct, an
// element of an array in one, a member named in a struct's map, or an
// element of a whole vector or array.
class ScalarValueTest : public BufferRequestTest {
 protected:
  // Every request writing `value` as the scalar `field` names, at each of the
  // places a scalar is written. `field` is a scalar field of the root table
  // that `ScalarStruct` declares too, and the vector `<field>_vector` holds.
  static std::vector<std::string> everywhere(const std::string& field,
                                             const std::string& value) {
    return {
        "put " + field + " " + value,
        "put " + field + "_vector[0] " + value,
        "put " + field + "_vector[2] " + value,
        "put " + field + "_vector [" + value + "]",
        "put " + field + "_vector [0, " + value + "]",
        "put scalars." + field + " " + value,
        "put scalars {\"" + field + "\": " + value + "}",
        "put object." + field + " " + value,
        "patch { put " + field + " " + value + " }",
    };
  }

  // The requests writing `value` into an element of a fixed-length array of
  // int32, which `ScalarStruct` holds as `i32_array`.
  static std::vector<std::string> intoArray(const std::string& value) {
    return {
        "put scalars.i32_array[1] " + value,
        "put scalars.i32_array [" + value + "]",
        "put scalars {\"i32_array\": [1, " + value + "]}",
    };
  }

  void expectRefusedEverywhere(const std::string& field,
                               const std::string& value) {
    for (const auto& request : everywhere(field, value)) {
      EXPECT_FALSE(parses(request)) << request;
    }
  }

  void expectAcceptedEverywhere(const std::string& field,
                                const std::string& value) {
    for (const auto& request : everywhere(field, value)) {
      EXPECT_TRUE(parses(request)) << request;
    }
  }
};

TEST_F(ScalarValueTest, RefusesAValueOfTheWrongKind) {
  for (const auto* field : {"b", "i8", "ui8", "i32", "ui64", "f32", "f64"}) {
    for (const auto* value :
         {"[5]", "[]", R"({"a": 1})", "{}", R"("5")", "null", "nope"}) {
      expectRefusedEverywhere(field, value);
    }
  }
  for (const auto* value : {"[5]", R"({"a": 1})", R"("5")", "true", "1.5"}) {
    for (const auto& request : intoArray(value)) {
      EXPECT_FALSE(parses(request)) << request;
    }
  }
  // An integer takes a whole numeral: not a fraction, an exponent or a bool.
  for (const auto* field : {"i8", "ui16", "i32", "ui64"}) {
    for (const auto* value : {"1.5", "1.0", "1e2", "true", "false"}) {
      expectRefusedEverywhere(field, value);
    }
  }
}

TEST_F(ScalarValueTest, WritesNotAVectorsLengthForAScalar) {
  // A one-element list on a scalar read as the list's length.
  EXPECT_FALSE(parses("put i8 [5]"));
  EXPECT_FALSE(parses("put i8_vector[0] [5, 6]"));
}

TEST_F(ScalarValueTest, RefusesAnIntegerOutOfRangeForItsWidth) {
  const auto cases = std::vector<std::pair<std::string, std::string>>{
      {"i8", "128"},
      {"i8", "-129"},
      {"i8", "300"},
      {"ui8", "256"},
      {"ui8", "-1"},
      {"i16", "32768"},
      {"i16", "-32769"},
      {"ui16", "65536"},
      {"ui16", "-1"},
      {"i32", "2147483648"},
      {"i32", "-2147483649"},
      {"ui32", "4294967296"},
      {"ui32", "-1"},
      {"i64", "9223372036854775808"},
      {"i64", "-9223372036854775809"},
      {"i64", "99999999999999999999999"},
      {"ui64", "18446744073709551616"},
      {"ui64", "-1"},
  };
  for (const auto& [field, value] : cases) {
    expectRefusedEverywhere(field, value);
  }
  for (const auto& request : intoArray("2147483648")) {
    EXPECT_FALSE(parses(request)) << request;
  }
}

TEST_F(ScalarValueTest, WritesEveryIntegerAtTheEdgesOfItsWidth) {
  const auto cases = std::vector<std::pair<std::string, std::string>>{
      {"i8", "-128"},
      {"i8", "127"},
      {"ui8", "255"},
      {"i16", "-32768"},
      {"ui16", "65535"},
      {"i32", "-2147483648"},
      {"ui32", "4294967295"},
      {"i64", "-9223372036854775808"},
      {"i64", "9223372036854775807"},
      {"ui64", "18446744073709551615"},
      {"ui64", "0"},
  };
  for (const auto& [field, value] : cases) {
    expectAcceptedEverywhere(field, value);
  }
  auto result = applyRequestTo(GetBuffer("{}"),
                               R"(patch { put i8 -128 ; put ui8 255 ; )"
                               R"(put i16 -32768 ; put ui32 4294967295 ; )"
                               R"(put i64 -9223372036854775808 })");
  const auto* ts = matcher_test::GetTestSchema(result.data());
  EXPECT_EQ(ts->i8(), -128);
  EXPECT_EQ(ts->ui8(), 255);
  EXPECT_EQ(ts->i16(), -32768);
  EXPECT_EQ(ts->ui32(), 4294967295U);
  EXPECT_EQ(ts->i64(), INT64_MIN);
}

TEST_F(ScalarValueTest, WritesTheWholeUnsigned64BitRange) {
  constexpr auto kTop = uint64_t{18446744073709551615U};
  constexpr auto kPastSigned = uint64_t{9223372036854775808U};
  // Each answer reads the last result, which the next call replaces.
  auto kept = flatbuffers::DetachedBuffer{};
  auto ts = [&](const char* request) {
    kept = applyRequestTo(GetBuffer(R"({"ui64_vector": [1, 2]})"), request);
    return matcher_test::GetTestSchema(kept.data());
  };
  EXPECT_EQ(ts("put ui64 18446744073709551615")->ui64(), kTop);
  EXPECT_EQ(ts("put ui64 9223372036854775808")->ui64(), kPastSigned);
  EXPECT_EQ(ts("put object.ui64 18446744073709551615")->object()->ui64(), kTop);
  EXPECT_EQ(ts("put scalars.ui64 18446744073709551615")->scalars()->ui64(),
            kTop);
  EXPECT_EQ(
      ts(R"(put scalars {"ui64": 18446744073709551615})")->scalars()->ui64(),
      kTop);
  EXPECT_EQ(
      ts("put ui64_vector[1] 18446744073709551615")->ui64_vector()->Get(1),
      kTop);
  EXPECT_EQ(
      ts("put ui64_vector[3] 18446744073709551615")->ui64_vector()->Get(3),
      kTop);
  const auto* whole =
      ts("put ui64_vector [18446744073709551615, 9223372036854775808, 5]")
          ->ui64_vector();
  ASSERT_EQ(whole->size(), 3U);
  EXPECT_EQ(whole->Get(0), kTop);
  EXPECT_EQ(whole->Get(1), kPastSigned);
  EXPECT_EQ(whole->Get(2), 5U);
  EXPECT_EQ(
      ts(R"(put object {"ui64": 18446744073709551615})")->object()->ui64(),
      kTop);
  EXPECT_EQ(ts(R"(put object_vector [{"ui64": 18446744073709551615}])")
                ->object_vector()
                ->Get(0)
                ->ui64(),
            kTop);
}

TEST_F(ScalarValueTest, RefusesTextAfterAValue) {
  for (const auto* request : {
           "put i32 5abc",
           "put i32 5 6",
           "put i32 5,",
           "put i32 5]",
           "put i32 0x10g",
           "put f32 1.5f",
           "put b truest",
           "put i32_vector [1] 2",
           "put i32_vector [1 2]",
           R"(put scalars {"i8": 1} 2)",
           R"(put scalars {"i8": 1 "i16": 2})",
           R"(put string "a" "b")",
           "patch { put i32 5abc }",
           "patch { put i32 5 6 }",
       }) {
    EXPECT_FALSE(parses(request)) << request;
  }
  EXPECT_TRUE(parses("patch { put i32 5 ; put i8 6 }"));
  EXPECT_TRUE(parses("put i32_vector [1, 2, ]"));
}

TEST_F(ScalarValueTest, ReadsAHexIntegerAgainstItsWidthToo) {
  auto result = applyRequestTo(GetBuffer("{}"),
                               "patch { put i32 0x10 ; put i8 -0x80 ; "
                               "put ui64 0xFFFFFFFFFFFFFFFF }");
  const auto* ts = matcher_test::GetTestSchema(result.data());
  EXPECT_EQ(ts->i32(), 16);
  EXPECT_EQ(ts->i8(), -128);
  EXPECT_EQ(ts->ui64(), uint64_t{18446744073709551615U});
  expectRefusedEverywhere("i8", "0x80");
  expectRefusedEverywhere("ui64", "0x10000000000000000");
  expectRefusedEverywhere("i32", "0x");
}

TEST_F(ScalarValueTest, RefusesAFiniteFloatPastWhatAFloat32Holds) {
  for (const auto* value : {"1e39", "-1e39", "3.5e38"}) {
    for (const auto& request : everywhere("f32", value)) {
      EXPECT_FALSE(parses(request)) << request;
    }
  }
  // A double literal past a double's range is no finite number either.
  expectRefusedEverywhere("f64", "1e309");
  // A numeral that rounds to the largest float32 is that float.
  auto result = applyRequestTo(GetBuffer("{}"), "put f32 3.4028235e38");
  EXPECT_EQ(matcher_test::GetTestSchema(result.data())->f32(),
            std::numeric_limits<float>::max());
  expectAcceptedEverywhere("f64", "1e300");
}

TEST_F(ScalarValueTest, WritesInfinitiesAndNotANumber) {
  for (const auto* field : {"f32", "f64"}) {
    for (const auto* value : {"inf", "-inf", "+inf", "infinity", "nan"}) {
      expectAcceptedEverywhere(field, value);
    }
  }
  auto result = applyRequestTo(GetBuffer("{}"),
                               "patch { put f32 -inf ; put f64 nan ; "
                               "put scalars.f32 inf }");
  const auto* ts = matcher_test::GetTestSchema(result.data());
  EXPECT_TRUE(std::isinf(ts->f32()) and ts->f32() < 0);
  EXPECT_TRUE(std::isnan(ts->f64()));
  EXPECT_TRUE(std::isinf(ts->scalars()->f32()));
  // An integer is a float's value too.
  EXPECT_EQ(matcher_test::GetTestSchema(
                applyRequestTo(GetBuffer("{}"), "put f32 3").data())
                ->f32(),
            3.0F);
  // And not for an integer.
  expectRefusedEverywhere("i32", "inf");
  expectRefusedEverywhere("i32", "nan");
}

TEST_F(ScalarValueTest, TakesTrueFalseZeroOrOneForABool) {
  for (const auto* value : {"true", "false", "0", "1"}) {
    expectAcceptedEverywhere("b", value);
  }
  for (const auto* value : {"2", "-1", "0.5", "1.0", R"("true")", "True"}) {
    expectRefusedEverywhere("b", value);
  }
  auto result =
      applyRequestTo(GetBuffer("{}"), "patch { put b 1 ; put scalars.b true }");
  const auto* ts = matcher_test::GetTestSchema(result.data());
  EXPECT_TRUE(ts->b());
  EXPECT_TRUE(ts->scalars()->b());
}

TEST_F(ScalarValueTest, ChecksAnEnumsOrdinalAgainstItsWidth) {
  // Color is a ubyte.
  EXPECT_FALSE(parses("put color 256"));
  EXPECT_FALSE(parses("put color -1"));
  EXPECT_FALSE(parses("put color_vector [4, 256]"));
  EXPECT_TRUE(parses("put color 255"));
}

TEST_F(ScalarValueTest, RefusesAWrongKindForAStringOrAVectorOfStrings) {
  for (const auto* request :
       {"put string 5", "put string [\"a\"]", "put string_vector [1]",
        "put string_vector[0] 5", "put string_vector \"a\"", "put i32_vector 5",
        R"(put i32_vector {"a": 1})"}) {
    EXPECT_FALSE(parses(request)) << request;
  }
}

TEST_F(ScalarValueTest, ReadsStringEscapes) {
  // Spelt with doubled backslashes: a raw string still reads a universal
  // character name.
  auto result = applyRequestTo(
      GetBuffer("{}"),
      "put string \"a\\\"b\\\\c\\n\\t\\u00e9\\ud83d\\ude00/\\/\"");
  const auto* string = matcher_test::GetTestSchema(result.data())->string();
  ASSERT_NE(string, nullptr);
  EXPECT_EQ(string->str(), "a\"b\\c\n\t\xc3\xa9\xf0\x9f\x98\x80//");
  EXPECT_FALSE(parses("put string \"\\ud83d\""));
  EXPECT_FALSE(parses("put string \"\\q\""));
  EXPECT_FALSE(parses("put string \"open"));
}

TEST_F(HandBuiltTest, RefusesAScalarPayloadOutOfItsFieldsRange) {
  const auto initial = withUnion(kInteger);
  EXPECT_EQ(putOutcome(initial, {fieldOffset("i8")}, flexPayload("300")),
            ApplyOutcome::Refused);
  EXPECT_EQ(putOutcome(initial, {fieldOffset("ui8")}, flexPayload("-1")),
            ApplyOutcome::Refused);
  EXPECT_EQ(
      putOutcome(initial, {fieldOffset("i8_vector")}, flexPayload("[1, 300]")),
      ApplyOutcome::Refused);
  EXPECT_EQ(putOutcome(initial, {fieldOffset("b")}, flexPayload("2")),
            ApplyOutcome::Refused);
  EXPECT_EQ(putOutcome(initial, {fieldOffset("f32")}, flexPayload("1e39")),
            ApplyOutcome::Refused);
  EXPECT_EQ(putOutcome(initial, {fieldOffset("i8")}, flexPayload("[5]")),
            ApplyOutcome::Refused);
  EXPECT_EQ(putOutcome(initial, {fieldOffset("i8")}, flexPayload("127")),
            ApplyOutcome::Applied);
}

TEST_F(ScalarValueTest, ChecksAValueAgainstTheArrayElementItIsWrittenAs) {
  constexpr auto kTop = uint64_t{18446744073709551615U};
  for (const auto* request : {
           "put wide.ui64_array[1] 18446744073709551615",
           "put wide.ui64_array [18446744073709551615, 9223372036854775808]",
           R"(put wide {"ui64_array": [18446744073709551615]})",
       }) {
    auto result = applyRequestTo(GetBuffer("{}"), request);
    const auto* wide = matcher_test::GetTestSchema(result.data())->wide();
    ASSERT_NE(wide, nullptr) << request;
    EXPECT_TRUE(wide->ui64_array()->Get(0) == kTop
                or wide->ui64_array()->Get(1) == kTop)
        << request;
  }
  for (const auto* request : {
           "put wide.ui64_array[0] 18446744073709551616",
           "put wide.ui64_array[0] -1",
           "put wide.ui64_array [1.5]",
           "put wide.b_array[0] 2",
           "put wide.b_array [true, 0.5]",
           R"(put wide {"b_array": ["true"]})",
           "put wide.f32_array[0] 1e39",
           "put wide.f32_array [1, [2]]",
           R"(put wide {"f32_array": [-1e39]})",
       }) {
    EXPECT_FALSE(parses(request)) << request;
  }
  for (const auto* request :
       {"put wide.b_array [true, 0]", "put wide.f32_array [inf, nan]",
        "put wide.f32_array[1] 3"}) {
    EXPECT_TRUE(parses(request)) << request;
  }
}

TEST_F(ScalarValueTest, WritesAnIntegerPastBoth64BitRangesAsAFloat) {
  // A double holds it, so a float takes it; an integer does not.
  auto result = applyRequestTo(GetBuffer("{}"),
                               "patch { put f64 18446744073709551616 ; "
                               "put f32 -9223372036854775809 }");
  const auto* ts = matcher_test::GetTestSchema(result.data());
  EXPECT_DOUBLE_EQ(ts->f64(), 18446744073709551616.0);
  EXPECT_FLOAT_EQ(ts->f32(), -9223372036854775809.0F);
  EXPECT_FALSE(parses("put ui64 18446744073709551616"));
  EXPECT_FALSE(parses("put f64 0x10000000000000000"));
}

TEST_F(ScalarValueTest, ReadsAFloatTooSmallForItsTypeAsTheNearestItHolds) {
  auto result = applyRequestTo(GetBuffer("{}"),
                               "patch { put f64 1e-400 ; put f32 1e-50 }");
  const auto* ts = matcher_test::GetTestSchema(result.data());
  EXPECT_EQ(ts->f64(), 0.0);
  EXPECT_EQ(ts->f32(), 0.0F);
}

// What the value reader takes, as the FlatBuffers JSON parser takes it.
TEST_F(ScalarValueTest, ReadsTheSpellingsAJsonValueMayTake) {
  auto result = applyRequestTo(
      GetBuffer("{}"),
      "patch { put i32 +5 ; put f32 .5 ; put f64 -.5e1 ; put ui8 0x0F ; "
      "put string 'it\\'s \\x41\\/' ; put scalars {i8: 3, 'i16': 4,} ; "
      "put i32_vector [\n 1 ,\t2\n] }");
  const auto* ts = matcher_test::GetTestSchema(result.data());
  EXPECT_EQ(ts->i32(), 5);
  EXPECT_EQ(ts->f32(), 0.5F);
  EXPECT_EQ(ts->f64(), -5.0);
  EXPECT_EQ(ts->ui8(), 15);
  ASSERT_NE(ts->string(), nullptr);
  EXPECT_EQ(ts->string()->str(), "it's A/");
  ASSERT_NE(ts->scalars(), nullptr);
  EXPECT_EQ(ts->scalars()->i8(), 3);
  EXPECT_EQ(ts->scalars()->i16(), 4);
  ASSERT_NE(ts->i32_vector(), nullptr);
  EXPECT_EQ(ts->i32_vector()->size(), 2U);
  // A trailing point makes a float, which an integer does not take.
  EXPECT_TRUE(parses("put f64 1."));
  EXPECT_FALSE(parses("put i16 1."));
}

TEST_F(ScalarValueTest, RefusesWhatIsNoValue) {
  for (const auto* request : {
           "put i32_vector [,]",
           "put i32_vector [1,,2]",
           R"(put scalars {"i8": 1, "i8": 2})",
           R"(put scalars {i8 1})",
           "put string \"\\udc00\"",
           "put string \"\\ud83d\\u0041\"",
           "put string \"a\tb\"",
           "put string \"\xff\"",
           "put string \"\xc3\"",
           "put i64 -0x8000000000000001",
           "put f64 1e",
           "put f64 1e+",
           "put f64 1..5",
           "put f64 -",
           "put f64 .",
       }) {
    EXPECT_FALSE(parses(request)) << request;
  }
  EXPECT_TRUE(parses("put i64 -0x8000000000000000"));
  EXPECT_TRUE(parses("put string \"\xc3\xa9\""));
  // Nesting past what the reader allows is refused rather than recursed into.
  EXPECT_FALSE(parses("put i32 " + std::string(100000, '[')));
}

TEST_F(HandBuiltTest, RefusesAScalarPayloadOfAnotherKind) {
  const auto initial = withUnion(kInteger);
  for (const auto* json : {"1.5", "true", "null"}) {
    EXPECT_EQ(putOutcome(initial, {fieldOffset("i32")}, flexPayload(json)),
              ApplyOutcome::Refused)
        << json;
  }
  // An unsigned payload past the signed range, which the FlatBuffers parser
  // cannot spell, is a uint64 and no int64.
  flexbuffers::Builder top;
  top.UInt(uint64_t{18446744073709551615U});
  top.Finish();
  EXPECT_EQ(putOutcome(initial, {fieldOffset("i64")}, top.GetBuffer()),
            ApplyOutcome::Refused);
  EXPECT_EQ(putOutcome(initial, {fieldOffset("ui64")}, top.GetBuffer()),
            ApplyOutcome::Applied);
}
