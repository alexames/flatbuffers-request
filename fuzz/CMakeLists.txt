# Fuzz targets require Clang's libFuzzer. On any other compiler this is a no-op
# so a plain -DFBREQUEST_BUILD_FUZZERS=ON never breaks the configure.
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(WARNING
    "FBREQUEST_BUILD_FUZZERS is ON but the compiler is not Clang; "
    "skipping fuzz targets (libFuzzer is Clang-only).")
  return()
endif()

add_executable(fbrequest_fuzz_parse fuzz_parse.cpp)
target_link_libraries(fbrequest_fuzz_parse
  PRIVATE
    FlatbuffersRequest::FlatbuffersRequest
    flatbuffers::flatbuffers)
target_compile_options(fbrequest_fuzz_parse PRIVATE -fsanitize=fuzzer)
target_link_options(fbrequest_fuzz_parse PRIVATE -fsanitize=fuzzer)

# Instrument the library too, when the top-level build opted into sanitizers via
# CMAKE_CXX_FLAGS (e.g. -fsanitize=fuzzer-no-link,address,undefined). Nothing to
# do here for that — those flags flow through the shared CMAKE_CXX_FLAGS.
