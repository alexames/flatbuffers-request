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
and arrays of scalars -- as a flexbuffer, read by the library itself. A value
of the wrong kind is refused rather than coerced, so an enum takes its
ORDINAL and not its name: a name would read as 0.

## Values

A value is checked against the scalar, string, struct, vector or array it is
written as, wherever that is: a field, a vector element, a struct member, an
element of an array in a struct, a member named in a struct's map, or an
element of a whole vector or array. Anything else is refused.

| Target | Takes |
|--------|-------|
| bool | `true`, `false`, `0` or `1`. |
| integer, enum | A whole numeral, decimal or `0x` hex, with an optional sign, within the target's width: `put i8 300`, `put ui8 -1` and `put color 256` are refused, not wrapped. A uint64 takes all of `0` to `18446744073709551615`. A fraction, an exponent (`1e2`), a bool or a non-finite value is not an integer. |
| float32 | Any number that does not round to infinity as a float32 (`1e39` is refused; `3.4028235e38` is the largest float32): an integer, past the 64-bit ranges too, a decimal or exponent numeral, and `inf`, `infinity` or `nan` with an optional sign. |
| float64 | Any number a double holds, as for a float32. A numeral too large for a double (`1e309`) is refused; one too small for its type (`1e-400`) is written as the nearest value it holds, 0 or a subnormal. Hex floats (`0x1p3`) are not read. |
| string | A string quoted with `"` or `'`, holding no control character and valid UTF-8, with the escapes `\"`, `\'`, `\\`, `\/`, `\b`, `\f`, `\n`, `\r`, `\t`, `\uXXXX` (a surrogate pair as two) and `\xXX` (a byte). |
| vector | A list of values its element takes, possibly empty, with an optional trailing comma. |
| array, struct | As [design.md](design.md) describes: a non-empty list no longer than the array, or a map naming only members the struct declares. |

A map's keys are quoted or bare names, and may not repeat. `null` is no
value, and comments are not read. A numeral may not run on into other text
(`put i32 5abc`, `put f32 1.5f`), and nothing may follow a value but
whitespace, or the `;` or `}` of a patch.

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

A delete names something the buffer stores, or it builds nothing: deleting an
absent field, an element past the end of a vector, an element of an absent
vector, anything inside a table or union the buffer does not store, a union
member other than the one stored, or a field of one, answers **`NotStored`**
(see [Applying a request](#applying-a-request)). A caller treating delete as
idempotent keeps the buffer it had.

A member of a struct, an element of an array in one, and a member of an
element of a vector of structs cannot be removed individually: a struct has no
way to record that a member is absent. The parser refuses such a delete
(`delete scalars.i8`, `delete struct_vector[0].i8`,
`delete nested.structs[1]`), whether or not the buffer stores the struct. A
`put` of a struct member does apply; see [design.md](design.md).

## patch

`patch [<path>] { <op> ; <op> ; ... }` applies a sequence of operations to the
object at `<path>`, threading the result of each into the next. This lets one
request carry several independent edits, or edit a sub-object without restating
its path each time.

```text
patch { put i8 99 ; put i16 88 }              # two puts on the root
patch player { put level 3 ; delete title }   # edits scoped to `player`
patch a { patch b { put x 1 } }               # nested patches
patch player { }                              # empty patch: a copy
```

An omitted path (`patch { ... }`) patches the current object directly. Each
inner operation's path is relative to the patched object.

A patch is all or nothing. Each operation is checked against the table the
operations before it left. The patch answers `Refused` where any operation is
`Refused`, and otherwise `NotStored` where any is `NotStored`; for either it
builds nothing, so `patch { put i8 1 ; delete i16 }` on a buffer without
`i16` writes no `i8` either. A caller that treats a delete of something absent
as success keeps its source only for a lone `delete`: a patch answering
`NotStored` made none of its edits. An empty patch names no edit, and is
applied as a copy.

The patched object must be a table or a struct that a `put` could reach: a
whole vector, a vector or array of structs or an element of one, and a union
with no member named are refused, even for a patch holding only deletes.

## Applying a request

`tryApplyRequest` answers what applying a request came to, and builds the new
root into the caller's builder only where it is `Applied`:

| Outcome | Meaning |
|---------|---------|
| `Applied` | Every operation made the edit it names. A put of the value a field already holds is applied. |
| `NotStored` | A delete names something the buffer does not store. Nothing is built. |
| `Refused` | An operation cannot make the edit it names, whatever the buffer stores, or would copy, unedited, something no copy can keep as it was: a union payload stored under NONE or under a member its union does not declare, a vector of unions whose discriminants do not pair with its elements, a discriminant stored without its union, or a field the schema does not declare (see [design.md](design.md#copying-what-a-request-does-not-edit)). Nothing is built. The parser refuses each request of the first kind; a hand-built one can still carry one. |

`applyRequest` answers the same with only a root, null where nothing was built.

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
