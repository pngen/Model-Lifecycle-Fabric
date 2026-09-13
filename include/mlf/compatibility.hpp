// Model Lifecycle Fabric — explicit, structured compatibility model.
//
// Model Lifecycle Fabric does not own canonical hardware or runtime facts; it
// consumes them. What it owns is whether a specific model generation, bound to a
// specific artifact generation, is admissible on a specific environment now.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "mlf/artifact.hpp"
#include "mlf/identity.hpp"
#include "mlf/provenance.hpp"
#include "mlf/status.hpp"

namespace mlf {

/// Structured compatibility verdict. Never prose.
enum class CompatibilityOutcome : std::uint8_t {
  COMPATIBLE = 0,
  COMPATIBLE_WITH_REBUILD,
  COMPATIBLE_WITH_RECOMPILE,
  COMPATIBLE_WITH_CONVERSION,
  INCOMPATIBLE_ARTIFACT,
  INCOMPATIBLE_RUNTIME,
  INCOMPATIBLE_BACKEND,
  INCOMPATIBLE_ARCHITECTURE,
  INCOMPATIBLE_PRECISION,
  INCOMPATIBLE_TOKENIZER,
  INCOMPATIBLE_ADAPTER,
  INCOMPATIBLE_POLICY,
  UNKNOWN,
  STALE_EVIDENCE,
  UNSUPPORTED,
};

inline constexpr std::size_t kCompatibilityOutcomeCount = 15;

[[nodiscard]] std::string_view to_string(CompatibilityOutcome outcome) noexcept;
[[nodiscard]] bool parse_compatibility_outcome(std::string_view text,
                                               CompatibilityOutcome& out) noexcept;

/// True when the outcome permits promotion without further work.
[[nodiscard]] constexpr bool is_serving_compatible(CompatibilityOutcome outcome) noexcept {
  return outcome == CompatibilityOutcome::COMPATIBLE;
}

/// True when the outcome permits promotion after a named rebuild/recompile step.
[[nodiscard]] constexpr bool is_conditionally_compatible(CompatibilityOutcome outcome) noexcept {
  return outcome == CompatibilityOutcome::COMPATIBLE_WITH_REBUILD ||
         outcome == CompatibilityOutcome::COMPATIBLE_WITH_RECOMPILE ||
         outcome == CompatibilityOutcome::COMPATIBLE_WITH_CONVERSION;
}

/// Named reason corresponding to an incompatible outcome.
[[nodiscard]] ReasonCode reason_for(CompatibilityOutcome outcome) noexcept;

/// What a model version generation declares it needs.
struct ModelRequirements {
  /// Serving/training runtime family, e.g. "tensorrt-llm", "vllm", "onnxruntime".
  std::string runtime{};
  /// Minimum acceptable runtime version when the environment reports one.
  std::string runtime_min_version{};
  /// Execution backend, e.g. "cuda", "rocm", "metal", "cpu".
  std::string backend{};
  /// Required accelerator architecture, e.g. "sm_120", "gfx942", "x86_64".
  std::string architecture{};
  /// Required numeric precision, e.g. "fp8", "bf16", "int4".
  std::string precision{};
  /// Tokenizer/config generation the artifact set was built against.
  std::string tokenizer_generation{};
  /// Adapter set identity when adapters participate in the lifecycle contract.
  std::string adapter_set{};

  [[nodiscard]] bool empty() const noexcept {
    return runtime.empty() && backend.empty() && architecture.empty() && precision.empty();
  }
};

/// What an environment reports about itself. Supplied by the caller; the
/// lifecycle runtime records it and never invents it.
struct EnvironmentProfile {
  /// Stable key under which compatibility facts are published.
  std::string key{};
  std::string runtime{};
  std::string runtime_version{};
  std::string backend{};
  std::string backend_version{};
  std::string architecture{};
  std::set<std::string> precisions{};
  std::string tokenizer_generation{};
  std::string adapter_set{};
  /// Numeric accelerator capability where meaningful, e.g. 1200 for sm_120.
  std::uint32_t compute_capability{0};
  Provenance provenance{Provenance::Unknown};

  [[nodiscard]] bool valid() const noexcept { return !key.empty() && key.size() <= 128; }
};

/// One published compatibility fact for a (model generation, environment) pair.
struct CompatibilityFact {
  ModelId model{};
  ModelVersionId version{};
  ModelGeneration model_generation{};
  ArtifactSetId artifact_set{};
  ArtifactGeneration artifact_generation{};
  CompatibilityGeneration generation{};
  std::string environment_key{};
  CompatibilityOutcome outcome{CompatibilityOutcome::UNKNOWN};
  std::string detail{};
  Provenance provenance{Provenance::Unknown};
  Sequence sequence{};
};

/// Structured result of a compatibility evaluation.
struct CompatibilityResult {
  CompatibilityOutcome outcome{CompatibilityOutcome::UNKNOWN};
  Decision decision{OutcomeCode::UNSUPPORTED};
};

/// Compare dotted numeric versions lexicographically by component. Returns -1, 0
/// or 1. Non-numeric components compare as strings; a shorter version that is a
/// prefix of a longer one compares less.
[[nodiscard]] int compare_versions(std::string_view a, std::string_view b) noexcept;

/// Derive the canonical architecture token for a CUDA compute capability.
[[nodiscard]] std::string cuda_architecture_token(std::uint32_t major, std::uint32_t minor);

/// Evaluate declared requirements against a concrete environment profile. This
/// is a pure, deterministic function of its inputs.
[[nodiscard]] CompatibilityResult evaluate_requirements(const ModelRequirements& requirements,
                                                        const EnvironmentProfile& environment);

/// Bounded store of published compatibility facts, one current fact per
/// (model version, environment key).
class CompatibilityRegistry {
 public:
  explicit CompatibilityRegistry(std::size_t max_facts = 8192) : max_facts_(max_facts) {}

  /// Publish a fact. A fact whose model, artifact or compatibility generation is
  /// older than the stored one is refused rather than overwriting newer truth.
  Decision publish(const CompatibilityFact& fact, CompatibilityGeneration assigned_generation);

  [[nodiscard]] const CompatibilityFact* lookup(ModelVersionId version,
                                                std::string_view environment_key) const;

  /// All facts for a version, in deterministic environment-key order.
  [[nodiscard]] std::vector<const CompatibilityFact*> facts_for(ModelVersionId version) const;

  /// Latest generation published for a version, across environments. Zero when
  /// nothing has been published.
  [[nodiscard]] CompatibilityGeneration latest_generation(ModelVersionId version) const;

  /// Invalidate every fact for a version because its model or artifact
  /// generation moved on. Returns the number of facts dropped.
  std::size_t invalidate_version(ModelVersionId version);

  [[nodiscard]] std::size_t size() const noexcept { return facts_.size(); }
  [[nodiscard]] std::size_t max_facts() const noexcept { return max_facts_; }
  void set_max_facts(std::size_t value) noexcept { max_facts_ = value; }

  [[nodiscard]] const std::map<std::pair<std::uint64_t, std::string>, CompatibilityFact>& raw()
      const noexcept {
    return facts_;
  }

  /// Replace the whole registry during recovery. Validates generations and
  /// bounds; on failure the registry is untouched.
  [[nodiscard]] bool restore(std::map<std::pair<std::uint64_t, std::string>, CompatibilityFact> facts);

 private:
  std::map<std::pair<std::uint64_t, std::string>, CompatibilityFact> facts_{};
  std::size_t max_facts_{8192};
};

}  // namespace mlf
