// Model Lifecycle Fabric — artifact-set identity binding.
//
// Model Lifecycle Fabric does not store or fetch artifacts. It binds lifecycle
// authority to an exact artifact generation and verifies content identity where
// the caller supplies a locator it can read.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "mlf/identity.hpp"
#include "mlf/provenance.hpp"
#include "mlf/status.hpp"

namespace mlf {

/// Content identity of one concrete artifact set.
struct ArtifactDescriptor {
  ArtifactSetId set_id{};
  ArtifactGeneration generation{};
  /// Content digest. "sha256:<hex>" when computed over real bytes, otherwise a
  /// caller-supplied opaque token recorded verbatim.
  std::string digest{};
  /// Opaque locator. Never dereferenced by the lifecycle runtime; retained for
  /// audit and for callers that hold a real filesystem path.
  std::string locator{};
  /// Declared media format, e.g. "safetensors", "gguf", "onnx", "torch-pt".
  std::string format{};
  std::uint64_t byte_size{0};
  Provenance provenance{Provenance::Unknown};
};

/// The binding a model version carries: which artifact set, at which generation,
/// with which digest. A digest or generation change invalidates every plan and
/// every evidence record bound to the previous binding.
struct ArtifactBinding {
  ArtifactSetId set_id{};
  ArtifactGeneration generation{};
  std::string digest{};

  [[nodiscard]] bool valid() const noexcept { return set_id.valid() && generation.valid(); }
  [[nodiscard]] bool matches(const ArtifactBinding& other) const noexcept {
    return set_id == other.set_id && generation == other.generation && digest == other.digest;
  }
  friend bool operator==(const ArtifactBinding& a, const ArtifactBinding& b) noexcept {
    return a.matches(b);
  }
};

/// Compute a stable content digest of a real file: "sha256:<64 lowercase hex>".
/// Returns false when the file cannot be read. Used by the real artifact proof
/// and by callers that want byte-level artifact identity.
[[nodiscard]] bool compute_file_digest(std::string_view path, std::string& out_digest);

/// Compute a stable content digest of an in-memory buffer.
[[nodiscard]] std::string compute_buffer_digest(const void* data, std::size_t size);

/// Validate the shape of a digest string. Accepts "sha256:<hex>" and opaque
/// tokens of bounded length made of [A-Za-z0-9:_-]. Rejects empty and oversized.
[[nodiscard]] bool valid_digest(std::string_view digest, std::size_t max_length) noexcept;

}  // namespace mlf
