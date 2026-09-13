// Model Lifecycle Fabric — shared helpers for the examples.
//
// Examples use only the public API.
#pragma once

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "mlf/artifact.hpp"
#include "mlf/engine.hpp"

namespace mlfexample {

inline std::string scratch_directory() {
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() / "mlf_examples";
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  return directory.string();
}

inline std::string write_artifact(const std::string& name, std::uint64_t seed, std::size_t size) {
  const std::filesystem::path path = std::filesystem::path(scratch_directory()) / name;
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  std::uint64_t state = seed == 0 ? 0x2545F4914F6CDD1Dull : seed;
  std::string buffer;
  buffer.reserve(size);
  for (std::size_t i = 0; i < size; ++i) {
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    buffer.push_back(static_cast<char>(state & 0xffu));
  }
  stream.write(buffer.data(), static_cast<std::streamsize>(buffer.size()));
  stream.close();
  return path.string();
}

inline bool require(const mlf::Decision& decision, const char* what) {
  if (decision.allowed()) return true;
  std::printf("  REFUSED %-28s %s\n", what, decision.render().c_str());
  return false;
}

inline mlf::ScopeId scope_by_path(const mlf::LifecycleEngine& engine, const std::string& path) {
  for (const mlf::ScopeRecord& record : engine.scopes()) {
    if (record.path == path) return record.id;
  }
  return mlf::ScopeId{};
}

inline mlf::ModelVersionRecord version_of(const mlf::LifecycleEngine& engine,
                                          mlf::ModelVersionId id) {
  for (const mlf::ModelVersionRecord& record : engine.all_versions()) {
    if (record.version == id) return record;
  }
  return {};
}

}  // namespace mlfexample
