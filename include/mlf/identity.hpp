// Model Lifecycle Fabric — strongly typed identities and generations.
//
// Every generation below corresponds to state that can become stale, superseded,
// or semantically different. No decorative generations.
#pragma once

#include <compare>
#include <cstdint>
#include <functional>
#include <ostream>
#include <string>
#include <string_view>

namespace mlf {

/// Strongly typed scalar identity rooted at a distinct tag type.
template <class Tag, class Rep = std::uint64_t>
class Id {
 public:
  using rep_type = Rep;

  constexpr Id() noexcept = default;
  constexpr explicit Id(Rep value) noexcept : value_(value) {}

  /// Construct without the explicit qualifier where a raw value is already trusted.
  static constexpr Id from_raw(Rep value) noexcept { return Id(value); }

  [[nodiscard]] constexpr Rep raw() const noexcept { return value_; }
  [[nodiscard]] constexpr bool valid() const noexcept { return value_ != Rep{0}; }

  friend constexpr bool operator==(Id a, Id b) noexcept { return a.value_ == b.value_; }
  friend constexpr std::strong_ordering operator<=>(Id a, Id b) noexcept {
    return a.value_ <=> b.value_;
  }

 private:
  Rep value_{0};
};

// ---------------------------------------------------------------------------
// Tag types. One tag per real lifecycle or stale-state boundary.
// ---------------------------------------------------------------------------

struct ModelIdTag;
struct ModelVersionIdTag;
struct ModelGenerationTag;
struct ArtifactSetIdTag;
struct ArtifactGenerationTag;
struct LifecycleGenerationTag;
struct PromotionIdTag;
struct PromotionGenerationTag;
struct RollbackIdTag;
struct RollbackGenerationTag;
struct RolloutIdTag;
struct RolloutGenerationTag;
struct RolloutStageIdTag;
struct RolloutStageGenerationTag;
struct CohortIdTag;
struct ScopeIdTag;
struct ScopeGenerationTag;
struct ReplicaIdTag;
struct ReplicaGenerationTag;
struct ResidencyGenerationTag;
struct CompatibilityGenerationTag;
struct PolicyGenerationTag;
struct HealthGenerationTag;
struct ReadinessGenerationTag;
struct EvidenceGenerationTag;
struct EvidenceIdTag;
struct WorkerIdTag;
struct WorkerBootIdTag;
struct CoordinatorEpochTag;
struct SnapshotGenerationTag;
struct SequenceTag;
struct AttemptIdTag;
struct RevisionTag;

/// Stable identity of a model family (e.g. "llama-3-8b-instruct").
using ModelId = Id<ModelIdTag>;
/// Identity of one version of a model family (e.g. "2.4.1").
using ModelVersionId = Id<ModelVersionIdTag>;
/// Revisions of a version's *declared identity content*: artifact-set binding,
/// required runtime/backend, architecture, tokenizer/config. Bumped by revision;
/// any plan or evidence bound to a prior model generation is stale.
using ModelGeneration = Id<ModelGenerationTag>;
/// Identity of a concrete artifact set (weights + config + tokenizer + kernels).
using ArtifactSetId = Id<ArtifactSetIdTag>;
/// Content revision of an artifact set. Bumped when bytes are replaced.
using ArtifactGeneration = Id<ArtifactGenerationTag>;
/// Increments on every committed lifecycle-state transition of a version.
/// Guards optimistic concurrency over lifecycle state.
using LifecycleGeneration = Id<LifecycleGenerationTag>;
/// Identity of one promotion attempt.
using PromotionId = Id<PromotionIdTag>;
/// Increments per promotion attempt against a version.
using PromotionGeneration = Id<PromotionGenerationTag>;
/// Identity of one rollback attempt.
using RollbackId = Id<RollbackIdTag>;
/// Increments per rollback attempt against a rollout lineage. At most one
/// rollback outcome may commit per rollback generation.
using RollbackGeneration = Id<RollbackGenerationTag>;
/// Identity of one rollout plan.
using RolloutId = Id<RolloutIdTag>;
/// Increments per rollout plan revision for a candidate lineage; stage completions
/// cite this value and a mismatch is rejected.
using RolloutGeneration = Id<RolloutGenerationTag>;
/// Identity of one stage within a rollout plan.
using RolloutStageId = Id<RolloutStageIdTag>;
/// Increments each time a stage is entered; stage feedback must cite the value
/// current at entry.
using RolloutStageGeneration = Id<RolloutStageGenerationTag>;
/// Identity of a canary cohort.
using CohortId = Id<CohortIdTag>;
/// Identity of a rollout scope.
using ScopeId = Id<ScopeIdTag>;
/// Increments when a scope's structural position or policy binding changes.
using ScopeGeneration = Id<ScopeGenerationTag>;
/// Identity of a serving replica incarnation.
using ReplicaId = Id<ReplicaIdTag>;
/// Incarnation counter for a replica; a restarted replica has a fresh generation.
using ReplicaGeneration = Id<ReplicaGenerationTag>;
/// Increments per residency intent or residency observation for a subject.
using ResidencyGeneration = Id<ResidencyGenerationTag>;
/// Increments per published compatibility fact set.
using CompatibilityGeneration = Id<CompatibilityGenerationTag>;
/// Increments per lifecycle policy revision.
using PolicyGeneration = Id<PolicyGenerationTag>;
/// Increments per published health observation stream for a subject.
using HealthGeneration = Id<HealthGenerationTag>;
/// Increments per published readiness observation stream for a subject.
using ReadinessGeneration = Id<ReadinessGenerationTag>;
/// Global monotonic evidence sequence for a subject.
using EvidenceGeneration = Id<EvidenceGenerationTag>;
/// Identity of one evidence record.
using EvidenceId = Id<EvidenceIdTag>;
/// Identity of a lifecycle worker (stable across boots).
using WorkerId = Id<WorkerIdTag>;
/// Identity of one worker process incarnation. A dead boot permanently loses
/// authority; a replacement worker requires a fresh boot identity.
using WorkerBootId = Id<WorkerBootIdTag>;
/// Epoch of the lifecycle coordinator incarnation. Advances on every restart.
using CoordinatorEpoch = Id<CoordinatorEpochTag>;
/// Increments per persisted snapshot.
using SnapshotGeneration = Id<SnapshotGenerationTag>;
/// Engine-wide sequence watermark for durable replay.
using Sequence = Id<SequenceTag>;
/// Identity of one external side-effect attempt.
using AttemptId = Id<AttemptIdTag>;
/// Generic monotonic revision counter.
using Revision = Id<RevisionTag>;

/// Namespace for generation helpers.
constexpr std::uint64_t kFirstGeneration = 1;

/// Deterministically advance a generation. Saturates rather than wrapping: a
/// wrapped generation could collide with a retired value.
template <class Tag, class Rep>
[[nodiscard]] constexpr Id<Tag, Rep> next_generation(Id<Tag, Rep> current) noexcept {
  const Rep value = current.raw();
  const Rep advanced = value + Rep{1};
  return advanced > value ? Id<Tag, Rep>(advanced) : Id<Tag, Rep>(~Rep{0});
}

/// Render an id for logs, CLI output, and deterministic explanations.
template <class Tag, class Rep>
[[nodiscard]] std::string to_string(Id<Tag, Rep> id) {
  return std::to_string(static_cast<unsigned long long>(id.raw()));
}

template <class Tag, class Rep>
std::ostream& operator<<(std::ostream& stream, Id<Tag, Rep> id) {
  return stream << static_cast<unsigned long long>(id.raw());
}

/// Parse a decimal identity. Returns an invalid id on any malformed input.
template <class Tag, class Rep>
[[nodiscard]] bool parse_id(std::string_view text, Id<Tag, Rep>& out) noexcept {
  if (text.empty() || text.size() > 20) return false;
  Rep value = 0;
  for (char c : text) {
    if (c < '0' || c > '9') return false;
    const Rep digit = static_cast<Rep>(c - '0');
    const Rep scaled = value * Rep{10};
    if (scaled / Rep{10} != value) return false;
    const Rep summed = scaled + digit;
    if (summed < scaled) return false;
    value = summed;
  }
  out = Id<Tag, Rep>(value);
  return true;
}

}  // namespace mlf

namespace std {

#define MLF_HASH_ID(TYPE)                                              \
  template <>                                                          \
  struct hash<TYPE> {                                                  \
    size_t operator()(const TYPE& id) const noexcept {                 \
      return std::hash<unsigned long long>{}(                          \
          static_cast<unsigned long long>(id.raw()));                  \
    }                                                                  \
  }

MLF_HASH_ID(::mlf::ModelId);
MLF_HASH_ID(::mlf::ModelVersionId);
MLF_HASH_ID(::mlf::ModelGeneration);
MLF_HASH_ID(::mlf::ArtifactSetId);
MLF_HASH_ID(::mlf::ArtifactGeneration);
MLF_HASH_ID(::mlf::LifecycleGeneration);
MLF_HASH_ID(::mlf::PromotionId);
MLF_HASH_ID(::mlf::PromotionGeneration);
MLF_HASH_ID(::mlf::RollbackId);
MLF_HASH_ID(::mlf::RollbackGeneration);
MLF_HASH_ID(::mlf::RolloutId);
MLF_HASH_ID(::mlf::RolloutGeneration);
MLF_HASH_ID(::mlf::RolloutStageId);
MLF_HASH_ID(::mlf::RolloutStageGeneration);
MLF_HASH_ID(::mlf::CohortId);
MLF_HASH_ID(::mlf::ScopeId);
MLF_HASH_ID(::mlf::ScopeGeneration);
MLF_HASH_ID(::mlf::ReplicaId);
MLF_HASH_ID(::mlf::ReplicaGeneration);
MLF_HASH_ID(::mlf::ResidencyGeneration);
MLF_HASH_ID(::mlf::CompatibilityGeneration);
MLF_HASH_ID(::mlf::PolicyGeneration);
MLF_HASH_ID(::mlf::HealthGeneration);
MLF_HASH_ID(::mlf::ReadinessGeneration);
MLF_HASH_ID(::mlf::EvidenceGeneration);
MLF_HASH_ID(::mlf::EvidenceId);
MLF_HASH_ID(::mlf::WorkerId);
MLF_HASH_ID(::mlf::WorkerBootId);
MLF_HASH_ID(::mlf::CoordinatorEpoch);
MLF_HASH_ID(::mlf::SnapshotGeneration);
MLF_HASH_ID(::mlf::Sequence);
MLF_HASH_ID(::mlf::AttemptId);
MLF_HASH_ID(::mlf::Revision);

#undef MLF_HASH_ID

}  // namespace std
