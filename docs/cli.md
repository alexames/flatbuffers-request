# `fbrequest` command-line tool

A thin, Unix-style wrapper around the library: it applies a single
put/patch/delete request to a FlatBuffers binary. It reads the input from a file
or stdin, and writes the result to stdout, a file, or back to the input file in
place.

## Synopsis

```
fbrequest --schema SCHEMA.bfbs [--output FILE | --in-place] REQUEST [INPUT]
```

## Options

| Option | Description |
|--------|-------------|
| `-s, --schema FILE` | Binary reflection schema (`.bfbs`) describing the buffer. **Required.** |
| `-o, --output FILE` | Write the result to `FILE`. Default: stdout. |
| `-i, --in-place` | Modify the input file in place (requires a file `INPUT`; not valid with `--output` or stdin). |
| `-h, --help` | Show usage and exit. |

## Positional arguments

| Argument | Description |
|----------|-------------|
| `REQUEST` | The request to apply, e.g. `put player.health 100`. **Required.** See [grammar.md](grammar.md). |
| `INPUT` | The input flatbuffer file. If omitted or `-`, the buffer is read from **stdin**. |

## Getting a `.bfbs` schema

The tool needs the *binary* reflection schema, produced from your `.fbs` with
`flatc`:

```bash
flatc -b --schema yourschema.fbs        # writes yourschema.bfbs
```

## Examples

```bash
# Filter: read from stdin, write to stdout
fbrequest -s player.bfbs 'put stats.health 100' < player.bin > player2.bin

# Read a file, write a file
fbrequest -s player.bfbs 'put name "Aria"' player.bin -o player.bin

# Edit in place
fbrequest -s player.bfbs -i 'delete title' player.bin

# A multi-op patch, piped
cat player.bin | fbrequest -s player.bfbs 'patch { put i8 5 ; delete title }' > out.bin
```

## Exit status

| Code | Meaning |
|------|---------|
| `0` | Success. |
| `1` | Runtime failure (I/O error, invalid schema/input, malformed request). |
| `2` | Usage error (bad or missing arguments). |

The tool verifies both the schema and the input buffer before applying a
request, and never writes partial output on failure.

## Installing

Via vcpkg, request the `tools` feature (it pulls in `cxxopts` and installs the
binary under `tools/flatbuffers-request/`):

```json
{ "dependencies": [ { "name": "flatbuffers-request", "features": ["tools"] } ] }
```

Or build from source with `-DFBREQUEST_BUILD_TOOLS=ON`.
