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

#pragma once
#include <ostream>
#include <span>
#include <string>
#include <type_traits>

#include "flatbuffers/flatbuffers.h"
#include "flatbuffers/reflection.h"
#include "flatbuffers/reflection_generated.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

#define EXPECT_FLATBUFFERS_EQ(schema, arg, expected)       \
  EXPECT_THAT(::flatbuffers::TypedBuffer((schema), (arg)), \
              ::flatbuffers::FlatbuffersEq(                \
                  ::flatbuffers::TypedBuffer((schema), (expected))))

#define EXPECT_FLATBUFFERS_NE(schema, arg, expected)       \
  EXPECT_THAT(::flatbuffers::TypedBuffer((schema), (arg)), \
              ::testing::Not(::flatbuffers::FlatbuffersEq( \
                  ::flatbuffers::TypedBuffer((schema), (expected)))))

#define ASSERT_FLATBUFFERS_EQ(schema, arg, expected)       \
  ASSERT_THAT(::flatbuffers::TypedBuffer((schema), (arg)), \
              ::flatbuffers::FlatbuffersEq(                \
                  ::flatbuffers::TypedBuffer((schema), (expected))))

#define ASSERT_FLATBUFFERS_NE(schema, arg, expected)       \
  ASSERT_THAT(::flatbuffers::TypedBuffer((schema), (arg)), \
              ::testing::Not(::flatbuffers::FlatbuffersEq( \
                  ::flatbuffers::TypedBuffer((schema), (expected)))))

namespace flatbuffers {

// TypedBuffer pairs a FlatBuffer binary payload with its reflection schema,
// enabling schema-aware comparison and pretty-printing.
//
// Regularity: TypedBuffer is a Regular type -- it supports default
// construction, copy, move, assignment, equality, and destruction with the
// expected semantics. Two TypedBuffers are equal when they reference the same
// schema (by pointer identity) and hold byte-identical buffers.
class TypedBuffer {
 public:
  TypedBuffer() = default;

  // Precondition: schema must be non-null and must outlive this TypedBuffer.
  // Precondition: container.data() must point to a valid contiguous range of
  //               container.size() bytes.
  template <typename ContiguousContainer>
  TypedBuffer(const reflection::Schema* schema,
              const ContiguousContainer& container)
      : schema_(schema),
        buffer_(container.data(), container.data() + container.size()) {}

  [[nodiscard]] const reflection::Schema* schema() const { return schema_; }
  [[nodiscard]] const std::vector<uint8_t>& buffer() const { return buffer_; }

  friend bool operator==(const TypedBuffer& lhs, const TypedBuffer& rhs) {
    return lhs.schema_ == rhs.schema_ && lhs.buffer_ == rhs.buffer_;
  }

  friend bool operator!=(const TypedBuffer& lhs, const TypedBuffer& rhs) {
    return !(lhs == rhs);
  }

 private:
  const reflection::Schema* schema_ = nullptr;
  std::vector<uint8_t> buffer_;
};

// Writes a human-readable JSON representation of the typed buffer to the
// output stream. Handles empty buffers, null schemas, and invalid buffers
// gracefully by printing a descriptive placeholder.
std::ostream& operator<<(std::ostream& out, const TypedBuffer& buffer);

// Recursively compares two FlatBuffer tables field-by-field using reflection.
//
// Preconditions:
//   - expected_table and arg_table must be non-null and point to valid,
//     verified FlatBuffer tables matching object_def's schema.
//   - listener must be non-null.
//
// Postcondition: returns true if all fields are equal. On the first mismatch,
// writes a diagnostic to listener and returns false. The path parameter is
// prepended to field names in diagnostic messages.
bool CompareTables(const reflection::Schema& schema,
                   const reflection::Object& object_def,
                   const Table* expected_table, const Table* arg_table,
                   const std::string& path,
                   ::testing::MatchResultListener* listener);

// GTest MATCHER_P expands to a class that captures `expected` by const ref —
// out of our control.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
MATCHER_P(FlatbuffersEq, expected,
          std::string("contains a flatbuffer that ")
              + (negation ? "does not match " : "matches ")
              + testing::PrintToString(expected)) {
  static_assert(std::is_same_v<std::remove_cvref_t<decltype(arg)>, TypedBuffer>,
                "Argument must be passed as a TypedBuffer");
  static_assert(
      std::is_same_v<std::remove_cvref_t<decltype(expected)>, TypedBuffer>,
      "Expected Flatbuffer must be passed as a TypedBuffer");

  if (expected.schema() != arg.schema()) {
    *result_listener << "schema mismatch";
    return false;
  }

  const reflection::Object& root_object_def = *expected.schema()->root_table();
  if (expected.buffer().empty()
      || !Verify(*expected.schema(), root_object_def, expected.buffer().data(),
                 expected.buffer().size())) {
    *result_listener << "expected flatbuffer failed verification";
    return false;
  }
  if (arg.buffer().empty()
      || !Verify(*arg.schema(), root_object_def, arg.buffer().data(),
                 arg.buffer().size())) {
    *result_listener << "arg flatbuffer failed verification";
    return false;
  }

  const Table* expected_table =
      flatbuffers::GetAnyRoot(expected.buffer().data());
  const Table* arg_table = flatbuffers::GetAnyRoot(arg.buffer().data());
  return CompareTables(*expected.schema(), root_object_def, expected_table,
                       arg_table, std::string(), result_listener);
}

}  // namespace flatbuffers
