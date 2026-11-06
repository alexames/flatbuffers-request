# Changelog

All notable changes to this project are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- `tryApplyRequest`, which answers what applying a request came to as an
  `ApplyOutcome` beside the new root: `Applied`, `NotStored` (a delete names
  something the buffer does not store) or `Refused` (no buffer could serve the
  request). Nothing is built into the caller's builder for an outcome other
  than `Applied`. A patch is all or nothing: it answers `Refused` where any of
  its operations is refused, otherwise `NotStored` where any is.
- `verifyBuffer`, which verifies a buffer against a schema reading each field
  as its type declares it, where FlatBuffers' reflection `Verify` reads a
  string or vector held through a 64-bit offset through 32 bits, asserts in a
  debug build on a `vector64`, and looks a union member up by its position
  among the union's values rather than its value. The `fbrequest` tool
  verifies its input with it.
- Puts and patches on a vector of structs, which were refused. Each rebuilds
  the vector at its struct's alignment, or what `force_align` asks, and
  copies the rest of the table as any edit does:
  - `put struct_vector[1].i8 5` writes one member of an element, through
    nested structs and into one element of a fixed-length array in one
    (`put holder_vector[1].inner.arr[2] 7`). A nested struct or an array in
    the element takes a map or a list, as a struct field's does.
  - `put struct_vector[1] {"i8": 5}` writes the members a map names and
    keeps the rest of the stored element, as a struct field's put does. An
    empty map names the element alone: `put struct_vector[n] {}`, with `n`
    the vector's length, appends a zeroed element, and starts a vector the
    buffer does not store.
  - An index past the end grows the vector to reach it, as every other
    vector grows, the elements between zeroed. A vector grows, and a whole
    put builds one, only as far as its struct's size per element lets it fit
    in a flatbuffer.
  - `put struct_vector [{...}, ...]` replaces every element, starting the
    vector where it is not stored. Each element names every member of its
    struct exactly once: a nested struct by a map of the same form, and a
    fixed-length array of scalars by a list of exactly its length. A member
    missing, undeclared or named twice is refused, since there is no stored
    element for it to keep. `[]` stores the vector empty.
  - `patch struct_vector[1] { put i8 5 ; put i16 6 }` patches an element.
  - A struct holding a fixed-length array of structs is written around it:
    no element of that array is written, so a whole put of a vector of such
    structs takes only `[]`.

  Values are checked as every other value is (see `docs/grammar.md`). A
  delete of an element (`delete struct_vector[1]`) was already served, and
  a delete inside one is still refused.

### Changed
- A request that would copy, unedited, a union payload stored under NONE or
  under a member its union does not declare is `Refused`: the schema gives
  no layout to copy it by, and it was written as an empty table. So is one
  that would copy a vector of unions whose discriminants do not pair one to
  one with its elements, which was truncated to the shorter; one that would
  copy a discriminant stored without its union, which was dropped; and one
  that would copy a field the schema does not declare, which was dropped. A
  request that
  removes such a union or element, or switches the union to another member,
  does not copy it and is still applied. A put whose value holds one at any
  depth is refused too.
- `applyRequest` returns a null offset, and builds nothing, for a request that
  does not make the edit it names. It returned a rebuilt copy of the source,
  indistinguishable from a request that was applied. A caller passing its
  result straight to `Finish` must now check it first. Both functions are
  `[[nodiscard]]`. A caller that treats a
  delete of something absent as success uses `tryApplyRequest` and keeps its
  source on `NotStored`. The `fbrequest` tool exits with status 1 for such a
  request.
- A `delete` of something the buffer does not store is `NotStored` rather than
  a reported success: a field it does not store, an element past the end of a
  vector, an element of a vector it does not store, anything inside a table or
  union it does not store, and a union member other than the one it holds, or
  a field of one.
- The parser refuses a `delete` naming a member of a struct, an element of an
  array in one, or a member of an element of a vector of structs
  (`delete scalars.i8`, `delete struct_vector[0].i8`,
  `delete nested.structs[1]`). A struct cannot lose a member, so each was a
  no-op reported applied.
- The parser refuses a struct `put` whose value writes nothing or drops part of
  itself: a map naming no member (`put scalars {}`), a map naming any member
  the struct does not declare, which was silently dropped
  (`put scalars {"i8": 1, "zzz": 2}`), an empty array or one longer than its
  declared length, and any of these as the value of a member a map names
  (`put nested {"inner": {}, "tail": 77}`), which wrote the members beside it.
  A hand-built request carrying one is `Refused`.
- A value for a scalar, a string, a struct, or a vector or array of scalars is
  read by the library's own JSON value reader, and checked against what it
  is written as (see `docs/grammar.md`). Values that were accepted and are now
  refused: `null`, which wrote 0; a bool for an integer (`put i32 true` wrote
  1); a fraction or exponent for an integer (`put i32 1.5` wrote 1); an
  integer out of its field's range; a hex float (`0x1p3`); a comment; and a
  string holding a control character or text that is not UTF-8.

### Fixed
- A `put` growing a vector to just under the bound on its size was reported
  `Applied` with the vector left as stored. The bound was checked against the
  builder's size before anything was built, and again by the builder after
  the fields before the vector were copied into it, which fell back to the
  stored vector where the second check failed; in a patch the first check
  ignored the caller's builder altogether. The bound is now decided once,
  against the caller's builder as it is on entry, alone or in a patch, and a
  put it accepts grows the vector, for vectors of scalars, strings, tables,
  unions and structs alike. It counts the vectors a request grows together:
  a vector grown inside an element of another (`put
  object_vector[N].i32_vector[M] 7`), and the vectors the puts of one patch
  grow, each of which passed the bound alone.
- A whole-vector `put` of scalars, strings or tables was not bounded at all,
  and a payload whose elements share one string or table builds a copy per
  element, so a small payload could build past what a flatbuffer holds. It
  is bounded as a vector of structs already was.
- A hand-built `patch` whose path stops at a scalar, a string, a whole
  vector, a union with no member named, or anything in an array was applied
  as though each of its operations' paths continued its own: `patch i8
  { put <empty path> 5 }` wrote `i8`, `patch i32_vector { delete [0] }`
  removed an element, and an empty patch at any path, resolved or not, was a
  copy. Each is `Refused`, as the parser refuses its text. So is a request
  missing a member the parser always writes: an operation's path, which was
  read as the empty path (a put without one replaced the whole root, and
  inside `patch object { ... }` a delete without one removed `object`), or a
  patch's operations and their types (a patch without them was a copy).
- `copyTable` pooled strings whatever its `useStringPooling` argument said.
  It pools them only where asked.
- What a request does not edit was copied wrongly, or the copy faulted, for
  several kinds of field. The apply copied with FlatBuffers' `CopyTable`,
  which reads every union member as a table, copies a vector of unions as
  offsets into the source buffer, reads a string or vector held through a
  64-bit offset (`offset64`, `vector64`) through 32 bits, lays out a vector
  of structs at 4 bytes whatever the struct's alignment, and aligns no vector
  to what its `force_align`, `nested_flatbuffer` or `flexbuffer` attribute
  asks. The apply now copies with its own reflection-driven copy, which reads
  each field as its type declares it: a union member by its discriminant's
  value, as a table, a struct or a string, in a field and in a vector of
  unions; a 64-bit offset through 64 bits, written back through one; a vector
  of structs at the struct's alignment; and a vector at the alignment its
  attributes ask, where the serialized schema carries them (`flatc
  --bfbs-builtins`). This holds for the tables the request's path steps
  through, for every table below them, and for a put's value. A put of a
  table, or of a vector of tables, is verified the same way rather than with
  FlatBuffers' reflection `Verify`, which refused a union member whose value
  is not its position and asserted in a debug build on a `vector64`.
- A put of a string or a 32-bit-length vector held through a 64-bit offset
  (`put text64 "x"`, `put bytes64 [1, 2]`) wrote a 32-bit offset into the
  field. It is written through a 64-bit offset. An element of such a vector,
  and a `vector64` written whole or by element, are refused; a delete of the
  whole field still removes it.
- A put or delete whose source vector of scalars or of structs asks for an
  alignment (`force_align`) rebuilt the vector at its element's alignment. It
  keeps the alignment asked.
- A value of the wrong kind for a scalar was written: `put i8 [5]` wrote the
  list's length, and a list or map given as a vector element or a struct
  member did the same. Each is refused, wherever the scalar is.
- An integer out of range for its field wrapped (`put i8 300` wrote 44,
  `put ui8 -1` wrote 255), a numeral past the signed 64-bit range was written
  as 0, and a finite float past a float32's range was written as infinity.
  Each is refused. A bool takes `true`, `false`, 0 or 1, and no other number;
  an integer takes no fraction or exponent.
- A uint64 above 2^63 - 1 was written as 0 when put as a field, a struct
  member, an element of a vector or an array, or in a whole vector
  (`put ui64_vector [18446744073709551615]`). The whole range, 0 to
  2^64 - 1, is written, and a numeral past it is refused.
- A hand-built `put` whose payload was not what its field holds read it
  unchecked: a table payload that was empty, a flexbuffer or truncated read
  out of bounds, and so did a flexbuffer payload that did not verify. The
  apply now verifies a table payload against its table and a flexbuffer
  payload as a flexbuffer, and checks its kind, and a request carrying no
  operation, or no operations for a patch's types, is `Refused`.
- The serialized `Request`'s offset paths (`Put.offsets`, `Patch.offsets`,
  `Delete.offsets`) are `[uint32]` rather than `[uint16]`, so that any element
  index a vector can hold is carried whole. A request serialized by an earlier
  release does not apply with this one; re-parse its text.
- `delete <union>` (`delete number`) and `delete <union vector>[i]` now parse,
  and clear the union or remove the element, whichever member it holds. A
  path stopping at a union is still refused for a `put` and a `patch`, where
  nothing says which member the payload is.
- `delete <union>.<member>`, and `delete <union vector>[i].<member>`, where
  the buffer holds another member, are now a no-op, as a delete of a field of
  such a member already was. They removed the member the buffer did hold.
- A `patch` takes only a path a `put` could, since its operations may be
  puts: one stopping at a whole vector, into an array of structs, or naming
  no member of a union, is refused even when it holds only deletes.
- A hand-built `put` of a union-vector element naming a member that holds no
  table (`NONE`, or an undeclared value) leaves the vector as stored, as it
  does for a single union. It wrote an empty element.
- A `put` read the flatbuffer or flexbuffer its payload carries in place,
  even where it did not start on an 8-byte boundary: the request's payload
  vector guarantees 4, and each table of a vector-of-tables payload, stored as
  a flexbuffer blob, as little as 1. That is undefined behaviour (UBSan reports
  it for `put object_vector [{"i8": 9}]`) and faults on targets that trap on
  misaligned loads. Such a payload is now copied to aligned storage first.
- `parseFlatbufferRequest` read past the end of the `std::string_view` it was
  given. The FlatBuffers JSON and flexbuffer parsers it hands the payload to
  read to a NUL terminator, which a view need not have, so the bytes after the
  view were parsed as part of the request: a view of `put i32 1` cut from
  `put i32 12` wrote 12, one cut short of a closing brace was accepted, and a
  view at the end of an allocation read beyond it. The request is now copied
  into a terminated string before it is parsed.
- A `put` of a vector element faulted wherever the buffer did not store the
  vector's table: `put object.i32_vector[0] 5` with no `object` read the
  vector out of a null table, for every element type. A `put` of an element
  of a vector of strings, tables or unions faulted wherever the buffer did not
  store the vector, even in a stored table (`put string_vector[0] "z"`). Each
  now creates the vector, padded as a stored vector grows past its end:
  zeroes, empty strings, empty tables, and the union's first member.
- A path naming a union's `NONE` member (`put number.NONE {}`,
  `delete number.NONE`) read the schema's object list at index -1. NONE holds
  no table, so the parser now refuses it. Apply looks a union member up by its
  discriminant rather than its position, so a hand-built request naming NONE
  or an undeclared member writes nothing and leaves the union as stored.
- Copying a stored union whose discriminant is NONE, or one the schema does
  not declare, faulted on every request that copied it; the verifier accepts
  both. Such a value is now kept as an empty table under the same
  discriminant.
- A `put` whose terminal field could not be built shifted every later field's
  offset by one: the table builder read an offset for it that the generate
  pass had not pushed, handing each later field its neighbour's and reading
  past the end of the list. Such a field now keeps what the buffer stores, or
  stays absent, and a union's discriminant stays with its value.
- An edit inside a union-vector element, or a delete inside a union, read the
  stored member as the member the path names when the two differed, reading
  its fields at the other member's offsets: a put of a Fraction's
  `denominator` over a stored `Integer` carried the Integer's value into the
  Fraction's numerator. The stored member is now a source only when it is the
  member named, and a delete of another member's field leaves the union as it
  was.
- A `delete` reaching into a table, union or struct the buffer did not store
  built one: `delete object.i8` with no `object` left an empty `object`, and
  `delete number.Integer.value` with no `number` left an `Integer` tagged
  NONE. Inside a table that was itself absent the union case faulted, and
  `delete scalars.i8` with no `scalars` copied a struct out of bytes that were
  not there. Such a delete now leaves the field absent.
- A whole-vector `put` of a vector of unions read the union's enum index as an
  object index. No payload carries the members' types, so nothing is built
  for one; the parser already refused it.
- An element index above 65535 was truncated to the 16 bits a path carried,
  so `put i32_vector[70000] 5` wrote element 4464 and
  `delete i32_vector[65537]` removed element 1. Paths are now 32 bits wide.
  A `put` (or `patch`) whose index would grow one vector past half of what a
  flatbuffer can hold is refused by the parser, and a hand-built one writes
  nothing, so the growth an index asks for leaves room for the table around
  it.
- A negative index (`put i32_vector[-1] 5`) wrapped to the top of the path's
  range, and a bracket holding no whole index (`delete i32_vector[x]`,
  `delete i32_vector[1`) was read as the vector itself, removing all of it.
  Both are refused.
- A grown vector of unions was padded with discriminant 1, which names no
  member where the union's first member is given another value
  (`union U { A = 2, B }`). Padding is now the first member that holds a
  table. A hand-built request naming a member value past 255 was truncated to
  a declared one; it now names no member and writes nothing.
- A request naming a union's `_type` discriminator (`put number_type 2`,
  `put number_vector_type[0] 2`, `delete number_type`) was reported applied
  while the table was rebuilt with the union as stored. It is refused.
- An index past the end of a fixed-length array (`put scalars.i32_array[99] 1`)
  parsed and wrote nothing. It is refused.
- An index into something that is not a vector or an array (`put i8[0] 1`), or
  a second index into an element (`put i32_vector[0][1] 5`), was left for the
  payload parser, which read the bracket as the value. It is refused, and so
  is any text after the operation (`delete i8 i16` deleted only `i8`).
- `delete struct_vector[0].i8` removed the whole element; it is now a no-op,
  as a delete inside a struct field is.
- A `patch` whose path is a vector or array of structs, or an element of one
  (`patch struct_vector { put i8 1 }`), parsed, and its puts read the patched
  field's offset as an element index or built nothing. One stopping at a
  whole vector, or at an element of an array of structs, is refused; one
  naming an element of a vector of structs patches it (see `### Added`).
- A union member that is a struct (possible in a schema generated for C++
  alone) was read as a table: a path into one, and a stored one copied
  because the request edited something else, read its bytes as a vtable. A
  path into one is refused, and a stored one is copied as its bytes, in a
  union and in a vector of unions.
- A `patch` whose path is a whole vector of tables
  (`patch object_vector { put i8 1 }`), and a path continuing past one
  (`delete object_vector.i8`, `delete struct_vector.i8`), read the next
  field's offset as an element index: the delete removed element 6, or
  element 1, and the put read its payload as a table. Both are refused.

## [0.4.1] - 2026-09-20

### Fixed
- A struct payload naming no member the struct declares still created the
  struct. An unknown key is ignored rather than refusing the whole map, but
  ignoring every key counted as a successful write, so `put scalars {"zzz": 1}`
  against a buffer storing no `scalars` left one of zeroes behind. A map that
  writes nothing -- an empty one included -- now leaves the field as it was,
  which is what the surrounding code already documented. An empty ARRAY
  payload counts the same way: `put scalars.i32_array []` reaches no element,
  so it no longer creates the struct either.
- A member of a struct named in a MAP took a value of the wrong kind. The
  request parser sees only the map and cannot tell which key goes to which
  type, and the writer converts whatever it is given -- so
  `put scalars {"i8": [1,2,3]}` wrote 3, the vector's LENGTH, and
  `{"i8": "hello"}` wrote 0. Both are refused now, as the same value already
  was when named by its own path.

## [0.4.0] - 2026-09-20

### Fixed
- A request whose path ended in an **enum-typed** field was refused as
  malformed. Whether the payload needed parsing against the schema was decided
  from `Type::index()`, which an enum scalar sets too — it names the enum — so
  `put tint 4` was handed to the table parser with no object to parse against.
  The decision now follows the object the path actually resolved to. The same
  mistake made a vector of enums read its element type as an object index, so
  `put tints[0] 5` and `put tints [0, 4]` were refused as well.
- A `put` into a **struct** member did nothing, and one into a struct the
  buffer did not store faulted. The struct branch copied the stored bytes
  unconditionally, ignoring the request, and read a field that was not there
  when the buffer had no struct. A struct is now built as an image — stored
  bytes, or zeroes — with the payload written into it.
- A `put` of a **whole struct** faulted: the payload was read as a flatbuffer
  table root. A struct is now carried as a flexbuffer map and written by
  member name, so a map naming a subset leaves the rest as they were.

- A `put` whose payload was the wrong KIND for its field was served rather
  than refused. A string on a scalar reads as 0, so `put color "Blue"` wrote
  Red; a non-map on a struct wrote a struct of zeroes, creating the field if
  the buffer did not have it. Both are refused now, and a request that writes
  nothing leaves the field as it was.
- `put struct_vector[0].i8 5` emitted a corrupt buffer (and segfaulted where
  the vector was absent): apply builds no offset for an inline struct element
  while the table builder still consumes one. It is refused now, as is a whole
  vector of structs and a whole vector of unions. `delete struct_vector[0]` is
  unaffected.

### Added
- **Fixed-size arrays of scalars** inside a struct take a put, by element
  (`put scalars.i32_array[3] 9`) or whole (`put scalars.i32_array [7, 8]`).
  An array cannot change length, so a payload longer than `N` writes nothing.
  An array of STRUCTS is not written: its elements' stride is the struct's
  byte size, which the scalar path does not answer.

## [0.3.1] - 2026-09-20

### Fixed
- A put into a stored union member kept the member's other fields. They were
  read out of the table HOLDING the union, at the member's own field offsets,
  which mean something else there or nothing: `put number.Fraction.numerator`
  dropped `denominator` to 0. The member is now the source, and only while the
  stored member is the one being written, so switching member carries nothing
  across.

## [0.3.0] - 2026-07-05

### Added
- `fbrequest` command-line tool (behind the `tools` vcpkg feature, or
  `-DFBREQUEST_BUILD_TOOLS=ON`): applies a request to a FlatBuffers binary,
  reading from a file or stdin and writing to stdout, a file, or in place.
  See [docs/cli.md](docs/cli.md).
- Golden-file tests exercising the CLI across all three I/O modes.

## [0.2.0] - 2026-07-05

### Changed
- **Breaking:** `parseFlatbufferRequest` now returns `std::optional<ByteBuffer>`
  (an owned `std::vector<uint8_t>`) instead of `std::optional<TypedBuffer<Request>>`.
  Read the result with `flatbuffers::GetRoot<serialized::Request>(buffer.data())`.

### Removed
- The `TypedBuffer` class template and its header. It was only a thin owned-buffer
  wrapper around the parse result and carried unused machinery; the plain buffer
  is a simpler API surface.

## [0.1.0] - 2026-07-04

Initial release, extracted from the Composer engine.

### Added
- `put` / `patch` / `delete` request parsing (`parseFlatbufferRequest`) and
  application (`applyRequest`) over arbitrary FlatBuffers reflection schemas.
- Support for scalars, strings, tables, unions, and vectors thereof, including
  vector-element edits with growth/padding, and whole-struct / struct-vector
  handling.
- `copyTable` reflection deep-copy primitive and the `TypedBuffer` helper.
- Standalone CMake build (via the `targets` helpers), install/export with a
  `find_package(FlatbuffersRequest)` config package, and a vcpkg port.
- GoogleTest suite (190 cases) with vendored schema-aware FlatBuffer matchers.

### Known limitations
- Fixed-size arrays (`[T:N]`) are not yet supported.
- Deleting an individual field inside a struct is a no-op (structs are
  indivisible).

  (Both as of 0.1.0; see 0.4.0 for what arrays now accept.)

[Unreleased]: https://github.com/alexames/flatbuffers-request/compare/v0.4.1...HEAD
[0.4.1]: https://github.com/alexames/flatbuffers-request/compare/v0.4.0...v0.4.1
[0.4.0]: https://github.com/alexames/flatbuffers-request/compare/v0.3.1...v0.4.0
[0.3.1]: https://github.com/alexames/flatbuffers-request/compare/v0.3.0...v0.3.1
[0.3.0]: https://github.com/alexames/flatbuffers-request/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/alexames/flatbuffers-request/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/alexames/flatbuffers-request/releases/tag/v0.1.0
