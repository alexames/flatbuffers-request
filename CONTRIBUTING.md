# Contributing

Thanks for your interest in `flatbuffers-request`.

## Building and testing

Requires a C++23 compiler, CMake ≥ 3.21, and vcpkg (with `VCPKG_ROOT` set).

```bash
cmake --preset default
cmake --build build
ctest --test-dir build --output-on-failure
```

**Run the asserting build too, and run its binary directly.** Much of this
library's contract with FlatBuffers is enforced by `assert`, which a Release
build compiles out -- a request that trips one passes the Release suite and
aborts in a consumer's debug build. With a multi-config generator, test
discovery records the exe path of whichever configuration was built last, so
`ctest -C Debug` can silently re-run the Release binary and report green:

```bash
cmake --build build --config Debug
cd build/flatbuffers && ../Debug/FlatbufferRequestTest.exe
```

`build/flatbuffers` is the working directory the suite needs, because it loads
`test_schema.bfbs` by a relative path.

New behaviour needs tests; bug fixes need a reproducing test. The suite lives in
[test/FlatbufferRequestTest.cpp](test/FlatbufferRequestTest.cpp) and is mostly
parameterized `(initial, request, expected)` triples — adding a case is usually
a one-line `TestArgs(...)`.

## Style

- Format with `clang-format` (`.clang-format` is checked in) before committing.
- Fix `clang-tidy` warnings (`.clang-tidy` is checked in).
- Keep to ASCII in source files.
- Follow the surrounding code's naming and comment density.

## Commits and pull requests

- Work on a branch; do not commit to `main`.
- Write clear commit summaries starting with a capital letter; do not use
  conventional-commit prefixes (`feat:`, `fix:`, …).
- Open a PR with a short summary, the specific changes, and how you tested them.
  CI (build + tests, format check, sanitizers) must be green.

## Scope

This library deliberately depends on FlatBuffers reflection internals; see
[docs/design.md](docs/design.md) before proposing changes that touch the
apply/parse core, and note the FlatBuffers-version coupling described there.
