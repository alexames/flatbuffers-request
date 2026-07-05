# Driver for the fbrequest CLI golden-file tests.
#
# Runs the tool in one of three I/O modes and byte-compares its output against
# the committed golden. Invoked by ctest via `cmake -P` with these -D inputs:
#   TOOL SCHEMA INPUT REQUEST GOLDEN MODE WORKDIR
set(out "${WORKDIR}/cli_golden_${MODE}.bin")

if(MODE STREQUAL "file")
  # REQUEST and INPUT as positionals; result written to a file via --output.
  execute_process(
    COMMAND "${TOOL}" --schema "${SCHEMA}" "${REQUEST}" "${INPUT}" --output "${out}"
    RESULT_VARIABLE rc)
elseif(MODE STREQUAL "stdin")
  # Input on stdin (no INPUT positional); result on stdout.
  execute_process(
    COMMAND "${TOOL}" --schema "${SCHEMA}" "${REQUEST}"
    INPUT_FILE "${INPUT}"
    OUTPUT_FILE "${out}"
    RESULT_VARIABLE rc)
elseif(MODE STREQUAL "inplace")
  # Edit a scratch copy of the input in place.
  file(COPY_FILE "${INPUT}" "${out}")
  execute_process(
    COMMAND "${TOOL}" --schema "${SCHEMA}" "${REQUEST}" "${out}" --in-place
    RESULT_VARIABLE rc)
else()
  message(FATAL_ERROR "run_cli_golden: unknown MODE '${MODE}'")
endif()

if(NOT rc EQUAL 0)
  message(FATAL_ERROR "fbrequest (MODE=${MODE}) exited with ${rc}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E compare_files "${out}" "${GOLDEN}"
  RESULT_VARIABLE diff)
if(NOT diff EQUAL 0)
  message(FATAL_ERROR
    "MODE=${MODE}: output '${out}' does not match golden '${GOLDEN}'")
endif()
