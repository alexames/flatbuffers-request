cmake_minimum_required(VERSION 3.21)

project(FlatbuffersRequest
  VERSION 0.4.1
  DESCRIPTION "Reflection-driven put/patch/delete requests for FlatBuffers"
  HOMEPAGE_URL "https://github.com/alexames/flatbuffers-request"
  LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(FBREQUEST_BUILD_TESTS "Build FlatbuffersRequest tests" ${PROJECT_IS_TOP_LEVEL})
option(FBREQUEST_BUILD_FUZZERS "Build FlatbuffersRequest fuzz targets" OFF)
option(FBREQUEST_BUILD_TOOLS "Build the fbrequest command-line tool" ${PROJECT_IS_TOP_LEVEL})
option(FBREQUEST_INSTALL "Generate install/export rules" ${PROJECT_IS_TOP_LEVEL})

# ---------------------------------------------------------------------------
# Build helpers (Bazel-style targets library) + dependencies
# ---------------------------------------------------------------------------
find_package(Targets CONFIG REQUIRED)
include(Targets)

find_package(flatbuffers CONFIG REQUIRED)

# ---------------------------------------------------------------------------
# Schema -> generated C++ (serialized::Request and friends)
#
# SCHEMA_ROOT_DIR is the schemas/ dir so the schema's own subdirectory
# (fbrequest/) becomes the generated header's include prefix, i.e. the code
# includes "fbrequest/FlatbufferRequest_generated.h".
# ---------------------------------------------------------------------------
flatbuffer_cpp_library(
    TARGET RequestSchemas
    SCHEMAS
        "schemas/fbrequest/FlatbufferRequest.fbs"
    SCHEMA_ROOT_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/schemas"
)

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------
cpp_library(
    TARGET FlatbuffersRequest
    SOURCE_DIR "src"
    HEADER_DIR "include"
    SOURCES
        "FlatbufferRequestApply.cpp"
        "FlatbufferRequestParse.cpp"
        "StringScan.cpp"
    HEADERS
        "fbrequest/FlatbufferRequest.hpp"
    DEPENDENCIES
        PUBLIC
            RequestSchemas
            flatbuffers::flatbuffers
)

# cpp_library sets CXX_STANDARD only as a private build property, so the
# language level does not flow to consumers of the installed/exported target.
# The public headers use std::optional / std::span / std::string_view, so
# export the requirement to make find_package() consumers compile as C++23.
target_compile_features(FlatbuffersRequest PUBLIC cxx_std_23)

# ---------------------------------------------------------------------------
# Command-line tool
# ---------------------------------------------------------------------------
if(FBREQUEST_BUILD_TOOLS)
  find_package(cxxopts CONFIG REQUIRED)
  cpp_binary(
      TARGET fbrequest
      SOURCE_DIR "tools"
      SOURCES
          "fbrequest.cpp"
      DEPENDENCIES
          PRIVATE
              FlatbuffersRequest
              flatbuffers::flatbuffers
              cxxopts::cxxopts
  )
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(FBREQUEST_BUILD_TESTS)
  enable_testing()

  set(FBREQUEST_BFBS_DIR "${PROJECT_BINARY_DIR}/flatbuffers")

  # The reflection schema the tests load at run time (as a stand-in user
  # schema) plus its generated C++ for constructing/verifying test buffers.
  flatbuffer_cpp_library(
      TARGET TestSchemas
      SCHEMAS
          "test/test_schema.fbs"
      SCHEMA_ROOT_DIR
          "${CMAKE_CURRENT_SOURCE_DIR}/test"
      BINARY_SCHEMAS_DIR
          "${FBREQUEST_BFBS_DIR}"
  )

  cpp_test(
      TARGET FlatbufferRequestTest
      SOURCE_DIR "test"
      SOURCES
          "FlatbufferRequestTest.cpp"
          "matchers/FlatbufferMatcher.cpp"
          "matchers/FlexbufferMatcher.cpp"
          "matchers/MatcherInternal.cpp"
      INCLUDES
          PRIVATE
              "${CMAKE_CURRENT_SOURCE_DIR}/test/matchers"
      DEPENDENCIES
          PRIVATE
              FlatbuffersRequest
              TestSchemas
              flatbuffers::flatbuffers
              GTest::gmock_main
      # test_schema.bfbs is emitted here; run the test from that directory so
      # its relative LoadFile("test_schema.bfbs") resolves.
      WORKING_DIRECTORY
          "${FBREQUEST_BFBS_DIR}"
  )

  # CLI golden-file tests. Each I/O mode (file, stdin, in-place) applies a
  # fixed request to golden/input.bin and byte-compares against golden/expected.bin.
  if(FBREQUEST_BUILD_TOOLS)
    foreach(mode file stdin inplace)
      add_test(
          NAME "fbrequest_cli_golden_${mode}"
          COMMAND "${CMAKE_COMMAND}"
              "-DTOOL=$<TARGET_FILE:fbrequest>"
              "-DSCHEMA=${FBREQUEST_BFBS_DIR}/test_schema.bfbs"
              "-DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/test/golden/input.bin"
              "-DREQUEST=put i8 99"
              "-DGOLDEN=${CMAKE_CURRENT_SOURCE_DIR}/test/golden/expected.bin"
              "-DMODE=${mode}"
              "-DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}"
              -P "${CMAKE_CURRENT_SOURCE_DIR}/test/run_cli_golden.cmake")
    endforeach()
  endif()
endif()

# ---------------------------------------------------------------------------
# Fuzzing (clang/libFuzzer only; opt-in)
# ---------------------------------------------------------------------------
if(FBREQUEST_BUILD_FUZZERS)
  add_subdirectory(fuzz)
endif()

# ---------------------------------------------------------------------------
# Install / export
#
# Both the library and the generated-schema target are exported together (the
# library PUBLIC-links the schema target). The generated header and the public
# headers are installed, and each target gains an install-interface include dir.
# ---------------------------------------------------------------------------
if(FBREQUEST_INSTALL)
  include(GNUInstallDirs)
  include(CMakePackageConfigHelpers)

  target_include_directories(FlatbuffersRequest INTERFACE
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
  target_include_directories(RequestSchemas INTERFACE
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")

  install(TARGETS FlatbuffersRequest RequestSchemas
    EXPORT FlatbuffersRequestTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

  # The command-line tool (relocated to tools/<port>/ by the vcpkg portfile).
  if(FBREQUEST_BUILD_TOOLS)
    install(TARGETS fbrequest RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
  endif()

  # Hand-written public headers.
  install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
  # Generated schema header (produced under the RequestSchemas build dir).
  install(DIRECTORY
    "${CMAKE_CURRENT_BINARY_DIR}/_flatbuffer_cpp_library/RequestSchemas/fbrequest"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
  # Ship the schema so consumers can regenerate or extend it.
  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/schemas/fbrequest/FlatbufferRequest.fbs"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/fbrequest/schemas/fbrequest")

  install(EXPORT FlatbuffersRequestTargets
    NAMESPACE FlatbuffersRequest::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/FlatbuffersRequest")

  configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/FlatbuffersRequestConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/FlatbuffersRequestConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/FlatbuffersRequest")
  write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/FlatbuffersRequestConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY SameMajorVersion)
  install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/FlatbuffersRequestConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/FlatbuffersRequestConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/FlatbuffersRequest")
endif()
