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
   It is a reflection-driven cousin of FlatBuffers' own `CopyTable`, with extra
   logic to insert, overwrite, or omit the targeted field.

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
public API: the `Parser`, its `builder_` / `flex_builder_` members, `CopyTable`,
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

Apply copies an unchanged sub-table with FlatBuffers' own `CopyTable`, which
resolves a union through `GetUnionType` without checking that the stored
discriminant names a table. A union stored with NONE or an undeclared
discriminant is handled where apply copies it itself -- in the table a
request edits -- but not inside a sub-table it hands to `CopyTable` whole.

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
