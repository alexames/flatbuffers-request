# Changelog

All notable changes to this project are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

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

[Unreleased]: https://github.com/alexames/flatbuffers-request/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/alexames/flatbuffers-request/releases/tag/v0.1.0
