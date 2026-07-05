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

#include <cstdint>
#include <string>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace flexbuffers {
class Reference;
}

// Convenience macros for flexbuffer equality assertions.
// These mirror the EXPECT_FLATBUFFERS_EQ / ASSERT_FLATBUFFERS_EQ family.
// Flexbuffers are schema-less, so the schema parameter is accepted for
// API symmetry with the flatbuffer macros but is intentionally unused.
#define EXPECT_FLEXBUFFERS_EQ(schema, arg, expected) \
  EXPECT_THAT((arg), ::flexbuffers::FlexbufferEq((expected)))

#define EXPECT_FLEXBUFFERS_NE(schema, arg, expected) \
  EXPECT_THAT((arg), ::testing::Not(::flexbuffers::FlexbufferEq((expected))))

#define ASSERT_FLEXBUFFERS_EQ(schema, arg, expected) \
  ASSERT_THAT((arg), ::flexbuffers::FlexbufferEq((expected)))

#define ASSERT_FLEXBUFFERS_NE(schema, arg, expected) \
  ASSERT_THAT((arg), ::testing::Not(::flexbuffers::FlexbufferEq((expected))))

namespace flexbuffers {

// Recursively checks equality of two flexbuffer values. Ignores whether
// values are stored as 'Indirect' and treats typed vectors as plain vectors.
//
// Preconditions:
//   - result_listener must be non-null.
//   - For the Reference overloads, both references must be valid (i.e.,
//     obtained from a well-formed flexbuffer).
//   - For the vector<uint8_t> overloads, the buffers must contain valid
//     flexbuffer data (they are interpreted via flexbuffers::GetRoot).
//
// Postcondition: returns true if the values are structurally and value-equal.
// On the first mismatch, writes a diagnostic including the location path
// to result_listener and returns false.
bool FlexbufferEqImpl(const Reference& expected, const Reference& arg,
                      const std::string& location,
                      ::testing::MatchResultListener* result_listener);

bool FlexbufferEqImpl(const Reference& expected,
                      const std::vector<uint8_t>& arg,
                      const std::string& location,
                      ::testing::MatchResultListener* result_listener);

bool FlexbufferEqImpl(const std::vector<uint8_t>& expected,
                      const Reference& arg, const std::string& location,
                      ::testing::MatchResultListener* result_listener);

bool FlexbufferEqImpl(const std::vector<uint8_t>& expected,
                      const std::vector<uint8_t>& arg,
                      const std::string& location,
                      ::testing::MatchResultListener* result_listener);

// TODO(73494146): Move this to Flatbuffers.
// GTest MATCHER_P expands to a class that captures `expected` by const ref —
// out of our control.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
MATCHER_P(FlexbufferEq, expected,
          std::string("contains a flexbuffer that ")
              + (negation ? "does not match " : "matches ")
              + testing::PrintToString(expected)) {
  return FlexbufferEqImpl(expected, arg, std::string(), result_listener);
}

}  // namespace flexbuffers
