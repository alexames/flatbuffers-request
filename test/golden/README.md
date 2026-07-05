# CLI golden fixture

These files drive `test/run_cli_golden.cmake` (registered as the
`fbrequest_cli_golden_{file,stdin,inplace}` ctest cases). Each mode applies the
same request to `input.bin` and byte-compares the tool's output to `expected.bin`.

| File | What it is |
|------|-----------|
| `input.json` | Human-readable source for `input.bin` (documentation only). |
| `input.bin` | The input flatbuffer: `{"i32": 10, "string": "hi"}` (schema `test/test_schema.fbs`). |
| `expected.bin` | Golden output of `fbrequest 'put i8 99' input.bin`, i.e. `{"i8": 99, "i32": 10, "string": "hi"}`. |

The goldens are byte-exact and therefore pinned to the FlatBuffers version the
library builds against (the `applyRequest` layout is deterministic per version).
If FlatBuffers is bumped and the layout changes, regenerate them:

```bash
# input.bin from input.json (text schema)
flatc -b -o test/golden test/test_schema.fbs test/golden/input.json

# expected.bin from the built tool (binary reflection schema)
fbrequest -s <build>/flatbuffers/test_schema.bfbs 'put i8 99' \
    test/golden/input.bin -o test/golden/expected.bin
```
