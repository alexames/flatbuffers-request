// libFuzzer entry point: read arbitrary bytes as a serialized Request, built
// by hand rather than parsed, and apply it to a buffer of the test schema.
// The apply must answer every request without crashing; an Applied one must
// build a buffer that verifies, and any other must build nothing.
//
// Two readings of the bytes: as a Request buffer, applied where it passes
// the Request verifier; and as a recipe for one, built from the field
// offsets, member values, indices and payloads the test schema gives, which
// reaches far more of the apply than verifying bytes do. A recipe's request
// must pass the Request verifier too, as the apply requires.
//
// Build with -DFBREQUEST_BUILD_FUZZERS=ON using a Clang toolchain; see
// fuzz/CMakeLists.txt.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "fbrequest/FlatbufferRequest.hpp"
#include "fbrequest/FlatbufferRequest_generated.h"
#include "flatbuffers/flatbuffers.h"
#include "flatbuffers/flexbuffers.h"
#include "flatbuffers/idl.h"
#include "flatbuffers/reflection.h"
#include "flatbuffers/util.h"

namespace {

using Bytes = std::vector<uint8_t>;

// Stops the run, saying why, so that a crash the fuzzer reports names the
// check that failed.
[[noreturn]] void fail(const char* why) {
  std::fprintf(stderr, "%s\n", why);
  std::abort();
}

// A buffer whose data is aligned to 8 bytes, as a flatbuffer is read.
class AlignedBytes {
 public:
  explicit AlignedBytes(std::span<const uint8_t> bytes)
      : words_((bytes.size() + 7) / 8), size_(bytes.size()) {
    if (not bytes.empty()) {
      std::memcpy(words_.data(), bytes.data(), bytes.size());
    }
  }
  const uint8_t* data() const {
    return reinterpret_cast<const uint8_t*>(words_.data());
  }
  size_t size() const { return size_; }

 private:
  std::vector<uint64_t> words_;
  size_t size_;
};

struct Fixture {
  std::string bfbs;
  const reflection::Schema* schema = nullptr;
  // Buffers the requests are applied to: an empty root, and one storing a
  // value of most kinds of field the test schema declares.
  std::vector<AlignedBytes> sources;
  // Values a step of a path may take: every field offset the schema
  // declares, every union member value, and small indices.
  std::vector<uint32_t> steps;
  std::vector<Bytes> payloads;
};

Bytes finished(flatbuffers::FlatBufferBuilder& fbb) {
  return {fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize()};
}

Bytes flexPayload(const char* json) {
  flatbuffers::Parser parser;
  flexbuffers::Builder builder;
  if (not parser.ParseFlexBuffer(json, nullptr, &builder)) {
    fail("the harness payload does not parse");
  }
  return builder.GetBuffer();
}

// The buffer `requests` build, one after another, from an empty root.
Bytes buildSource(const reflection::Schema* schema,
                  const std::vector<const char*>& requests) {
  flatbuffers::FlatBufferBuilder fbb;
  fbb.Finish(
      flatbuffers::Offset<flatbuffers::Table>(fbb.EndTable(fbb.StartTable())));
  auto current = finished(fbb);
  for (const auto* text : requests) {
    const auto request = fbrequest::parseFlatbufferRequest(schema, text);
    if (not request) {
      fail("a request building the fixture does not parse");
    }
    const auto source = AlignedBytes(current);
    flatbuffers::FlatBufferBuilder next;
    const auto applied = fbrequest::tryApplyRequest(
        next, schema, flatbuffers::GetAnyRoot(source.data()),
        flatbuffers::GetRoot<serialized::Request>(request->data()));
    if (applied.outcome != fbrequest::ApplyOutcome::Applied) {
      fail("a request building the fixture is not applied");
    }
    next.Finish(applied.root);
    current = finished(next);
  }
  return current;
}

const Fixture& fixture() {
  static const Fixture built = [] {
    auto result = Fixture{};
    auto text = std::string{};
    if (not flatbuffers::LoadFile(FBREQUEST_FUZZ_SCHEMA, false, &text)) {
      fail("the test schema does not load");
    }
    flatbuffers::Parser parser;
    if (not parser.Parse(text.c_str())) {
      fail("the test schema does not parse");
    }
    parser.Serialize();
    result.bfbs.assign(
        reinterpret_cast<const char*>(parser.builder_.GetBufferPointer()),
        parser.builder_.GetSize());
    result.schema = reflection::GetSchema(result.bfbs.data());

    result.sources.emplace_back(buildSource(result.schema, {}));
    result.sources.emplace_back(buildSource(
        result.schema,
        {
            "put i8 1",
            "put string \"kept\"",
            R"(put object {"i8": 3, "string": "inner", "i32_vector": [4]})",
            "put object.object.i16 5",
            R"(put number.Integer {"value": 5})",
            "put number.Integer.inner_vector[1].a 2",
            R"(put sparse.Integer {"value": 6})",
            "put scalars.i32_array[2] 9",
            "put nested.inner.arr[1] 3",
            "put i32_vector [1, 2, 3]",
            "put ui64_vector [18446744073709551615]",
            R"(put string_vector ["a", "b"])",
            "put object_vector[1].i8 7",
            "put number_vector[0].Fraction.numerator 1",
            "put number_vector[1].Integer.value 2",
            "put sparse_vector[1].Fraction.denominator 3",
            "put struct_vector[1].i8 4",
            R"(put holder_vector[0] {"tail": 8})",
            "put marker_vector[2].color 4",
            "put aligned_vector[0].x 1",
            "put color_vector [0, 4, 5]",
            "put wide.ui64_array[1] 2",
        }));

    auto steps = std::set<uint32_t>{0, 1, 2, 3, 4, 7, 100};
    for (const auto* object : *result.schema->objects()) {
      for (const auto* field : *object->fields()) {
        steps.insert(field->offset());
      }
    }
    for (const auto* enumDef : *result.schema->enums()) {
      for (const auto* value : *enumDef->values()) {
        steps.insert(static_cast<uint32_t>(value->value()));
      }
    }
    result.steps.assign(steps.begin(), steps.end());

    for (const auto* json :
         {"0", "1", "-1", "7", "300", "70000", "18446744073709551615", "1.5",
          "true", "\"s\"", "[]", "[1, 2]", "[\"a\"]", "{}", R"({"i8": 1})",
          R"({"a": 1, "arr": [1, 2, 3]})", R"([{"on": true, "color": 4}])",
          R"({"value": 1})", R"([{"x": 1, "y": 2}])"}) {
      result.payloads.push_back(flexPayload(json));
    }
    for (const auto& source : result.sources) {
      result.payloads.emplace_back(source.data(),
                                   source.data() + source.size());
    }
    flatbuffers::FlatBufferBuilder table;
    table.Finish(flatbuffers::Offset<flatbuffers::Table>(
        table.EndTable(table.StartTable())));
    result.payloads.push_back(finished(table));
    return result;
  }();
  return built;
}

// Reads the fuzzer's bytes one at a time, answering zero once they run out.
class Reader {
 public:
  explicit Reader(std::span<const uint8_t> bytes) : bytes_(bytes) {}
  uint8_t next() {
    if (bytes_.empty()) {
      return 0;
    }
    const auto byte = bytes_.front();
    bytes_ = bytes_.subspan(1);
    return byte;
  }
  std::span<const uint8_t> take(size_t count) {
    const auto taken = bytes_.first(std::min(count, bytes_.size()));
    bytes_ = bytes_.subspan(taken.size());
    return taken;
  }

 private:
  std::span<const uint8_t> bytes_;
};

constexpr int kDeepestPatch = 3;
constexpr uint8_t kMostSteps = 6;
constexpr uint8_t kMostUpdates = 3;
constexpr uint8_t kDropOneIn = 16;
// Three in eight puts, two deletes, two patches, and one of no type the
// Request schema declares.
constexpr uint8_t kOperationKinds = 8;

// A path from `objectDef`, most of whose steps follow the schema: a field
// the object declares, an element index of a vector or array, a member of a
// union. The rest are any value `steps` holds, which the schema seldom
// resolves. `*endDef` is set to the table or struct the path ends at, or
// null.
flatbuffers::Offset<flatbuffers::Vector<uint32_t>> buildPath(
    flatbuffers::FlatBufferBuilder& fbb, Reader& reader,
    const reflection::Object* objectDef, const reflection::Object** endDef) {
  const auto& built = fixture();
  const auto* objects = built.schema->objects();
  auto path = std::vector<uint32_t>{};
  const auto* current = objectDef;
  // The field the last step named where it holds more than one step's worth:
  // a vector, an array or a union.
  const reflection::Field* pending = nullptr;
  auto pendingIndexed = false;
  const auto memberOf = [&](const reflection::Field* fieldDef) {
    const auto* values =
        built.schema->enums()->Get(fieldDef->type()->index())->values();
    const auto* value = values->Get(reader.next() % values->size());
    path.push_back(static_cast<uint32_t>(value->value()));
    const auto* memberType = value->union_type();
    pending = nullptr;
    current =
        (memberType != nullptr and memberType->base_type() == reflection::Obj)
            ? objects->Get(memberType->index())
            : nullptr;
  };
  for (auto count = reader.next() % kMostSteps; count > 0; count--) {
    const auto guide = reader.next();
    if (guide % 8 == 0) {
      path.push_back(built.steps[reader.next() % built.steps.size()]);
      current = nullptr;
      pending = nullptr;
      continue;
    }
    if (pending != nullptr) {
      const auto* type = pending->type();
      if (type->base_type() == reflection::Union) {
        memberOf(pending);
        continue;
      }
      if (not pendingIndexed) {
        path.push_back(reader.next() % 5);
        pendingIndexed = true;
        if (type->element() == reflection::Union) {
          continue;
        }
        current = (type->element() == reflection::Obj)
                      ? objects->Get(type->index())
                      : nullptr;
        pending = nullptr;
        continue;
      }
      memberOf(pending);
      continue;
    }
    if (current == nullptr or current->fields()->size() == 0) {
      path.push_back(built.steps[reader.next() % built.steps.size()]);
      continue;
    }
    const auto* fieldDef =
        current->fields()->Get(reader.next() % current->fields()->size());
    path.push_back(fieldDef->offset());
    const auto* type = fieldDef->type();
    switch (type->base_type()) {
      case reflection::Obj:
        current = objects->Get(type->index());
        break;
      case reflection::Union:
      case reflection::Vector:
      case reflection::Vector64:
      case reflection::Array:
        pending = fieldDef;
        pendingIndexed = false;
        break;
      default:
        current = nullptr;
        break;
    }
  }
  *endDef = (pending == nullptr) ? current : nullptr;
  return fbb.CreateVector(path);
}

flatbuffers::Offset<flatbuffers::Vector<uint8_t>> buildPayload(
    flatbuffers::FlatBufferBuilder& fbb, Reader& reader) {
  const auto& payloads = fixture().payloads;
  const auto choice = reader.next();
  if (choice < payloads.size() * 4) {
    return fbb.CreateVector(payloads[choice % payloads.size()]);
  }
  const auto raw = reader.take(reader.next());
  return fbb.CreateVector(raw.data(), raw.size());
}

// One operation, of the type the next byte names, its members each present
// unless a flag byte drops it.
flatbuffers::Offset<void> buildOperation(flatbuffers::FlatBufferBuilder& fbb,
                                         Reader& reader, int depth,
                                         const reflection::Object* objectDef,
                                         serialized::Operation* type) {
  const reflection::Object* endDef = nullptr;
  const auto kind = reader.next();
  const auto flags = reader.next();
  // A member is dropped in one operation of sixteen, so that most requests
  // carry what the parser writes and reach past the shape check.
  const auto keep = [&](int member) {
    return flags % kDropOneIn != static_cast<uint8_t>(member);
  };
  switch (kind % kOperationKinds) {
    case 0:
    case 1:
    case 2: {
      *type = serialized::Operation::Put;
      const auto path =
          keep(0) ? buildPath(fbb, reader, objectDef, &endDef) : 0;
      const auto payload = keep(1) ? buildPayload(fbb, reader) : 0;
      return serialized::CreatePut(fbb, path, payload).Union();
    }
    case 3:
    case 4: {
      *type = serialized::Operation::Delete;
      const auto path =
          keep(0) ? buildPath(fbb, reader, objectDef, &endDef) : 0;
      return serialized::CreateDelete(fbb, path).Union();
    }
    case 5:
    case 6: {
      *type = serialized::Operation::Patch;
      const auto path =
          keep(0) ? buildPath(fbb, reader, objectDef, &endDef) : 0;
      auto types = std::vector<serialized::Operation>{};
      auto updates = std::vector<flatbuffers::Offset<void>>{};
      if (depth < kDeepestPatch) {
        for (auto count = reader.next() % (kMostUpdates + 1); count > 0;
             count--) {
          auto nestedType = serialized::Operation::NONE;
          updates.push_back(buildOperation(
              fbb, reader, depth + 1, (endDef != nullptr) ? endDef : objectDef,
              &nestedType));
          types.push_back(nestedType);
        }
      }
      // The Request verifier takes the types and the operations only
      // together.
      if (not keep(1)) {
        return serialized::CreatePatch(fbb, path).Union();
      }
      return serialized::CreatePatch(fbb, path, fbb.CreateVector(types),
                                     fbb.CreateVector(updates))
          .Union();
    }
    default: {
      // NONE, or a type the union does not declare, which the Request
      // verifier takes with any table beside it.
      *type = static_cast<serialized::Operation>((flags % 2 == 0) ? 0 : 4);
      return serialized::CreateDelete(
                 fbb, buildPath(fbb, reader, objectDef, &endDef))
          .Union();
    }
  }
}

// Applies the Request in `request` to every source, in a builder already
// holding `heldBytes` bytes of the caller's own, and stops the run where the
// apply answers what it must not.
void applyToEverySource(const uint8_t* request, size_t heldBytes) {
  const auto& built = fixture();
  for (const auto& source : built.sources) {
    flatbuffers::FlatBufferBuilder fbb;
    if (heldBytes > 0) {
      fbb.CreateString(std::string(heldBytes, 'x'));
    }
    const auto held = fbb.GetSize();
    const auto applied = fbrequest::tryApplyRequest(
        fbb, built.schema, flatbuffers::GetAnyRoot(source.data()),
        flatbuffers::GetRoot<serialized::Request>(request));
    if (applied.outcome != fbrequest::ApplyOutcome::Applied) {
      if (fbb.GetSize() != held or not applied.root.IsNull()) {
        fail("a request that is not Applied built something");
      }
      continue;
    }
    fbb.Finish(applied.root);
    if (not fbrequest::verifyBuffer(built.schema,
                                    {fbb.GetBufferPointer(), fbb.GetSize()})) {
      fail("an Applied request built a buffer that does not verify");
    }
  }
}

// A put may grow a vector to any index a flatbuffer can hold, up to a
// gigabyte: correct, and more than the fuzzer's memory limit. A request whose
// path values, summed over every path it holds, pass this is not applied:
// each step of a path may grow a vector of its own.
constexpr uint64_t kLargestAppliedPathTotal = 1U << 16;

// Adds the values of `offsets` to `total`.
void addPath(const flatbuffers::Vector<uint32_t>* offsets, uint64_t& total) {
  if (offsets != nullptr) {
    for (const auto value : *offsets) {
      total += value;
    }
  }
}

// Adds the values of every path `operation`, of type `type`, holds to
// `total`.
void addPaths(serialized::Operation type, const void* operation,
              uint64_t& total) {
  switch (type) {
    case serialized::Operation::Put:
      addPath(static_cast<const serialized::Put*>(operation)->offsets(), total);
      return;
    case serialized::Operation::Delete:
      addPath(static_cast<const serialized::Delete*>(operation)->offsets(),
              total);
      return;
    case serialized::Operation::Patch: {
      const auto* patch = static_cast<const serialized::Patch*>(operation);
      addPath(patch->offsets(), total);
      if (patch->updates() == nullptr or patch->updates_type() == nullptr) {
        return;
      }
      const auto count =
          std::min(patch->updates()->size(), patch->updates_type()->size());
      for (flatbuffers::uoffset_t i = 0; i < count; i++) {
        if (patch->updates()->Get(i) != nullptr) {
          addPaths(patch->updates_type()->Get(i), patch->updates()->Get(i),
                   total);
        }
      }
      return;
    }
    default:
      return;
  }
}

// The bytes of its own a run's builder holds before the apply, chosen by the
// run's first byte, so that the bound on what a request may grow is fuzzed
// against a builder that is not empty.
constexpr size_t kHeldBytesStep = 300;
constexpr uint8_t kHeldBytesChoices = 4;

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size == 0) {
    return 0;
  }
  const auto bytes = std::span{data + 1, size - 1};
  const auto heldBytes = ((data[0] >> 1U) % kHeldBytesChoices) * kHeldBytesStep;
  if (data[0] % 2 == 0) {
    const auto request = AlignedBytes(bytes);
    auto verifier = flatbuffers::Verifier(request.data(), request.size());
    if (not serialized::VerifyRequestBuffer(verifier)) {
      return 0;
    }
    const auto* root =
        flatbuffers::GetRoot<serialized::Request>(request.data());
    auto total = uint64_t{0};
    if (root->operation() != nullptr) {
      addPaths(root->operation_type(), root->operation(), total);
    }
    if (total > kLargestAppliedPathTotal) {
      return 0;
    }
    applyToEverySource(request.data(), heldBytes);
    return 0;
  }
  auto reader = Reader{bytes};
  flatbuffers::FlatBufferBuilder fbb;
  auto type = serialized::Operation::NONE;
  const auto operation =
      buildOperation(fbb, reader, 0, fixture().schema->root_table(), &type);
  fbb.Finish(serialized::CreateRequest(fbb, type, operation));
  // The apply takes only a request that passes the Request verifier, which
  // every recipe is built to: each operation is paired with its type, or
  // with one the verifier does not read (NONE, or one the union does not
  // declare).
  auto verifier = flatbuffers::Verifier(fbb.GetBufferPointer(), fbb.GetSize());
  if (not serialized::VerifyRequestBuffer(verifier)) {
    fail("a request built from a recipe does not pass the Request verifier");
  }
  applyToEverySource(fbb.GetBufferPointer(), heldBytes);
  return 0;
}
