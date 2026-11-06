# Changelog

All notable changes to this project are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Fixed
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
