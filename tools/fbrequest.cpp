// Copyright 2026 Alexander Ames
//
// fbrequest -- apply put/patch/delete requests to FlatBuffers binaries.
//
// Reads a flatbuffer from a file or stdin, applies a reflection-driven request
// against the given schema, and writes the result to stdout, a file, or back
// to the input file in place.

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include "cxxopts.hpp"
#include "fbrequest/FlatbufferRequest.hpp"
#include "flatbuffers/flatbuffers.h"
#include "flatbuffers/reflection.h"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace {

using Bytes = std::vector<uint8_t>;

// Exit codes: 0 success, 1 runtime failure, 2 usage error.
constexpr int kUsageError = 2;
constexpr int kRuntimeError = 1;

std::optional<Bytes> readStream(std::istream& in) {
  Bytes buf((std::istreambuf_iterator<char>(in)),
            std::istreambuf_iterator<char>());
  if (in.bad()) {
    return std::nullopt;
  }
  return buf;
}

std::optional<Bytes> readFile(const std::string& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    std::cerr << "fbrequest: cannot open input file: " << path << '\n';
    return std::nullopt;
  }
  return readStream(stream);
}

bool writeStream(std::ostream& out, const Bytes& bytes) {
  out.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  out.flush();
  return out.good();
}

bool writeFile(const std::string& path, const Bytes& bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) {
    std::cerr << "fbrequest: cannot open output file: " << path << '\n';
    return false;
  }
  return writeStream(stream, bytes);
}

// FlatBuffers are binary; keep the standard streams from translating bytes.
void setBinaryStdio() {
#ifdef _WIN32
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
#endif
}

}  // namespace

int main(int argc, char** argv) {
  cxxopts::Options options(
      "fbrequest", "Apply a put/patch/delete request to a FlatBuffers binary.");
  // clang-format off
  options.add_options()
    ("s,schema", "Binary reflection schema (.bfbs) [required]",
     cxxopts::value<std::string>())
    ("o,output", "Write the result to FILE (default: stdout)",
     cxxopts::value<std::string>(), "FILE")
    ("i,in-place", "Modify the input file in place")
    ("h,help", "Show this help and exit")
    ("request", "The request, e.g. 'put player.health 100'",
     cxxopts::value<std::string>())
    ("input", "Input flatbuffer file ('-' or omitted reads stdin)",
     cxxopts::value<std::string>());
  // clang-format on
  options.parse_positional({"request", "input"});
  options.positional_help("REQUEST [INPUT]");
  options.show_positional_help();

  cxxopts::ParseResult args;
  try {
    args = options.parse(argc, argv);
  } catch (const std::exception& e) {
    std::cerr << "fbrequest: " << e.what() << "\n\n" << options.help() << '\n';
    return kUsageError;
  }

  if (args.count("help") != 0U) {
    std::cout << options.help() << '\n';
    return EXIT_SUCCESS;
  }
  if (args.count("schema") == 0U) {
    std::cerr << "fbrequest: --schema is required\n\n"
              << options.help() << '\n';
    return kUsageError;
  }
  if (args.count("request") == 0U) {
    std::cerr << "fbrequest: a REQUEST argument is required\n\n"
              << options.help() << '\n';
    return kUsageError;
  }

  const auto request = args["request"].as<std::string>();
  const bool inPlace = args.count("in-place") != 0U;
  const bool haveOutput = args.count("output") != 0U;
  const bool haveInputFile =
      args.count("input") != 0U && args["input"].as<std::string>() != "-";

  if (inPlace && haveOutput) {
    std::cerr << "fbrequest: --in-place cannot be combined with --output\n";
    return kUsageError;
  }
  if (inPlace && !haveInputFile) {
    std::cerr << "fbrequest: --in-place requires an input file (not stdin)\n";
    return kUsageError;
  }

  // Load and verify the reflection schema.
  const auto schemaBytes = readFile(args["schema"].as<std::string>());
  if (!schemaBytes) {
    return kRuntimeError;
  }
  flatbuffers::Verifier schemaVerifier(schemaBytes->data(),
                                       schemaBytes->size());
  if (!reflection::VerifySchemaBuffer(schemaVerifier)) {
    std::cerr << "fbrequest: not a valid binary schema (.bfbs): "
              << args["schema"].as<std::string>() << '\n';
    return kRuntimeError;
  }
  const auto* schema = reflection::GetSchema(schemaBytes->data());

  // Load the input flatbuffer (file or stdin).
  std::optional<Bytes> input;
  if (haveInputFile) {
    input = readFile(args["input"].as<std::string>());
  } else {
    setBinaryStdio();
    input = readStream(std::cin);
  }
  if (!input) {
    std::cerr << "fbrequest: failed to read input\n";
    return kRuntimeError;
  }

  // Verify the input against the schema before touching it.
  if (!flatbuffers::Verify(*schema, *schema->root_table(), input->data(),
                           input->size())) {
    std::cerr << "fbrequest: input is not a valid flatbuffer for the schema\n";
    return kRuntimeError;
  }

  // Parse and apply the request.
  const auto parsed = fbrequest::parseFlatbufferRequest(schema, request);
  if (!parsed) {
    std::cerr << "fbrequest: malformed request: " << request << '\n';
    return kRuntimeError;
  }

  flatbuffers::FlatBufferBuilder fbb;
  fbb.Finish(fbrequest::applyRequest(
      fbb, schema, flatbuffers::GetAnyRoot(input->data()),
      flatbuffers::GetRoot<serialized::Request>(parsed->data())));
  const Bytes result(fbb.GetBufferPointer(),
                     fbb.GetBufferPointer() + fbb.GetSize());

  // Write the result: in place, to a file, or to stdout.
  if (inPlace) {
    return writeFile(args["input"].as<std::string>(), result) ? EXIT_SUCCESS
                                                              : kRuntimeError;
  }
  if (haveOutput) {
    return writeFile(args["output"].as<std::string>(), result) ? EXIT_SUCCESS
                                                               : kRuntimeError;
  }
  setBinaryStdio();
  if (!writeStream(std::cout, result)) {
    std::cerr << "fbrequest: failed to write to stdout\n";
    return kRuntimeError;
  }
  return EXIT_SUCCESS;
}
