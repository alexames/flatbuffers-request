// libFuzzer entry point: feed arbitrary bytes as request text, and — when it
// parses — apply it against an empty root to exercise the build path too.
//
// Build with -DFBREQUEST_BUILD_FUZZERS=ON using a Clang toolchain; see
// fuzz/CMakeLists.txt.
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "fbrequest/FlatbufferRequest.hpp"
#include "flatbuffers/flatbuffers.h"
#include "flatbuffers/idl.h"
#include "flatbuffers/reflection.h"

namespace {

// A small schema covering the shapes the apply/parse code handles.
constexpr const char* kSchema = R"(
namespace demo;
table Sub { x: int32; name: string; }
union U { Sub }
struct S { a: int32; b: float32; }
table Root {
  i8: int8;
  i32: int32;
  f32: float32;
  name: string;
  sub: Sub;
  u: U;
  s: S;
  ints: [int32];
  names: [string];
  subs: [Sub];
}
root_type Root;
)";

const std::string& schemaBfbs() {
  static const std::string bfbs = [] {
    flatbuffers::Parser parser;
    parser.Parse(kSchema);
    parser.Serialize();  // serialize the parsed schema as a .bfbs
    return std::string(
        reinterpret_cast<const char*>(parser.builder_.GetBufferPointer()),
        parser.builder_.GetSize());
  }();
  return bfbs;
}

const std::vector<uint8_t>& emptyRoot() {
  static const std::vector<uint8_t> root = [] {
    flatbuffers::Parser parser;
    parser.Parse(kSchema);
    parser.Parse("{}");
    const uint8_t* p = parser.builder_.GetBufferPointer();
    return std::vector<uint8_t>(p, p + parser.builder_.GetSize());
  }();
  return root;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const auto* schema = reflection::GetSchema(schemaBfbs().data());
  std::string_view request(reinterpret_cast<const char*>(data), size);

  auto parsed = fbrequest::parseFlatbufferRequest(schema, request);
  if (parsed) {
    flatbuffers::FlatBufferBuilder fbb;
    fbb.Finish(fbrequest::applyRequest(
        fbb, schema, flatbuffers::GetAnyRoot(emptyRoot().data()),
        parsed->get()));
  }
  return 0;
}
