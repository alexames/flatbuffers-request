// libFuzzer entry point: feed arbitrary bytes as request text, and — when it
// parses — apply it against an empty root to exercise the build path too.
//
// Build with -DFBREQUEST_BUILD_FUZZERS=ON using a Clang toolchain; see
// fuzz/CMakeLists.txt.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "fbrequest/FlatbufferRequest.hpp"
#include "fbrequest/FlatbufferRequest_generated.h"
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

// A put may grow a vector to any index a flatbuffer can hold, up to a
// gigabyte: correct, and more than the fuzzer's memory limit. A put or patch
// whose path steps past this is parsed but not applied; a delete builds
// nothing to reach an index, so it is applied whatever its path.
constexpr uint32_t kLargestAppliedPathValue = 1U << 16;

bool pathFits(const flatbuffers::Vector<uint32_t>* offsets) {
  return offsets == nullptr
         or std::ranges::all_of(*offsets, [](uint32_t value) {
              return value <= kLargestAppliedPathValue;
            });
}

bool operationFits(serialized::Operation type, const void* operation) {
  switch (type) {
    case serialized::Operation::Put:
      return pathFits(
          static_cast<const serialized::Put*>(operation)->offsets());
    case serialized::Operation::Patch: {
      const auto* patch = static_cast<const serialized::Patch*>(operation);
      if (not pathFits(patch->offsets())) {
        return false;
      }
      if (patch->updates() == nullptr or patch->updates_type() == nullptr) {
        return true;
      }
      for (flatbuffers::uoffset_t i = 0; i < patch->updates()->size(); i++) {
        if (not operationFits(patch->updates_type()->Get(i),
                              patch->updates()->Get(i))) {
          return false;
        }
      }
      return true;
    }
    default:
      return true;
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const auto* schema = reflection::GetSchema(schemaBfbs().data());
  std::string_view request(reinterpret_cast<const char*>(data), size);

  auto parsed = fbrequest::parseFlatbufferRequest(schema, request);
  if (parsed) {
    // The apply takes only a request that passes the Request verifier, so
    // every request the parser writes must pass it.
    auto verifier = flatbuffers::Verifier(parsed->data(), parsed->size());
    if (not serialized::VerifyRequestBuffer(verifier)) {
      std::fprintf(stderr, "a parsed request does not pass the verifier
");
      std::abort();
    }
    const auto* root =
        flatbuffers::GetRoot<serialized::Request>(parsed->data());
    if (not operationFits(root->operation_type(), root->operation())) {
      return 0;
    }
    flatbuffers::FlatBufferBuilder fbb;
    const auto applied = fbrequest::tryApplyRequest(
        fbb, schema, flatbuffers::GetAnyRoot(emptyRoot().data()),
        flatbuffers::GetRoot<serialized::Request>(parsed->data()));
    if (applied.outcome == fbrequest::ApplyOutcome::Applied) {
      fbb.Finish(applied.root);
    }
  }
  return 0;
}
