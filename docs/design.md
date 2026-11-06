# Design notes

## Overview

`flatbuffers-request` has two halves:

1. **Parse** ([src/FlatbufferRequestParse.cpp](../src/FlatbufferRequestParse.cpp)) —
   turns request text into a serialized `Request` flatbuffer (schema in
   [schemas/fbrequest/FlatbufferRequest.fbs](../schemas/fbrequest/FlatbufferRequest.fbs)).
   Each operation stores an **offset path** (a `[uint32]` vector of vtable
   offsets / vector indices / union discriminants) plus, for `put`, a
   flexbuffer-encoded payload.

2. **Apply** ([src/FlatbufferRequestApply.cpp](../src/FlatbufferRequestApply.cpp)) —
   walks the target reflection schema and rebuilds the buffer, copying every
   field that is unchanged and substituting the field named by the offset path.
   Everything the path does not reach is copied by the library's own
   reflection-driven copy ([src/TableCopy.cpp](../src/TableCopy.cpp)); see
   [Copying what a request does not edit](#copying-what-a-request-does-not-edit).

Splitting parse from apply means a request can be serialized, logged, sent over
a wire, or replayed — apply is a pure function of `(schema, source, request)`.

## Reporting what was written

Apply resolves a request's path against the schema and the source, and checks
its payload against the type it is written as, before it builds anything into
the caller's builder, and answers `Applied`, `NotStored` or `Refused`
(`tryApplyRequest`). Only an `Applied` request builds into the caller's
builder, so a request that would change nothing leaves it as it was, and the
caller can tell a delete of something absent (`NotStored`, which an idempotent
caller treats as success) from a request no buffer could serve (`Refused`).
The split follows what each half can know: the parser refuses what the schema
alone decides -- a struct's members, an array's length -- and the apply
decides what depends on the buffer.

## Why offset paths, not field names

The parsed `Request` records numeric vtable offsets rather than field names, so
apply never re-parses identifiers and the request is a compact binary. The
schema is still required at apply time (to know field types and rebuild the
table), but path resolution is a cheap integer walk.

## Structs and arrays

A struct is stored inline in whatever holds it, with no vtable and no offset of
its own, so its members are all always present and the whole of it is rewritten
whenever any part is. Apply therefore builds a struct as a byte **image** — the
stored bytes where the buffer has them, zeroes where it does not — writes the
payload into that image, and pushes it inline.

- **`put` reaches a struct member** (`put scalars.i8 7`), a whole struct
  (`put scalars {...}`, a map naming any subset of members, the rest keeping
  what they had), and a struct the buffer does not store yet (the unnamed
  members start from zero).
- **`delete` of a struct member is refused**: a struct has no way to record
  that a member is absent, so there is nothing for a delete to do.
- **Fixed-size arrays of scalars** inside a struct take a put by element
  (`put scalars.i32_array[3] 9`) or whole (`put scalars.i32_array [7, 8]`,
  leaving the elements the payload does not reach). An array cannot grow or
  shrink, so a payload longer than `N` is refused.
- **A struct put writes what its payload names, or is refused.** A map
  naming no member (`put scalars {}`), or naming any member the struct does
  not declare (`put scalars {"i8": 1, "zzz": 2}`), an empty array, an array
  longer than its length, and any of these as the value of a member a map
  names (`put nested {"inner": {}, "tail": 77}`), are refused rather than
  written in part or not at all. So is a member whose value is of the wrong
  kind: a non-map for a nested struct, a non-array for an array, a string,
  map or array for a scalar. The parser decides all of this from the schema
  alone; the apply refuses a hand-built request carrying one.

A `put` is REFUSED, rather than served wrongly, for:

- an element of a vector or array **of structs** (`put struct_vector[0].i8 5`,
  `put nested.structs[0].a 2`) and a whole vector or array of structs, and a
  `patch` of one, whose operations would be puts into it. A struct element
  is inline: apply builds no offset for one while the table builder still
  consumes one, which would read a neighbouring field's offset.
- a whole vector of **unions**, whose element types live in a second vector
  that a JSON array cannot carry.
- a payload of the wrong kind for its field — a string where a number belongs
  (so an enum takes its ordinal, never its name, which would read as 0), a
  list or a map where a scalar belongs, or a non-map for a struct -- or out of
  its range: an integer past its width, a finite float past a float32's. See
  [grammar.md](grammar.md#values).

The flexbuffer payloads are read by the library's own value reader rather
than FlatBuffers' flexbuffer parser, which reads a numeral past the signed
64-bit range as 0: a uint64 above 2^63 - 1 would be written as 0, and an
out-of-range value could not be told from a 0 the request spelled.

A `delete` is unaffected by those refusals: removing a whole element is well
defined where writing one is not, so `delete struct_vector[0]` still works.

A struct payload is carried as a **flexbuffer map**, not as a table: a struct
cannot be a parser root type, and apply writes its members by name into the
image.

## Coupling to FlatBuffers internals

Apply and parse use FlatBuffers surface that is **not** part of its stable
public API: the `Parser`, its `builder_` / `flex_builder_` members,
`GetFieldT` / `GetFieldI` / `GetFieldS`, `GetTypeSize`, `UnionTypeFieldSuffix`,
and the `reflection::*` generated structs.

Consequences:

- The library must be **compiled against the same FlatBuffers version** its
  generated header was produced with (the generated header's `static_assert`
  enforces this), and the FlatBuffers types in its public API make that a hard
  ABI contract for consumers too.
- A FlatBuffers release that reshuffles those internals can break the build even
  though the request logic is unchanged. Treat a major FlatBuffers bump as a
  reason for a new release here, not a silent rebuild. The tested floor is
  recorded in [vcpkg.json](../vcpkg.json).

Severing this coupling (depending only on the stable reflection API) is possible
but a larger rework; it is intentionally deferred.

## Copying what a request does not edit

A request rebuilds the tables its path steps through and copies every other
field the buffer stores, and a put of a table copies its value into the
buffer the same way. FlatBuffers' own `CopyTable` cannot do either faithfully:
it reads every union member as a table (a struct, a string, NONE or an
undeclared member included), copies a vector of unions as offsets into the
buffer it copies from, reads a string or vector held through a 64-bit offset
through 32 bits, lays out a vector of structs at 4 bytes whatever the struct's
alignment, and aligns no vector past its element. So the library copies with
its own reflection-driven copy, which reads each field as its type declares
it:

- a union member by its discriminant's VALUE, as the table, struct or string
  it holds, in a union field and in each element of a vector of unions;
- a string or vector held through a 64-bit offset through 64 bits, and a
  `vector64`'s 64-bit length. The caller's builder is a 32-bit
  `FlatBufferBuilder`, which has no separate 64-bit region, so the copied
  object is placed where the copy reaches it and referred to through a 64-bit
  offset, as FlatBuffers' own parser writes such a field into its 32-bit
  builder. A buffer under 2 GiB reaches every object either way;
- a vector of structs at the struct's alignment, and a vector at what its
  `force_align`, `nested_flatbuffer` or `flexbuffer` attribute asks. These
  attributes reach a serialized schema only when it is built with
  `flatc --bfbs-builtins`; without them the schema does not say, and a
  vector is aligned to its element. A nested flatbuffer is aligned to the
  widest alignment the schema declares anywhere (a struct's, or a field's
  `force_align`), since the alignment its buffer was built with is not
  recorded.

Some payloads have no layout the schema gives, so no copy of them can come
out as it went in: a union payload stored under NONE or under a member its
union does not declare (which a buffer written with a newer schema may hold),
and a vector of unions whose discriminants do not pair one to one with its
elements. Two more cannot be kept by a copy that rebuilds tables field by
field from the schema: a discriminant stored without the union it names,
which the builders write only beside its value, and a field at a vtable
offset the schema does not declare. A request that would copy any of these
is `Refused` before anything is built.
One that does not copy it -- a delete of the union, the element or a table
holding it, or a put switching the union to another member -- is applied.

A put's table value is verified by the library's own verifier, which reads
the same field kinds the copy does, rather than FlatBuffers' reflection
`Verify`. That one reads a 64-bit offset through 32 bits, asserts in a debug
build on a `vector64`, and looks a union member up by its position among the
union's values. `verifyBuffer` offers the same verifier to a caller, and the
`fbrequest` tool verifies its input with it.

A put that names a field held through a 64-bit offset is written through one
where the field's length is 32 bits (`offset64` strings and vectors, whole).
An element of such a vector, and a `vector64` written whole or by element, are
refused: the rebuilds of a vector read and write their elements through
32-bit accessors.

## Layout

```
include/fbrequest/   public headers (FlatbufferRequest.hpp)
src/                 apply + parse + StringScan (private scan helpers)
schemas/fbrequest/   the Request schema
test/                GoogleTest suite + vendored reflection matchers
test/matchers/       schema-aware FlatBuffer/Flexbuffer equality matchers
```

The build uses the [`targets`](https://github.com/alexames/targets) CMake helper
library (`flatbuffer_cpp_library`, `cpp_library`, `cpp_test`), matching the
Composer engine it was extracted from.
