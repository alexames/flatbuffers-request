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
is a non-negative decimal integer that fits 32 bits; a sign, a bracket that does
not hold a whole index, and an index into something that is not a vector or an
array are refused. Nothing may follow the operation but whitespace.

`value` is parsed as JSON against the field's type:
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

A path never names a union's `_type` discriminator, nor a vector of unions'
`_type` vector: each is written only with the union beside it, so a request
naming one is refused. A union is switched with `put <union>.<member> ...` and
cleared with `delete <union>`.

An index into a fixed-length array must be less than its length. A `put` (and a
`patch`) may not index a vector so far that the vector, grown to reach it,
would be larger than a flatbuffer can hold.

## put

`put <path> <value>` writes `value` at `path`.

- **Scalars / strings:** `put health 100`, `put name "Aria"`.
- **Tables:** `put player {"level": 3, "name": "Aria"}` (JSON object).
- **Union members:** `put number.Integer {"value": 9}` — sets both the union
  value and its type discriminant. A path stopping at the union
  (`put number {...}`) names no member, and is refused. `NONE` holds no table,
  so a path naming it (`put number.NONE {}`, `delete number.NONE`) is refused,
  and so is a path into a member that is a struct.
- **Whole vectors:** `put scores [1, 2, 3]`, `put names ["a", "b"]`.
- **Vector elements:** `put scores[4] 9`. Intermediate elements are created:
  scalar vectors zero-pad, string vectors pad with `""`, table/union vectors pad
  with empty elements (a union vector's padding is its first member). Setting
  an out-of-range index grows the vector, and an element of a vector the
  buffer does not store creates it, along with any table on the path to it.

Intermediate objects along the path are created as needed, so
`put a.b.c 1` on an empty table yields `{"a":{"b":{"c":1}}}`.

## delete

`delete <path>` removes what `path` selects:

- a scalar/string/table/union field (removed entirely); `delete number` and
  `delete number.Integer` both clear a union holding an Integer,
- a whole vector (`delete scores`),
- a single vector element (`delete scores[1]` — later elements shift down);
  for a vector of unions, `delete number_vector[1]` or
  `delete number_vector[1].<the member it holds>`,
- a field *inside* a sub-object, union member, or vector element.

Deleting an absent field, an element of an absent vector, or anything inside a
table, union or struct the buffer does not store is a **no-op** (the buffer is
returned unchanged; nothing is created on the way), and so is deleting a union
member other than the one stored, or a field of one. A field inside a struct,
or inside an element of a vector of structs, cannot be removed individually
— a struct has no way to record that a member is absent, so such a delete
is a no-op. A `put` of a struct member does apply; see [design.md](design.md).

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

The patched object must be a table or a struct that a `put` could reach: a
whole vector, a vector or array of structs or an element of one, and a union
with no member named are refused, even for a patch holding only deletes.

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
