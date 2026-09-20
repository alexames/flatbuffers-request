# Request language

A request is a single top-level operation. The three operations are `put`,
`patch`, and `delete`. Whitespace between tokens is insignificant.

```ebnf
request     = operation ;
operation   = put | patch | delete ;

put         = "put"    path value ;
delete      = "delete" path ;
patch       = "patch"  [ path ] "{" [ operation { ";" operation } ] "}" ;

path        = segment { "." segment } ;
segment     = field-name [ "[" index "]" ] [ "." union-member ] ;
value       = json-scalar | json-string | json-object | json-array ;
```

`field-name` and `union-member` are identifiers from the target schema. `index`
is a non-negative integer. `value` is parsed as JSON against the field's type:
tables, union members and vectors of tables go through the FlatBuffers JSON
parser, and everything else -- scalars, enums, strings, structs, and vectors
and arrays of scalars -- through a flexbuffer. A payload of the wrong kind is
refused rather than coerced, so an enum takes its ORDINAL and not its name: a
name would read as 0.

## Paths

A path navigates from the root table (or, inside a `patch`, from the patched
object) to a target field:

| Path | Selects |
|------|---------|
| `health` | a scalar/string field on the current object |
| `player.stats.health` | a field nested through sub-tables |
| `scores[2]` | element 2 of a vector |
| `number.Integer` | the `Integer` member of a `number` union |
| `number.Integer.value` | a field inside the active union member |
| `history[0].label` | a field inside a vector element |

## put

`put <path> <value>` writes `value` at `path`.

- **Scalars / strings:** `put health 100`, `put name "Aria"`.
- **Tables:** `put player {"level": 3, "name": "Aria"}` (JSON object).
- **Union members:** `put number.Integer {"value": 9}` — sets both the union
  value and its type discriminant.
- **Whole vectors:** `put scores [1, 2, 3]`, `put names ["a", "b"]`.
- **Vector elements:** `put scores[4] 9`. Intermediate elements are created:
  scalar vectors zero-pad, string vectors pad with `""`, table/union vectors pad
  with empty elements. Setting an out-of-range index grows the vector.

Intermediate objects along the path are created as needed, so
`put a.b.c 1` on an empty table yields `{"a":{"b":{"c":1}}}`.

## delete

`delete <path>` removes what `path` selects:

- a scalar/string/table/union field (removed entirely),
- a whole vector (`delete scores`),
- a single vector element (`delete scores[1]` — later elements shift down),
- a field *inside* a sub-object, union member, or vector element.

Deleting an absent field or an element of an absent vector is a **no-op** (the
buffer is returned unchanged). A field inside a struct cannot be removed
individually — a struct has no way to record that a member is absent, so such a
delete is a no-op. A `put` of a struct member does apply; see
[design.md](design.md).

## patch

`patch [<path>] { <op> ; <op> ; ... }` applies a sequence of operations to the
object at `<path>`, threading the result of each into the next. This lets one
request carry several independent edits, or edit a sub-object without restating
its path each time.

```text
patch { put i8 99 ; put i16 88 }              # two puts on the root
patch player { put level 3 ; delete title }   # edits scoped to `player`
patch a { patch b { put x 1 } }               # nested patches
patch player { }                              # empty patch: no-op
```

An omitted path (`patch { ... }`) patches the current object directly. Each
inner operation's path is relative to the patched object.

## Examples

These are drawn from the test suite ([test/FlatbufferRequestTest.cpp](../test/FlatbufferRequestTest.cpp)):

| Initial | Request | Result |
|---------|---------|--------|
| `{}` | `put object.i8 99` | `{"object":{"i8":99}}` |
| `{}` | `put number.Integer {"value":99}` | `{"number_type":"Integer","number":{"value":99}}` |
| `{"i32_vector":[10]}` | `put i32_vector[2] 99` | `{"i32_vector":[10,0,99]}` |
| `{"i8":5,"i32":10}` | `delete i8` | `{"i32":10}` |
| `{"scores":[1,2,3]}` | `delete scores[1]` | `{"scores":[1,3]}` |
| `{}` | `patch { put i8 99 ; put i16 88 }` | `{"i8":99,"i16":88}` |
