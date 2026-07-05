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

#include "testing/FlexbufferMatcher.hpp"

#include <flatbuffers/flexbuffers.h>

#include <cassert>
#include <cstring>

#include "testing/MatcherInternal.hpp"

using ::testing::MatchResultListener;
using ::testing::PrintToString;

namespace flexbuffers {
namespace {

const char* FlexbufferTypeNames(const Type type) {
  switch (type) {
    case FBT_NULL:
      return "Null";
    case FBT_BOOL:
      return "Bool";
    case FBT_INDIRECT_INT:
    case FBT_INT:
      return "Int";
    case FBT_INDIRECT_UINT:
    case FBT_UINT:
      return "UInt";
    case FBT_INDIRECT_FLOAT:
    case FBT_FLOAT:
      return "Float";
    case FBT_KEY:
      return "Key";
    case FBT_STRING:
      return "String";
    case FBT_MAP:
      return "Map";
    case FBT_VECTOR:
    case FBT_VECTOR_INT:
    case FBT_VECTOR_UINT:
    case FBT_VECTOR_FLOAT:
    case FBT_VECTOR_KEY:
    case FBT_VECTOR_STRING_DEPRECATED:
    case FBT_VECTOR_INT2:
    case FBT_VECTOR_UINT2:
    case FBT_VECTOR_FLOAT2:
    case FBT_VECTOR_INT3:
    case FBT_VECTOR_UINT3:
    case FBT_VECTOR_FLOAT3:
    case FBT_VECTOR_INT4:
    case FBT_VECTOR_UINT4:
    case FBT_VECTOR_FLOAT4:
    case FBT_VECTOR_BOOL:
      return "Vector";
    case FBT_BLOB:
      return "Blob";
    case FBT_MAX_TYPE:
      assert(false);
  }
  assert(false);
  return "";
}

}  // namespace

// Checks the equality of two Flexbuffers. This checker ignores whether values
// are 'Indirect' and typed vectors are treated as plain vectors.
bool FlexbufferEqImpl(const Reference& expected, const Reference& arg,
                      const std::string& path,
                      ::testing::MatchResultListener* listener) {
  const auto* expected_type = FlexbufferTypeNames(expected.GetType());
  const auto* arg_type = FlexbufferTypeNames(arg.GetType());

  RETURN_IF_NOT_TRUE_MSG(strcmp(expected_type, arg_type) == 0,
                         std::string("type ") + expected_type,
                         std::string("type ") + arg_type, path, listener);

  switch (expected.GetType()) {
    case FBT_NULL: {
      // No value checking necessary as Null has no value.
      break;
    }
    case FBT_BOOL: {
      RETURN_IF_NOT_TRUE_MSG(expected.AsBool() == arg.AsBool(),
                             (expected.AsBool() ? "true" : "false"),
                             (arg.AsBool() ? "true" : "false"), path, listener);
      break;
    }
    case FBT_INDIRECT_INT:
    case FBT_INT: {
      RETURN_IF_NOT_TRUE_MSG(expected.AsInt64() == arg.AsInt64(),
                             PrintToString(expected.AsInt64()),
                             PrintToString(arg.AsInt64()), path, listener);
      break;
    }
    case FBT_INDIRECT_UINT:
    case FBT_UINT: {
      RETURN_IF_NOT_TRUE_MSG(expected.AsUInt64() == arg.AsUInt64(),
                             PrintToString(expected.AsUInt64()),
                             PrintToString(arg.AsUInt64()), path, listener);
      break;
    }
    case FBT_INDIRECT_FLOAT:
    case FBT_FLOAT: {
      RETURN_IF_NOT_TRUE_MSG(expected.AsDouble() == arg.AsDouble(),
                             PrintToString(expected.AsDouble()),
                             PrintToString(arg.AsDouble()), path, listener);
      break;
    }
    case FBT_KEY: {
      RETURN_IF_NOT_TRUE_MSG(strcmp(expected.AsKey(), arg.AsKey()) == 0,
                             PrintToString(expected.AsKey()),
                             PrintToString(arg.AsKey()), path, listener);
      break;
    }
    case FBT_STRING: {
      RETURN_IF_NOT_TRUE_MSG(
          strcmp(expected.AsString().c_str(), arg.AsString().c_str()) == 0,
          PrintToString(expected.AsString().c_str()),
          PrintToString(arg.AsString().c_str()), path, listener);
      break;
    }
    case FBT_MAP: {
      const Map expected_map = expected.AsMap();
      const Map arg_map = arg.AsMap();
      RETURN_IF_NOT_TRUE_MSG(
          expected_map.size() == arg_map.size(),
          "map of size " + PrintToString(expected_map.size()),
          "map of size " + PrintToString(arg_map.size()), path, listener);
      const TypedVector expected_keys = expected_map.Keys();
      const TypedVector arg_keys = arg_map.Keys();
      for (size_t i = 0; i < expected_keys.size(); ++i) {
        RETURN_IF_NOT_TRUE(FlexbufferEqImpl(
            expected_keys[i], arg_keys[i],
            path + "[" + expected_keys[i].AsKey() + "]", listener));
      }
      // Don't return in case of success, because we still need to check that
      // the values match. This is done in the vector section below, since Maps
      // are also Vectors in the flexbuffer representation.
      [[fallthrough]];
    }
    case FBT_VECTOR:
    case FBT_VECTOR_INT:
    case FBT_VECTOR_UINT:
    case FBT_VECTOR_FLOAT:
    case FBT_VECTOR_KEY:
    case FBT_VECTOR_STRING_DEPRECATED:
    case FBT_VECTOR_INT2:
    case FBT_VECTOR_UINT2:
    case FBT_VECTOR_FLOAT2:
    case FBT_VECTOR_INT3:
    case FBT_VECTOR_UINT3:
    case FBT_VECTOR_FLOAT3:
    case FBT_VECTOR_INT4:
    case FBT_VECTOR_UINT4:
    case FBT_VECTOR_FLOAT4:
    case FBT_VECTOR_BOOL: {
      const Vector expected_vector = expected.AsVector();
      const Vector arg_vector = arg.AsVector();
      RETURN_IF_NOT_TRUE_MSG(
          expected_vector.size() == arg_vector.size(),
          "vector of size " + PrintToString(expected_vector.size()),
          "vector of size " + PrintToString(arg_vector.size()), path, listener);
      for (size_t i = 0; i < expected_vector.size(); ++i) {
        RETURN_IF_NOT_TRUE(FlexbufferEqImpl(expected_vector[i], arg_vector[i],
                                            path + "[" + PrintToString(i) + "]",
                                            listener));
      }
      break;
    }
    case FBT_BLOB: {
      RETURN_IF_NOT_TRUE_MSG(
          expected.AsBlob().size() == arg.AsBlob().size()
              && std::memcmp(expected.AsBlob().data(), arg.AsBlob().data(),
                             expected.AsBlob().size())
                     == 0,
          PrintToString(expected.AsBlob()), PrintToString(arg.AsBlob()), path,
          listener);
      break;
    }
    case FBT_MAX_TYPE:
      // FBT_MAX_TYPE is a sentinel, not a valid flexbuffer type.
      // The type comparison above (via FlexbufferTypeNames) will assert
      // before reaching here.
      assert(false && "FBT_MAX_TYPE is not a valid flexbuffer type");
      return false;
  }
  return true;
}

bool FlexbufferEqImpl(const Reference& expected,
                      const std::vector<uint8_t>& arg, const std::string& path,
                      ::testing::MatchResultListener* listener) {
  return FlexbufferEqImpl(expected, GetRoot(arg), path, listener);
}

bool FlexbufferEqImpl(const std::vector<uint8_t>& expected,
                      const Reference& arg, const std::string& path,
                      ::testing::MatchResultListener* listener) {
  return FlexbufferEqImpl(GetRoot(expected), arg, path, listener);
}

bool FlexbufferEqImpl(const std::vector<uint8_t>& expected,
                      const std::vector<uint8_t>& arg, const std::string& path,
                      ::testing::MatchResultListener* listener) {
  return FlexbufferEqImpl(GetRoot(expected), GetRoot(arg), path, listener);
}
}  // namespace flexbuffers
