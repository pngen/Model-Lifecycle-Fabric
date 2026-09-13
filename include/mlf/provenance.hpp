// Model Lifecycle Fabric — validation provenance classification.
#pragma once

#include <cstdint>
#include <string_view>

namespace mlf {

/// Honest classification of how a fact was established.
///
/// REAL        — observed against the live host, filesystem, GPU, or real OS
///               processes/sockets present on this machine.
/// SYNTHETIC   — produced by the deterministic synthetic lifecycle backend.
///               Never a claim about physical infrastructure.
/// UNSUPPORTED — the scenario cannot be exercised in this environment; the
///               runtime records the gap instead of fabricating a result.
enum class Provenance : std::uint8_t {
  Unknown = 0,
  Real = 1,
  Synthetic = 2,
  Unsupported = 3,
};

[[nodiscard]] constexpr std::string_view to_string(Provenance p) noexcept {
  switch (p) {
    case Provenance::Real:        return "REAL";
    case Provenance::Synthetic:   return "SYNTHETIC";
    case Provenance::Unsupported: return "UNSUPPORTED";
    case Provenance::Unknown:     break;
  }
  return "UNKNOWN";
}

[[nodiscard]] constexpr bool is_claim_of_physical_reality(Provenance p) noexcept {
  return p == Provenance::Real;
}

}  // namespace mlf
