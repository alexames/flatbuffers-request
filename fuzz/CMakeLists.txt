# Fuzz targets require Clang's libFuzzer. On any other compiler this is a no-op
# so a plain -DFBREQUEST_BUILD_FUZZERS=ON never breaks the configure.
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(WARNING
    "FBREQUEST_BUILD_FUZZERS is ON but the compiler is not Clang; "
    "skipping fuzz targets (libFuzzer is Clang-only).")
  return()
endif()

foreach(target fbrequest_fuzz_parse fbrequest_fuzz_apply)
  string(REPLACE "fbrequest_fuzz_" "fuzz_" source "${target}")
  add_executable(${target} "${source}.cpp")
  target_link_libraries(${target}
    PRIVATE
      FlatbuffersRequest::FlatbuffersRequest
      flatbuffers::flatbuffers)
  target_compile_options(${target} PRIVATE -fsanitize=fuzzer)
  target_link_options(${target} PRIVATE -fsanitize=fuzzer)
endforeach()

# The apply target reads the test schema's text at startup, so that what it
# fuzzes is the schema the suite covers.
target_compile_definitions(fbrequest_fuzz_apply
  PRIVATE
    FBREQUEST_FUZZ_SCHEMA="${PROJECT_SOURCE_DIR}/test/test_schema.fbs")

# Instrument the library too, when the top-level build opted into sanitizers via
# CMAKE_CXX_FLAGS (e.g. -fsanitize=fuzzer-no-link,address,undefined). Nothing to
# do here for that — those flags flow through the shared CMAKE_CXX_FLAGS.
