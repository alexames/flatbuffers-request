# flatbuffers-request

Reflection-driven **put / patch / delete** requests for [FlatBuffers](https://github.com/google/flatbuffers) binaries.

Given a FlatBuffers reflection schema (`.bfbs`) and an existing buffer, `flatbuffers-request` parses a small textual request language and applies it — producing a **new, valid buffer** with a field set, a subtree merged, or an element removed. It works entirely through the FlatBuffers reflection API, so it operates on *any* schema without generated per-type code.

```text
put   player.stats.health 100
patch player { put name "Aria" ; delete title }
delete inventory[3]
```

> Status: **v0.4.1**, extracted from the [Composer](https://github.com/alexames) engine's editor. Tables, vectors, unions, strings, scalars, enums and structs are supported, including fixed-size arrays inside a struct. Vectors of structs are not yet edited element-by-element.

## Install (vcpkg)

`flatbuffers-request` is published through the [`alexames/vcpkg-registry`](https://github.com/alexames/vcpkg-registry). Add it to your registry configuration and manifest:

`vcpkg-configuration.json`
```json
{
  "registries": [
    {
      "kind": "git",
      "repository": "https://github.com/alexames/vcpkg-registry",
      "baseline": "<latest-baseline>",
      "packages": ["flatbuffers-request", "targets"]
    }
  ]
}
```

`vcpkg.json`
```json
{ "dependencies": ["flatbuffers-request"] }
```

Then, in CMake:
```cmake
find_package(FlatbuffersRequest CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE FlatbuffersRequest::FlatbuffersRequest)
```

## Usage

```cpp
#include "fbrequest/FlatbufferRequest.hpp"
#include "flatbuffers/flatbuffers.h"
#include "flatbuffers/reflection.h"

// `schema` is a reflection::Schema* (load a .bfbs via reflection::GetSchema).
// `current` is an existing FlatBuffers binary that matches that schema.
void bump_health(const reflection::Schema* schema,
                 const std::vector<uint8_t>& current) {
  auto request = fbrequest::parseFlatbufferRequest(schema, "put player.health 100");
  if (!request) return;  // malformed request

  flatbuffers::FlatBufferBuilder fbb;
  fbb.Finish(fbrequest::applyRequest(
      fbb, schema,
      flatbuffers::GetAnyRoot(current.data()),
      flatbuffers::GetRoot<serialized::Request>(request->data())));

  // fbb.GetBufferPointer() / fbb.GetSize() now hold the updated buffer.
}
```

The public API is three functions in namespace `fbrequest` ([include/fbrequest/FlatbufferRequest.hpp](include/fbrequest/FlatbufferRequest.hpp)):

| Function | Purpose |
|----------|---------|
| `parseFlatbufferRequest(schema, text)` | Parse request text into a serialized `Request` buffer (`std::nullopt` if malformed). |
| `applyRequest(fbb, schema, source, request)` | Apply a parsed request to `source`, building the result into `fbb`. |
| `copyTable(fbb, schema, ptr, pool)` | Deep-copy a table by reflection (the primitive `applyRequest` builds on). |

## Request language

See [docs/grammar.md](docs/grammar.md) for the full grammar. In brief:

- **`put <path> <value>`** — set a scalar, string, table, union member, vector, or vector element. Missing intermediate objects/elements are created; vectors grow (zero/empty-padded) as needed.
- **`delete <path>`** — remove a field, a whole vector, or one vector element. Deleting an absent field is a no-op.
- **`patch <path> { <op> ; <op> ; ... }`** — apply a sequence of operations relative to the object at `<path>` (empty path patches the target directly). Patches nest.

Paths are dotted field names with `[i]` for vector indices and `.MemberName` for union members, e.g. `number.Integer.value`, `object_vector[2].name`.

## Command-line tool

The `tools` feature builds `fbrequest`, a Unix-style CLI that applies one request
to a FlatBuffers binary — reading from a file or stdin, writing to stdout, a
file, or in place:

```bash
fbrequest -s player.bfbs 'put stats.health 100' < player.bin > player2.bin
fbrequest -s player.bfbs -i 'delete title' player.bin        # edit in place
```

It takes the binary reflection schema (`.bfbs`) as `--schema`. See
[docs/cli.md](docs/cli.md) for the full reference. Install it via vcpkg with the
`tools` feature, or build from source with `-DFBREQUEST_BUILD_TOOLS=ON`.

## Building from source

Requires a C++23 compiler, CMake ≥ 3.21, and vcpkg (for the `flatbuffers` and `targets` dependencies).

```bash
cmake --preset default            # Ninja + vcpkg toolchain
cmake --build build
ctest --test-dir build --output-on-failure
```

Or with Visual Studio: `cmake --preset vs && cmake --build build --config Release`.

`VCPKG_ROOT` must point at your vcpkg checkout (the presets read it for the toolchain file).

## Relationship to FlatBuffers versions

This library uses FlatBuffers reflection **internals** (the `Parser`, `CopyTable`, and reflection structs), which upstream does not guarantee stable across releases. It is developed against FlatBuffers `25.12.19`. A major FlatBuffers bump may require a corresponding release here — see [docs/design.md](docs/design.md).

## License

[Apache-2.0](LICENSE). Portions of the test-support matchers are derived from Google LLC code; see [NOTICE](NOTICE).
