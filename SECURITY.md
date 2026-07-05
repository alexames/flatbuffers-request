# Security Policy

## Threat model

`flatbuffers-request` parses **request text** and applies it against a
**reflection schema** and a **source buffer**. Treat all three as potentially
untrusted:

- `parseFlatbufferRequest` scans arbitrary caller-supplied strings.
- `applyRequest` performs reflection-driven pointer and offset arithmetic while
  rebuilding a buffer.

Malformed *request text* is expected and handled: the parser returns
`std::nullopt` rather than crashing. Malformed or adversarial **schemas** and
**source buffers** are outside the current hardening guarantees — callers should
verify a source buffer against its schema (e.g. `flatbuffers::Verify`) before
applying a request to it, as the tests do.

The apply path is exercised under AddressSanitizer / UndefinedBehaviorSanitizer
in CI and has a libFuzzer target ([fuzz/](fuzz/)). Reports of crashes,
out-of-bounds access, or memory corruption on any input are in scope and
welcome.

## Reporting a vulnerability

Please report suspected vulnerabilities privately via GitHub's
[private vulnerability reporting](https://github.com/alexames/flatbuffers-request/security/advisories/new)
rather than opening a public issue. Include a minimal reproducer (schema +
source + request) where possible. You can expect an initial response within a
few days.
