// Model Lifecycle Fabric — model and model-version records.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "mlf/artifact.hpp"
#include "mlf/compatibility.hpp"
#include "mlf/identity.hpp"
#include "mlf/lifecycle.hpp"
#include "mlf/provenance.hpp"
#include "mlf/status.hpp"

namespace mlf {

/// Stable identity of a model family.
struct ModelRecord {
  ModelId id{};
  /// Stable family name, e.g. "llama-3-8b-instruct". Unique.
  std::string name{};
  /// Coarse family/lineage label used for reporting only.
  std::string family{};
  Provenance provenance{Provenance::Unknown};
  Sequence created_sequence{};
};

/// One version of a model family, bound to an exact artifact generation and an
/// exact lifecycle generation.
///
/// A version label is identity metadata. It is never lifecycle authority: two
/// records with the same label at different generations are different
/// authorities, and a retired label may not be reused unless policy allows it.
struct ModelVersionRecord {
  ModelId model{};
  ModelVersionId version{};
  /// Human-facing label such as "2.4.1-rc2".
  std::string label{};

  /// Revisions of declared identity content: artifact binding, requirements,
  /// predecessor. Bumped by revise(); every plan and evidence record bound to a
  /// previous value is stale.
  ModelGeneration generation{ModelGeneration(kFirstGeneration)};

  /// Predecessor version this generation succeeds, when it has one.
  ModelVersionId predecessor{};
  ModelGeneration predecessor_generation{};

  ArtifactBinding artifact{};
  ModelRequirements requirements{};

  LifecycleState state{LifecycleState::REGISTERED};
  /// Increments on every committed lifecycle transition. Optimistic concurrency
  /// token for lifecycle mutation.
  LifecycleGeneration lifecycle_generation{LifecycleGeneration(kFirstGeneration)};

  /// Latest promotion attempt recorded against this version.
  PromotionId last_promotion{};
  PromotionGeneration promotion_generation{};
  /// Active or most recent rollout for this version.
  RolloutId rollout{};
  RolloutGeneration rollout_generation{};
  RolloutStageId current_stage{};
  RolloutStageGeneration current_stage_generation{};

  /// Latest compatibility generation published for this version. Zero means no
  /// compatibility has been established.
  CompatibilityGeneration compatibility_generation{};

  /// Latest rollback attempt recorded against this version.
  RollbackId last_rollback{};
  RollbackGeneration rollback_generation{};

  Provenance provenance{Provenance::Unknown};
  Sequence created_sequence{};
  Sequence updated_sequence{};

  [[nodiscard]] bool retired() const noexcept { return state == LifecycleState::RETIRED; }
  [[nodiscard]] bool failed() const noexcept { return state == LifecycleState::FAILED; }
  [[nodiscard]] bool has_compatibility() const noexcept {
    return compatibility_generation.valid();
  }
};

}  // namespace mlf
