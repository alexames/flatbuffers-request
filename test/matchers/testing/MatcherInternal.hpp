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
#include <string>

namespace testing {
class MatchResultListener;
}

// These macros must remain macros (not inline functions) because they perform
// early return from the calling function when a condition fails.

// Precondition: must be used inside a function returning bool.
// Returns false from the enclosing function if condition is false.
#define RETURN_IF_NOT_TRUE(condition) \
  do {                                \
    if (!(condition)) {               \
      return false;                   \
    }                                 \
  } while (0)

// Precondition: must be used inside a function returning bool.
// Reports a mismatch message and returns false from the enclosing function
// if condition is false.
#define RETURN_IF_NOT_TRUE_MSG(condition, expected_string, arg_string, path,  \
                               listener)                                      \
  do {                                                                        \
    if (!(condition)) {                                                       \
      ::flatbuffers::MismatchMessage((expected_string), (arg_string), (path), \
                                     (listener));                             \
      return false;                                                           \
    }                                                                         \
  } while (0)

namespace flatbuffers {

// Formats and writes a field mismatch description to the listener.
//
// Precondition: listener must be non-null.
void MismatchMessage(const std::string& expected_string,
                     const std::string& arg_string, const std::string& path,
                     ::testing::MatchResultListener* listener);

}  // namespace flatbuffers
