#pragma once
#include <cstdint>

#include "fbrequest/FlatbufferRequest.hpp"

// Internal to the library; not part of the installed public headers. The
// function below is an external symbol of the library all the same, reached
// only by code that includes this header from src/, which the tests do to
// name a bound small enough to reach.

namespace fbrequest {

// `tryApplyRequest`, with the most bytes the vectors a request grows may take,
// counting what `fbb` already holds, given as `largestGrownVectorBytes`
// rather than kLargestGrownVectorBytes. The bound is decided once, against
// the size of `fbb` on entry, before anything is built: a request it refuses
// is Refused, and every vector a request it accepts grows is grown. What the
// rest of the table copies beside them is not counted.
//
// @param fbb As for `tryApplyRequest`.
// @param schema As for `tryApplyRequest`.
// @param sourceTable As for `tryApplyRequest`.
// @param request As for `tryApplyRequest`.
// @param useStringPooling As for `tryApplyRequest`.
// @param largestGrownVectorBytes The most bytes `fbb` may hold on entry and
//     in the vectors a request grows, or builds whole, together. A value
//     above kLargestGrownVectorBytes is taken as kLargestGrownVectorBytes.
// @return As for `tryApplyRequest`.
[[nodiscard]] ApplyResult tryApplyRequestGrowingWithin(
    flatbuffers::FlatBufferBuilder& fbb, const reflection::Schema* schema,
    const flatbuffers::Table* sourceTable, const serialized::Request* request,
    bool useStringPooling, uint64_t largestGrownVectorBytes);

}  // namespace fbrequest
