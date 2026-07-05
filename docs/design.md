# Design notes

## Overview

`flatbuffers-request` has two halves:

1. **Parse** ([src/FlatbufferRequestParse.cpp](../src/FlatbufferRequestParse.cpp)) —
   turns request text into a serialized `Request` flatbuffer (schema in
   [schemas/fbrequest/FlatbufferRequest.fbs](../schemas/fbrequest/FlatbufferRequest.fbs)).
   Each operation stores an **offset path** (a `[uint16]` vector of vtable
   offsets / vector indices / union discriminants) plus, for `put`, a
   flexbuffer-encoded payload.

2. **Apply** ([src/FlatbufferRequestApply.cpp](../src/FlatbufferRequestApply.cpp)) —
   walks the target reflection schema and rebuilds the buffer, copying every
   field that is unchanged and substituting the field named by the offset path.
   It is a reflection-driven cousin of FlatBuffers' own `CopyTable`, with extra
   logic to insert, overwrite, or omit the targeted field.

Splitting parse from apply means a request can be serialized, logged, sent over
a wire, or replayed — apply is a pure function of `(schema, source, request)`.

## Why offset paths, not field names

The parsed `Request` records numeric vtable offsets rather than field names, so
apply never re-parses identifiers and the request is a compact binary. The
schema is still required at apply time (to know field types and rebuild the
table), but path resolution is a cheap integer walk.

## Structs and arrays

- **Structs** are inline, fixed-layout, and indivisible. A `put`/`delete` may
  replace or copy a whole struct field but cannot add or remove an individual
  struct member — such a `delete` is a documented no-op.
- **Fixed-size arrays** (`[T:N]`) are **not yet supported**; apply asserts if it
  reaches one. This is the main known gap.

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
