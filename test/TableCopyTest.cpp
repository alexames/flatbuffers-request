// What a request does not edit comes out of the apply as it went in, for every
// kind of field a schema can declare: unions holding structs and strings,
// vectors of them, strings and vectors held through 64-bit offsets, vectors
// of structs aligned past 4 bytes, and vectors the schema asks to align. Each
// is checked where the apply copies it itself (a table along the request's
// path), below that (a table copied whole), and in a put's value.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <ostream>
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

using namespace fbrequest;

namespace {

// Struct and string union members, and 64-bit offsets, are accepted only for
// schemas generated for C++. `Spaced` gives its one member a value that is
// not its position, which a reader indexing members by position misreads.
constexpr auto kSchema = R"fbs(
  namespace copy_test;

  table Leaf { x: int32; w: int64; }
  struct Wide { a: int8; b: int64; }
  struct Forced (force_align: 16) { a: int32; }
  union Member { Leaf, Wide, Text: string, Forced }
  union Spaced { Leaf = 3 }

  table Holder {
    n: int32;
    member: Member;
    members: [Member];
    spaced: Spaced;
    text64: string (offset64);
    bytes64: [ubyte] (offset64);
    wides64: [Wide] (vector64);
    wides: [Wide];
    forced: [Forced];
    aligned: [ubyte] (force_align: 16);
    nested: [ubyte] (nested_flatbuffer: "Leaf");
    leaves: [Leaf];
    wides32: [Wide] (offset64);
  }

  table Root {
    i: int32;
    // Sorts before `holder`, so a copy builds it first.
    filler: string;
    holder: Holder;
    holders: [Holder];
  }

  root_type Root;
)fbs";

// The shapes a copy must keep, each a field of Holder (a union with its
// discriminant).
enum class Shape {
  kLeafMember,
  kStructMember,
  kStringMember,
  kForcedMember,
  kUnionVector,
  kSpacedMember,
  kString64,
  kOffset64Vector,
  kVector64,
  kAlignedStructs,
  kForcedStructs,
  kForcedBytes,
  kNestedFlatbuffer,
  kTables,
};

constexpr auto kEveryShape = std::array{
    Shape::kLeafMember,       Shape::kStructMember,   Shape::kStringMember,
    Shape::kForcedMember,     Shape::kUnionVector,    Shape::kSpacedMember,
    Shape::kString64,         Shape::kOffset64Vector, Shape::kVector64,
    Shape::kAlignedStructs,   Shape::kForcedStructs,  Shape::kForcedBytes,
    Shape::kNestedFlatbuffer, Shape::kTables,
};

const char* shapeName(Shape shape) {
  switch (shape) {
    case Shape::kLeafMember:
      return "LeafMember";
    case Shape::kStructMember:
      return "StructMember";
    case Shape::kStringMember:
      return "StringMember";
    case Shape::kForcedMember:
      return "ForcedMember";
    case Shape::kUnionVector:
      return "UnionVector";
    case Shape::kSpacedMember:
      return "SpacedMember";
    case Shape::kString64:
      return "String64";
    case Shape::kOffset64Vector:
      return "Offset64Vector";
    case Shape::kVector64:
      return "Vector64";
    case Shape::kAlignedStructs:
      return "AlignedStructs";
    case Shape::kForcedStructs:
      return "ForcedStructs";
    case Shape::kForcedBytes:
      return "ForcedBytes";
    case Shape::kNestedFlatbuffer:
      return "NestedFlatbuffer";
    case Shape::kTables:
      return "Tables";
  }
  return "";
}

const char* shapeJson(Shape shape) {
  switch (shape) {
    case Shape::kLeafMember:
      return R"json("member_type": "Leaf", "member": {"x": 2, "w": 3})json";
    case Shape::kStructMember:
      return R"json("member_type": "Wide", "member": {"a": 2, "b": 3})json";
    case Shape::kStringMember:
      return R"json("member_type": "Text", "member": "two")json";
    case Shape::kForcedMember:
      return R"json("member_type": "Forced", "member": {"a": 32})json";
    case Shape::kUnionVector:
      return R"json("members_type": ["Leaf", "Wide", "Text"],
        "members": [{"x": 4, "w": 5}, {"a": 6, "b": 7}, "eight"])json";
    case Shape::kSpacedMember:
      return R"json("spaced_type": "Leaf", "spaced": {"x": 30, "w": 31})json";
    case Shape::kString64:
      return R"json("text64": "nine")json";
    case Shape::kOffset64Vector:
      return R"json("bytes64": [10, 11, 12])json";
    case Shape::kVector64:
      return R"json("wides64": [{"a": 13, "b": 14}, {"a": 15, "b": 16}])json";
    case Shape::kAlignedStructs:
      return R"json("wides": [{"a": 17, "b": 18}])json";
    case Shape::kForcedStructs:
      return R"json("forced": [{"a": 19}])json";
    case Shape::kForcedBytes:
      return R"json("aligned": [20, 21, 22])json";
    case Shape::kNestedFlatbuffer:
      return R"json("nested": {"x": 23, "w": 24})json";
    case Shape::kTables:
      return R"json("leaves": [{"x": 25, "w": 26}])json";
  }
  return "";
}

void PrintTo(Shape shape, std::ostream* out) { *out << shapeName(shape); }

bool isMember(Shape shape) {
  return shape == Shape::kLeafMember or shape == Shape::kStructMember
         or shape == Shape::kStringMember or shape == Shape::kForcedMember;
}

// Every shape, with `member` the one its union holds.
std::vector<Shape> everyShapeHolding(Shape member) {
  auto shapes = std::vector<Shape>{member};
  for (const auto shape : kEveryShape) {
    if (not isMember(shape)) {
      shapes.push_back(shape);
    }
  }
  return shapes;
}

// A Holder storing `shapes`, and `n` as 1.
std::string holderJson(std::span<const Shape> shapes) {
  auto json = std::string(R"json({"n": 1)json");
  for (const auto shape : shapes) {
    json += ", ";
    json += shapeJson(shape);
  }
  return json + "}";
}

class TableCopyTest : public ::testing::Test {
 protected:
  TableCopyTest() {
    auto parser = makeParser();
    parser->Serialize();
    bfbs_.assign(
        parser->builder_.GetBufferPointer(),
        parser->builder_.GetBufferPointer() + parser->builder_.GetSize());
    schema_ = reflection::GetSchema(bfbs_.data());
  }

  static std::unique_ptr<flatbuffers::Parser> makeParser() {
    auto options = flatbuffers::IDLOptions{};
    options.lang_to_generate = flatbuffers::IDLOptions::kCpp;
    // Keeps force_align and nested_flatbuffer in the serialized schema.
    options.binary_schema_builtins = true;
    auto parser = std::make_unique<flatbuffers::Parser>(options);
    EXPECT_TRUE(parser->Parse(kSchema)) << parser->error_;
    return parser;
  }

  // The buffer `json` describes, with a root of `rootType`.
  static std::vector<uint8_t> buffer(const char* rootType,
                                     const std::string& json) {
    auto parser = makeParser();
    EXPECT_TRUE(parser->SetRootType(rootType));
    EXPECT_TRUE(parser->Parse(json.c_str())) << parser->error_;
    return {parser->builder_.GetBufferPointer(),
            parser->builder_.GetBufferPointer() + parser->builder_.GetSize()};
  }

  // A Root holding the three holders, as `holder`, `holders[0]` and
  // `holders[1]`.
  static std::vector<uint8_t> source(std::span<const Shape> holder,
                                     std::span<const Shape> first,
                                     std::span<const Shape> second) {
    return buffer("Root", R"json({"i": 1, "filler": "p", "holder": )json"
                              + holderJson(holder) + R"json(, "holders": [)json"
                              + holderJson(first) + ", " + holderJson(second)
                              + "]}");
  }

  // A Root whose holders store every shape, their unions each holding one
  // kind of member.
  static std::vector<uint8_t> source() {
    return source(everyShapeHolding(Shape::kLeafMember),
                  everyShapeHolding(Shape::kStructMember),
                  everyShapeHolding(Shape::kStringMember));
  }

  const reflection::Object* object(const char* name) const {
    return schema_->objects()->LookupByKey(name);
  }

  uint16_t field(const char* objectName, const char* name) const {
    return object(objectName)->fields()->LookupByKey(name)->offset();
  }

  struct Applied {
    ApplyOutcome outcome;
    std::vector<uint8_t> bytes;
  };

  Applied applyBuilt(const std::vector<uint8_t>& initial,
                     const uint8_t* request) {
    flatbuffers::FlatBufferBuilder fbb;
    const auto result =
        tryApplyRequest(fbb, schema_, flatbuffers::GetAnyRoot(initial.data()),
                        flatbuffers::GetRoot<serialized::Request>(request));
    if (result.outcome != ApplyOutcome::Applied) {
      EXPECT_EQ(fbb.GetSize(), 0U);
      return {result.outcome, {}};
    }
    fbb.Finish(result.root);
    auto bytes = std::vector<uint8_t>(fbb.GetBufferPointer(),
                                      fbb.GetBufferPointer() + fbb.GetSize());
    EXPECT_TRUE(verifyBuffer(schema_, bytes));
    return {result.outcome, std::move(bytes)};
  }

  // The outcome of `text`, or nullopt where it does not parse.
  std::optional<Applied> applyText(const std::vector<uint8_t>& initial,
                                   std::string_view text) {
    const auto request = parseFlatbufferRequest(schema_, text);
    if (not request.has_value()) {
      return std::nullopt;
    }
    return applyBuilt(initial, request->data());
  }

  Applied applyPut(const std::vector<uint8_t>& initial,
                   const std::vector<uint32_t>& path,
                   const std::vector<uint8_t>& payload) {
    flatbuffers::FlatBufferBuilder requestBuilder;
    requestBuilder.Finish(serialized::CreateRequest(
        requestBuilder, serialized::Operation::Put,
        serialized::CreatePut(requestBuilder, requestBuilder.CreateVector(path),
                              requestBuilder.CreateVector(payload))
            .Union()));
    return applyBuilt(initial, requestBuilder.GetBufferPointer());
  }

  // `text` applied, failing the test unless it is.
  std::vector<uint8_t> applied(const std::vector<uint8_t>& initial,
                               std::string_view text) {
    auto result = applyText(initial, text);
    if (not result.has_value()) {
      ADD_FAILURE() << "Failed to parse request: " << text;
      return {};
    }
    EXPECT_EQ(result->outcome, ApplyOutcome::Applied) << text;
    return std::move(result->bytes);
  }

  // Whether `text` builds nothing: it does not parse, or it is Refused.
  bool refused(const std::vector<uint8_t>& initial, std::string_view text) {
    const auto result = applyText(initial, text);
    return not result.has_value() or result->outcome == ApplyOutcome::Refused;
  }

  const flatbuffers::Table* subtable(const flatbuffers::Table* table,
                                     const char* objectName,
                                     const char* name) const {
    return table->GetPointer<const flatbuffers::Table*>(
        field(objectName, name));
  }

  const flatbuffers::Table* holderAt(const flatbuffers::Table* root,
                                     uint32_t index) const {
    const auto* holders = root->GetPointer<
        const flatbuffers::Vector<flatbuffers::Offset<flatbuffers::Table>>*>(
        field("copy_test.Root", "holders"));
    return (holders != nullptr and index < holders->size())
               ? holders->Get(index)
               : nullptr;
  }

  // Whether `pointer` is `alignment`-aligned from the start of `bytes`.
  static bool alignedIn(const std::vector<uint8_t>& bytes, const void* pointer,
                        size_t alignment) {
    return (static_cast<const uint8_t*>(pointer) - bytes.data()) % alignment
           == 0;
  }

  static void expectWide(const uint8_t* wide, int8_t a, int64_t b) {
    auto storedA = int8_t{0};
    auto storedB = int64_t{0};
    std::memcpy(&storedA, wide, sizeof(storedA));
    std::memcpy(&storedB, wide + 8, sizeof(storedB));
    EXPECT_EQ(storedA, a);
    EXPECT_EQ(storedB, b);
  }

  void expectLeaf(const flatbuffers::Table* leaf, int32_t x, int64_t w) const {
    ASSERT_NE(leaf, nullptr);
    EXPECT_EQ(leaf->GetField<int32_t>(field("copy_test.Leaf", "x"), 0), x);
    EXPECT_EQ(leaf->GetField<int64_t>(field("copy_test.Leaf", "w"), 0), w);
  }

  static const flatbuffers::Vector<flatbuffers::Offset<flatbuffers::Table>>*
  tables(const flatbuffers::Table* table, uint16_t field) {
    return table->GetPointer<
        const flatbuffers::Vector<flatbuffers::Offset<flatbuffers::Table>>*>(
        field);
  }

  // Expects `holder`, in `bytes`, to store `shape` as shapeJson wrote it, at
  // its alignment.
  void expectShape(const std::vector<uint8_t>& bytes,
                   const flatbuffers::Table* holder, Shape shape) const {
    SCOPED_TRACE(shapeName(shape));
    ASSERT_NE(holder, nullptr);
    const auto* h = "copy_test.Holder";
    switch (shape) {
      case Shape::kLeafMember:
      case Shape::kStructMember:
      case Shape::kStringMember:
      case Shape::kForcedMember: {
        const auto* value =
            holder->GetPointer<const uint8_t*>(field(h, "member"));
        ASSERT_NE(value, nullptr);
        const auto stored =
            holder->GetField<uint8_t>(field(h, "member_type"), 0);
        if (shape == Shape::kLeafMember) {
          EXPECT_EQ(stored, 1);
          expectLeaf(reinterpret_cast<const flatbuffers::Table*>(value), 2, 3);
        } else if (shape == Shape::kStructMember) {
          EXPECT_EQ(stored, 2);
          EXPECT_TRUE(alignedIn(bytes, value, 8));
          expectWide(value, 2, 3);
        } else if (shape == Shape::kStringMember) {
          EXPECT_EQ(stored, 3);
          EXPECT_EQ(reinterpret_cast<const flatbuffers::String*>(value)->str(),
                    "two");
        } else {
          EXPECT_EQ(stored, 4);
          EXPECT_TRUE(alignedIn(bytes, value, 16));
          EXPECT_EQ(flatbuffers::ReadScalar<int32_t>(value), 32);
        }
        return;
      }
      case Shape::kUnionVector: {
        const auto* types =
            holder->GetPointer<const flatbuffers::Vector<uint8_t>*>(
                field(h, "members_type"));
        const auto* members = tables(holder, field(h, "members"));
        ASSERT_NE(types, nullptr);
        ASSERT_NE(members, nullptr);
        ASSERT_EQ(types->size(), 3U);
        ASSERT_EQ(members->size(), 3U);
        EXPECT_EQ(types->Get(0), 1);
        EXPECT_EQ(types->Get(1), 2);
        EXPECT_EQ(types->Get(2), 3);
        expectLeaf(members->Get(0), 4, 5);
        const auto* wide = reinterpret_cast<const uint8_t*>(members->Get(1));
        EXPECT_TRUE(alignedIn(bytes, wide, 8));
        expectWide(wide, 6, 7);
        EXPECT_EQ(reinterpret_cast<const flatbuffers::String*>(members->Get(2))
                      ->str(),
                  "eight");
        return;
      }
      case Shape::kSpacedMember:
        EXPECT_EQ(holder->GetField<uint8_t>(field(h, "spaced_type"), 0), 3);
        expectLeaf(subtable(holder, h, "spaced"), 30, 31);
        return;
      case Shape::kString64: {
        const auto* text64 =
            holder->GetPointer<const flatbuffers::String*,
                               flatbuffers::uoffset64_t>(field(h, "text64"));
        ASSERT_NE(text64, nullptr);
        EXPECT_EQ(text64->str(), "nine");
        return;
      }
      case Shape::kOffset64Vector: {
        const auto* bytes64 =
            holder->GetPointer<const flatbuffers::Vector<uint8_t>*,
                               flatbuffers::uoffset64_t>(field(h, "bytes64"));
        ASSERT_NE(bytes64, nullptr);
        EXPECT_EQ(std::vector<uint8_t>(bytes64->begin(), bytes64->end()),
                  (std::vector<uint8_t>{10, 11, 12}));
        return;
      }
      case Shape::kVector64: {
        const auto* wides64 =
            holder->GetPointer<const uint8_t*, flatbuffers::uoffset64_t>(
                field(h, "wides64"));
        ASSERT_NE(wides64, nullptr);
        EXPECT_EQ(flatbuffers::ReadScalar<uint64_t>(wides64), 2U);
        EXPECT_TRUE(alignedIn(bytes, wides64 + 8, 8));
        expectWide(wides64 + 8, 13, 14);
        expectWide(wides64 + 24, 15, 16);
        return;
      }
      case Shape::kAlignedStructs: {
        const auto* wides =
            holder->GetPointer<const uint8_t*>(field(h, "wides"));
        ASSERT_NE(wides, nullptr);
        EXPECT_EQ(flatbuffers::ReadScalar<uint32_t>(wides), 1U);
        EXPECT_TRUE(alignedIn(bytes, wides + 4, 8));
        expectWide(wides + 4, 17, 18);
        return;
      }
      case Shape::kForcedStructs: {
        const auto* forced =
            holder->GetPointer<const uint8_t*>(field(h, "forced"));
        ASSERT_NE(forced, nullptr);
        EXPECT_EQ(flatbuffers::ReadScalar<uint32_t>(forced), 1U);
        EXPECT_TRUE(alignedIn(bytes, forced + 4, 16));
        EXPECT_EQ(flatbuffers::ReadScalar<int32_t>(forced + 4), 19);
        return;
      }
      case Shape::kForcedBytes: {
        const auto* aligned =
            holder->GetPointer<const flatbuffers::Vector<uint8_t>*>(
                field(h, "aligned"));
        ASSERT_NE(aligned, nullptr);
        EXPECT_TRUE(alignedIn(bytes, aligned->data(), 16));
        EXPECT_EQ(std::vector<uint8_t>(aligned->begin(), aligned->end()),
                  (std::vector<uint8_t>{20, 21, 22}));
        return;
      }
      case Shape::kNestedFlatbuffer: {
        const auto* nested =
            holder->GetPointer<const flatbuffers::Vector<uint8_t>*>(
                field(h, "nested"));
        ASSERT_NE(nested, nullptr);
        EXPECT_TRUE(alignedIn(bytes, nested->data(), 8));
        expectLeaf(flatbuffers::GetAnyRoot(nested->data()), 23, 24);
        return;
      }
      case Shape::kTables: {
        const auto* leaves = tables(holder, field(h, "leaves"));
        ASSERT_NE(leaves, nullptr);
        ASSERT_EQ(leaves->size(), 1U);
        expectLeaf(leaves->Get(0), 25, 26);
        return;
      }
    }
  }

  void expectHolder(const std::vector<uint8_t>& bytes,
                    const flatbuffers::Table* holder,
                    std::span<const Shape> shapes) const {
    for (const auto shape : shapes) {
      expectShape(bytes, holder, shape);
    }
  }

  // Expects the holders in `bytes` to store what source() wrote into them.
  void expectHoldersKept(const std::vector<uint8_t>& bytes,
                         std::span<const Shape> holder,
                         std::span<const Shape> first,
                         std::span<const Shape> second) const {
    ASSERT_FALSE(bytes.empty());
    const auto* root = flatbuffers::GetAnyRoot(bytes.data());
    expectHolder(bytes, subtable(root, "copy_test.Root", "holder"), holder);
    expectHolder(bytes, holderAt(root, 0), first);
    expectHolder(bytes, holderAt(root, 1), second);
  }

  void expectEveryHolderKept(const std::vector<uint8_t>& bytes) const {
    expectHoldersKept(bytes, everyShapeHolding(Shape::kLeafMember),
                      everyShapeHolding(Shape::kStructMember),
                      everyShapeHolding(Shape::kStringMember));
  }

  std::vector<uint8_t> bfbs_;
  const reflection::Schema* schema_{nullptr};
};

// Requests that copy every holder, along the request's path or below it. The
// fillers vary where everything built after them falls, so a copy that does not
// align what it builds cannot land aligned by chance for all of them.
std::vector<std::string> copyingRequests() {
  auto requests = std::vector<std::string>{
      "put i 2",
      "delete i",
      "delete filler",
      "put holder.n 5",
      "put holders[1].n 5",
      "put holders[0].leaves[0].x 25",
      "delete holder.n",
      "delete holders[1].n",
      "patch holder { put n 5 }",
      "patch {}",
  };
  for (auto length = 0; length < 8; length++) {
    requests.push_back("put filler \"" + std::string(length, 'p') + "\"");
  }
  return requests;
}

class ShapeCopyTest : public TableCopyTest,
                      public ::testing::WithParamInterface<Shape> {};

// Each shape alone, so that one the copy misreads is named by its case rather
// than hidden behind another that faults first.
TEST_P(ShapeCopyTest, KeepsAShapeARequestDoesNotEdit) {
  const auto shapes = std::array{GetParam()};
  const auto initial = source(shapes, shapes, shapes);
  ASSERT_TRUE(verifyBuffer(schema_, initial));
  for (const auto& request : copyingRequests()) {
    SCOPED_TRACE(request);
    expectHoldersKept(applied(initial, request), shapes, shapes, shapes);
  }
}

TEST_P(ShapeCopyTest, KeepsAShapeInAPutsValue) {
  const auto shapes = std::array{GetParam()};
  const auto initial = source(shapes, shapes, shapes);
  const auto value = buffer("copy_test.Holder", holderJson(shapes));
  auto result = applyPut(initial, {field("copy_test.Root", "holder")}, value);
  ASSERT_EQ(result.outcome, ApplyOutcome::Applied);
  expectHoldersKept(result.bytes, shapes, shapes, shapes);

  result = applyPut(initial, {field("copy_test.Root", "holders"), 2}, value);
  ASSERT_EQ(result.outcome, ApplyOutcome::Applied);
  expectHoldersKept(result.bytes, shapes, shapes, shapes);
  expectHolder(result.bytes,
               holderAt(flatbuffers::GetAnyRoot(result.bytes.data()), 2),
               shapes);
}

INSTANTIATE_TEST_SUITE_P(EveryShape, ShapeCopyTest,
                         ::testing::ValuesIn(kEveryShape),
                         [](const ::testing::TestParamInfo<Shape>& info) {
                           return std::string(shapeName(info.param));
                         });

TEST_F(TableCopyTest, KeepsEveryFieldARequestDoesNotEdit) {
  const auto initial = source();
  ASSERT_TRUE(verifyBuffer(schema_, initial));
  expectEveryHolderKept(initial);
  for (const auto& request : copyingRequests()) {
    SCOPED_TRACE(request);
    expectEveryHolderKept(applied(initial, request));
  }
}

TEST_F(TableCopyTest, KeepsAFieldsAlignmentWhereverTheCopyPlacesIt) {
  const auto initial = source();
  for (auto length = 0; length < 16; length++) {
    const auto request = "put filler \"" + std::string(length, 'p') + "\"";
    SCOPED_TRACE(request);
    const auto bytes = applied(initial, request);
    expectEveryHolderKept(bytes);
    // A field written after the filler, along the path, as well.
    expectEveryHolderKept(applied(bytes, "put holder.n 7"));
  }
}

// A put's value is copied into the buffer as the source is. The values are
// built by hand: FlatBuffers' `Parser::Deserialize` refuses a schema holding a
// vector64 of structs, so the request parser cannot parse a table value for
// one.
TEST_F(TableCopyTest, KeepsEveryFieldOfAPutsValue) {
  const auto initial = source();
  for (const auto member :
       {Shape::kLeafMember, Shape::kStructMember, Shape::kStringMember}) {
    SCOPED_TRACE(shapeName(member));
    const auto shapes = everyShapeHolding(member);
    const auto value = buffer("copy_test.Holder", holderJson(shapes));
    auto result = applyPut(initial, {field("copy_test.Root", "holder")}, value);
    ASSERT_EQ(result.outcome, ApplyOutcome::Applied);
    const auto* root = flatbuffers::GetAnyRoot(result.bytes.data());
    expectHolder(result.bytes, subtable(root, "copy_test.Root", "holder"),
                 shapes);

    result = applyPut(initial, {field("copy_test.Root", "holders"), 2}, value);
    ASSERT_EQ(result.outcome, ApplyOutcome::Applied);
    expectEveryHolderKept(result.bytes);
    expectHolder(result.bytes,
                 holderAt(flatbuffers::GetAnyRoot(result.bytes.data()), 2),
                 shapes);
  }
}

// A Holder whose `member` stores a Leaf under `member`, built by hand: the
// JSON parser writes neither NONE nor an undeclared member with a payload.
std::vector<uint8_t> holderStoring(const reflection::Schema* schema,
                                   uint8_t member, bool rootIsHolder) {
  const auto* holderDef = schema->objects()->LookupByKey("copy_test.Holder");
  const auto* leafDef = schema->objects()->LookupByKey("copy_test.Leaf");
  const auto* rootDef = schema->objects()->LookupByKey("copy_test.Root");
  flatbuffers::FlatBufferBuilder fbb;
  auto leafStart = fbb.StartTable();
  fbb.AddElement<int32_t>(leafDef->fields()->LookupByKey("x")->offset(), 9);
  const auto leaf = fbb.EndTable(leafStart);
  auto holderStart = fbb.StartTable();
  fbb.AddElement<uint8_t>(
      holderDef->fields()->LookupByKey("member_type")->offset(), member);
  fbb.AddOffset(holderDef->fields()->LookupByKey("member")->offset(),
                flatbuffers::Offset<void>(leaf));
  fbb.AddElement<int32_t>(holderDef->fields()->LookupByKey("n")->offset(), 1);
  const auto holder = fbb.EndTable(holderStart);
  if (rootIsHolder) {
    fbb.Finish(flatbuffers::Offset<void>(holder));
  } else {
    auto rootStart = fbb.StartTable();
    fbb.AddOffset(rootDef->fields()->LookupByKey("holder")->offset(),
                  flatbuffers::Offset<void>(holder));
    fbb.AddElement<int32_t>(rootDef->fields()->LookupByKey("i")->offset(), 1);
    fbb.Finish(flatbuffers::Offset<void>(fbb.EndTable(rootStart)));
  }
  return {fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize()};
}

constexpr auto kNone = uint8_t{0};
constexpr auto kUndeclared = uint8_t{99};

// No copy can come out as it went in for a payload whose layout the schema
// does not give, so a request that copies one is refused, below the path
// as along it; a request that replaces or removes it never reads it.
TEST_F(TableCopyTest, RefusesToCopyAUnionPayloadWithNoMemberToReadItAs) {
  for (const auto member : {kNone, kUndeclared}) {
    SCOPED_TRACE(static_cast<int>(member));
    const auto initial = holderStoring(schema_, member, false);
    ASSERT_TRUE(verifyBuffer(schema_, initial) == (member == kNone));
    for (const auto* request :
         {"put i 2", "delete i", "put holder.n 2", "delete holder.n",
          "patch {}", "patch holder { put n 2 }"}) {
      EXPECT_TRUE(refused(initial, request)) << request;
    }
    const auto deleted = applied(initial, "delete holder");
    EXPECT_EQ(subtable(flatbuffers::GetAnyRoot(deleted.data()),
                       "copy_test.Root", "holder"),
              nullptr);
    const auto cleared = applied(initial, "delete holder.member");
    EXPECT_EQ(subtable(flatbuffers::GetAnyRoot(cleared.data()),
                       "copy_test.Root", "holder")
                  ->GetField<int32_t>(field("copy_test.Holder", "n"), 0),
              1);
    const auto switched = applied(initial, "put holder.member.Leaf.x 4");
    const auto* holder = subtable(flatbuffers::GetAnyRoot(switched.data()),
                                  "copy_test.Root", "holder");
    expectLeaf(subtable(holder, "copy_test.Holder", "member"), 4, 0);
  }
}

TEST_F(TableCopyTest, RefusesAPutWhoseValueHoldsAUnionPayloadItCannotRead) {
  const auto initial = source();
  for (const auto member : {kNone, kUndeclared}) {
    const auto value = holderStoring(schema_, member, true);
    EXPECT_EQ(
        applyPut(initial, {field("copy_test.Root", "holder")}, value).outcome,
        ApplyOutcome::Refused);
  }
  EXPECT_EQ(applyPut(initial, {field("copy_test.Root", "holder")},
                     holderStoring(schema_, 1, true))
                .outcome,
            ApplyOutcome::Applied);
}

// The rebuild keeps only the elements both halves of a vector of unions hold,
// so a vector whose halves differ in length cannot be copied whole.
TEST_F(TableCopyTest, RefusesToCopyAVectorOfUnionsWithUnpairedDiscriminants) {
  const auto* holderDef = object("copy_test.Holder");
  const auto* rootDef = object("copy_test.Root");
  flatbuffers::FlatBufferBuilder fbb;
  const auto text = fbb.CreateString("t");
  const auto values = fbb.CreateVector(
      std::vector<flatbuffers::Offset<void>>{text.Union(), text.Union()});
  const auto types = fbb.CreateVector(std::vector<uint8_t>{3});
  auto holderStart = fbb.StartTable();
  fbb.AddOffset(holderDef->fields()->LookupByKey("members")->offset(), values);
  fbb.AddOffset(holderDef->fields()->LookupByKey("members_type")->offset(),
                types);
  const auto holder = fbb.EndTable(holderStart);
  auto rootStart = fbb.StartTable();
  fbb.AddOffset(rootDef->fields()->LookupByKey("holder")->offset(),
                flatbuffers::Offset<void>(holder));
  fbb.Finish(flatbuffers::Offset<void>(fbb.EndTable(rootStart)));
  const auto initial = std::vector<uint8_t>(
      fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize());
  EXPECT_TRUE(refused(initial, "put i 2"));
  EXPECT_TRUE(refused(initial, "put holder.n 2"));
  EXPECT_TRUE(refused(initial, "delete holder.members[0]"));
  EXPECT_FALSE(refused(initial, "delete holder.members"));
}

// A put of an offset64 string or 32-bit-length vector writes it through a
// 64-bit offset. Editing an element of one, or writing a vector64, is
// refused: those are read and written through 32-bit accessors.
TEST_F(TableCopyTest, WritesOrRefusesAFieldHeldThroughA64BitOffset) {
  const auto initial = source();
  const auto* h = "copy_test.Holder";

  auto bytes = applied(initial, R"(put holder.text64 "ten")");
  const auto* holder = subtable(flatbuffers::GetAnyRoot(bytes.data()),
                                "copy_test.Root", "holder");
  EXPECT_EQ(
      (holder
           ->GetPointer<const flatbuffers::String*, flatbuffers::uoffset64_t>(
               field(h, "text64"))
           ->str()),
      "ten");

  bytes = applied(initial, "put holder.bytes64 [7, 8]");
  holder = subtable(flatbuffers::GetAnyRoot(bytes.data()), "copy_test.Root",
                    "holder");
  const auto* bytes64 =
      holder->GetPointer<const flatbuffers::Vector<uint8_t>*,
                         flatbuffers::uoffset64_t>(field(h, "bytes64"));
  EXPECT_EQ(std::vector<uint8_t>(bytes64->begin(), bytes64->end()),
            (std::vector<uint8_t>{7, 8}));

  bytes = applied(initial, "delete holder.wides64");
  holder = subtable(flatbuffers::GetAnyRoot(bytes.data()), "copy_test.Root",
                    "holder");
  EXPECT_FALSE(holder->CheckField(field(h, "wides64")));

  EXPECT_TRUE(refused(initial, "put holder.bytes64[0] 1"));
  EXPECT_TRUE(refused(initial, "put holder.bytes64[5] 1"));
  EXPECT_TRUE(refused(initial, "delete holder.bytes64[0]"));
  EXPECT_TRUE(refused(initial, R"(put holder.wides64 [{"a": 1, "b": 2}])"));
  EXPECT_TRUE(refused(initial, "delete holder.wides64[0]"));
}

// A vector of structs held through a 64-bit offset with a 32-bit length is
// written whole through that offset, as a vector of scalars is; its elements
// are read through 32-bit accessors, so a request naming one is refused.
TEST_F(TableCopyTest, WritesAVectorOfStructsHeldThroughA64BitOffsetWhole) {
  const auto initial = source();
  const auto bytes =
      applied(initial, R"(put holder.wides32 [{"a": 3, "b": 4}, {"a": 5, )"
                       R"("b": -6}])");
  const auto* holder = subtable(flatbuffers::GetAnyRoot(bytes.data()),
                                "copy_test.Root", "holder");
  const auto* wides =
      holder->GetPointer<const uint8_t*, flatbuffers::uoffset64_t>(
          field("copy_test.Holder", "wides32"));
  ASSERT_NE(wides, nullptr);
  EXPECT_EQ(flatbuffers::ReadScalar<uint32_t>(wides), 2U);
  EXPECT_TRUE(alignedIn(bytes, wides + 4, 8));
  expectWide(wides + 4, 3, 4);
  expectWide(wides + 20, 5, -6);
  EXPECT_TRUE(refused(bytes, "put holder.wides32[0].a 1"));
  EXPECT_TRUE(refused(bytes, "put holder.wides32[2] {}"));
  EXPECT_TRUE(refused(bytes, "delete holder.wides32[0]"));
}

// A vector of structs keeps its struct's alignment, 8 bytes for one holding
// an int64 and 16 for one forced there, wherever an edit of it lands.
TEST_F(TableCopyTest, KeepsAVectorOfStructsAlignedWhereAnEditRebuildsIt) {
  const auto shapes = std::array{Shape::kAlignedStructs, Shape::kForcedStructs};
  const auto initial = source(shapes, shapes, shapes);
  const auto expectAligned = [&](const std::vector<uint8_t>& bytes,
                                 const char* name, size_t alignment) {
    const auto* holder = subtable(flatbuffers::GetAnyRoot(bytes.data()),
                                  "copy_test.Root", "holder");
    const auto* vector = holder->GetPointer<const flatbuffers::VectorOfAny*>(
        field("copy_test.Holder", name));
    ASSERT_NE(vector, nullptr) << name;
    if (vector->size() != 0) {
      EXPECT_TRUE(alignedIn(bytes, vector->Data(), alignment)) << name;
    }
  };
  for (auto length = 0; length < 16; length++) {
    const auto filler = "put filler \"" + std::string(length, 'p') + "\"";
    SCOPED_TRACE(filler);
    const auto padded = applied(initial, filler);
    for (const auto* request : {
             "put holder.wides[0].a 9",
             "put holder.wides[3] {}",
             R"(put holder.wides [{"a": 1, "b": 2}])",
             "delete holder.wides[0]",
             "put holder.forced[0].a 9",
             R"(put holder.forced[2] {"a": 4})",
             R"(put holder.forced [{"a": 1}, {"a": 2}])",
         }) {
      SCOPED_TRACE(request);
      const auto bytes = applied(padded, request);
      expectAligned(bytes, "wides", 8);
      expectAligned(bytes, "forced", 16);
    }
  }
}

// A vector the schema asks to align keeps that alignment when an edit
// rebuilds it, as it does when it is copied.
TEST_F(TableCopyTest, KeepsAVectorsAlignmentWhereAnEditRebuildsIt) {
  const auto shapes = std::array{Shape::kForcedBytes};
  const auto initial = source(shapes, shapes, shapes);
  for (auto length = 0; length < 16; length++) {
    const auto filler = "put filler \"" + std::string(length, 'p') + "\"";
    SCOPED_TRACE(filler);
    const auto padded = applied(initial, filler);
    for (const auto* request :
         {"put holder.aligned[1] 9", "put holder.aligned[4] 9",
          "delete holder.aligned[0]", "put holder.aligned [1, 2]"}) {
      SCOPED_TRACE(request);
      const auto bytes = applied(padded, request);
      const auto* holder = subtable(flatbuffers::GetAnyRoot(bytes.data()),
                                    "copy_test.Root", "holder");
      const auto* aligned =
          holder->GetPointer<const flatbuffers::Vector<uint8_t>*>(
              field("copy_test.Holder", "aligned"));
      ASSERT_NE(aligned, nullptr);
      EXPECT_TRUE(alignedIn(bytes, aligned->data(), 16));
    }
  }
}

// A Root whose holder stores a discriminant beside `field` but not the union
// it names.
std::vector<uint8_t> loneDiscriminant(const reflection::Schema* schema,
                                      bool vector) {
  const auto* holderDef = schema->objects()->LookupByKey("copy_test.Holder");
  const auto* rootDef = schema->objects()->LookupByKey("copy_test.Root");
  flatbuffers::FlatBufferBuilder fbb;
  const auto types = fbb.CreateVector(std::vector<uint8_t>{1});
  auto holderStart = fbb.StartTable();
  if (vector) {
    fbb.AddOffset(holderDef->fields()->LookupByKey("members_type")->offset(),
                  types);
  } else {
    fbb.AddElement<uint8_t>(
        holderDef->fields()->LookupByKey("member_type")->offset(), 1);
  }
  const auto holder = fbb.EndTable(holderStart);
  auto rootStart = fbb.StartTable();
  fbb.AddOffset(rootDef->fields()->LookupByKey("holder")->offset(),
                flatbuffers::Offset<void>(holder));
  fbb.Finish(flatbuffers::Offset<void>(fbb.EndTable(rootStart)));
  return {fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize()};
}

// The builders write a discriminant only beside its union, so a copy would
// lose one stored alone. A request that writes the union writes both.
TEST_F(TableCopyTest, RefusesToCopyADiscriminantWithoutItsUnion) {
  for (const auto vector : {false, true}) {
    SCOPED_TRACE(vector ? "members_type" : "member_type");
    const auto initial = loneDiscriminant(schema_, vector);
    ASSERT_TRUE(verifyBuffer(schema_, initial));
    EXPECT_TRUE(refused(initial, "put i 2"));
    EXPECT_TRUE(refused(initial, "patch {}"));
    EXPECT_FALSE(refused(initial, "delete holder"));
  }
  const auto bytes =
      applied(loneDiscriminant(schema_, false), "put holder.member.Leaf.x 3");
  const auto* holder = subtable(flatbuffers::GetAnyRoot(bytes.data()),
                                "copy_test.Root", "holder");
  expectLeaf(subtable(holder, "copy_test.Holder", "member"), 3, 0);
  const auto grown = applied(loneDiscriminant(schema_, true),
                             "put holder.members[0].Leaf.x 3");
  holder = subtable(flatbuffers::GetAnyRoot(grown.data()), "copy_test.Root",
                    "holder");
  const auto* members = tables(holder, field("copy_test.Holder", "members"));
  ASSERT_NE(members, nullptr);
  ASSERT_EQ(members->size(), 1U);
  expectLeaf(members->Get(0), 3, 0);
}

// A copy driven by the schema cannot keep a field the schema does not
// declare, as a buffer written with a newer schema may store.
TEST_F(TableCopyTest, RefusesToCopyAFieldTheSchemaDoesNotDeclare) {
  const auto* rootDef = object("copy_test.Root");
  auto newest = flatbuffers::voffset_t{0};
  for (const auto* fieldDef : *rootDef->fields()) {
    newest = std::max(newest, fieldDef->offset());
  }
  const auto* holderDef = object("copy_test.Holder");
  flatbuffers::FlatBufferBuilder fbb;
  auto holderStart = fbb.StartTable();
  fbb.AddElement<int32_t>(holderDef->fields()->LookupByKey("n")->offset(), 1);
  const auto holder = fbb.EndTable(holderStart);
  auto rootStart = fbb.StartTable();
  fbb.AddElement<int32_t>(rootDef->fields()->LookupByKey("i")->offset(), 1);
  fbb.AddOffset(rootDef->fields()->LookupByKey("holder")->offset(),
                flatbuffers::Offset<void>(holder));
  fbb.AddElement<int32_t>(static_cast<flatbuffers::voffset_t>(newest + 2), 7);
  fbb.Finish(flatbuffers::Offset<void>(fbb.EndTable(rootStart)));
  const auto initial = std::vector<uint8_t>(
      fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize());
  ASSERT_TRUE(verifyBuffer(schema_, initial));
  EXPECT_TRUE(refused(initial, "put i 2"));
  EXPECT_TRUE(refused(initial, "put holder.n 2"));
  EXPECT_TRUE(refused(initial, "patch {}"));
}

// FlatBuffers' reflection `Verify` reads a 64-bit offset through 32 bits,
// asserts in a debug build on a vector64, and looks a union member up by its
// position; the source holds each of those and verifies.
TEST_F(TableCopyTest, VerifiesEachFieldAsItsTypeDeclares) {
  const auto initial = source();
  EXPECT_TRUE(verifyBuffer(schema_, initial));
  for (const auto length :
       {size_t{0}, size_t{3}, size_t{7}, initial.size() / 2}) {
    EXPECT_FALSE(verifyBuffer(schema_, std::span{initial.data(), length}))
        << length;
  }
}

// A vtable entry pointing a discriminant past the buffer must be refused
// before the discriminant is read.
TEST_F(TableCopyTest, RefusesADiscriminantOutsideTheBuffer) {
  const auto shapes = std::array{Shape::kUnionVector, Shape::kLeafMember};
  for (const auto* name : {"member_type", "members_type"}) {
    SCOPED_TRACE(name);
    auto bytes = source(shapes, shapes, shapes);
    const auto* root = flatbuffers::GetAnyRoot(bytes.data());
    const auto* holder = subtable(root, "copy_test.Root", "holder");
    const auto* data = reinterpret_cast<const uint8_t*>(holder);
    const auto* vtable =
        data - flatbuffers::ReadScalar<flatbuffers::soffset_t>(data);
    const auto entry = static_cast<size_t>(vtable - bytes.data())
                       + field("copy_test.Holder", name);
    ASSERT_NE(flatbuffers::ReadScalar<flatbuffers::voffset_t>(&bytes[entry]),
              0);
    flatbuffers::WriteScalar<flatbuffers::voffset_t>(&bytes[entry], 0xfff0);
    EXPECT_FALSE(verifyBuffer(schema_, bytes));
  }
}

// A member the union does not declare, inside a vector of unions, has no
// layout to verify it against.
TEST_F(TableCopyTest, RefusesAnUndeclaredMemberInAVectorOfUnions) {
  const auto* holderDef = object("copy_test.Holder");
  const auto* rootDef = object("copy_test.Root");
  flatbuffers::FlatBufferBuilder fbb;
  const auto text = fbb.CreateString("t");
  const auto values =
      fbb.CreateVector(std::vector<flatbuffers::Offset<void>>{text.Union()});
  const auto types = fbb.CreateVector(std::vector<uint8_t>{99});
  auto holderStart = fbb.StartTable();
  fbb.AddOffset(holderDef->fields()->LookupByKey("members")->offset(), values);
  fbb.AddOffset(holderDef->fields()->LookupByKey("members_type")->offset(),
                types);
  const auto holder = fbb.EndTable(holderStart);
  auto rootStart = fbb.StartTable();
  fbb.AddOffset(rootDef->fields()->LookupByKey("holder")->offset(),
                flatbuffers::Offset<void>(holder));
  fbb.Finish(flatbuffers::Offset<void>(fbb.EndTable(rootStart)));
  const auto initial = std::vector<uint8_t>(
      fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize());
  EXPECT_FALSE(verifyBuffer(schema_, initial));
  EXPECT_TRUE(refused(initial, "put i 2"));
  EXPECT_FALSE(refused(initial, "delete holder.members[0]"));
}

}  // namespace
