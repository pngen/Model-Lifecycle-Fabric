// Model Lifecycle Fabric — rollout plans, cohorts, stages, immutable history.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "mlf/artifact.hpp"
#include "mlf/evidence.hpp"
#include "mlf/identity.hpp"
#include "mlf/provenance.hpp"
#include "mlf/scope.hpp"
#include "mlf/status.hpp"

namespace mlf {

/// Generic rollout strategy. The runtime never assumes a single deployment
/// platform: every strategy below is expressed through the same plan structure.
enum class RolloutStrategy : std::uint8_t {
  Canary = 0,
  Percentage,
  ScopeByScope,
  BlueGreen,
  ManualStages,
  ImmediateCutover,
};

inline constexpr std::size_t kRolloutStrategyCount = 6;

[[nodiscard]] std::string_view to_string(RolloutStrategy strategy) noexcept;
[[nodiscard]] bool parse_rollout_strategy(std::string_view text, RolloutStrategy& out) noexcept;

/// Outcome recorded for a stage. A stage record is written once and never
/// rewritten: history is append-only.
enum class StageDecision : std::uint8_t {
  Pending = 0,
  Entered,
  Advanced,
  Failed,
  RolledBack,
  Superseded,
};

[[nodiscard]] std::string_view to_string(StageDecision decision) noexcept;

enum class CriterionComparison : std::uint8_t {
  AtLeast = 0,
  AtMost,
};

[[nodiscard]] std::string_view to_string(CriterionComparison comparison) noexcept;

/// A numeric acceptance or failure criterion over a piece of evidence.
struct Criterion {
  EvidenceKind kind{EvidenceKind::HealthCheck};
  CriterionComparison comparison{CriterionComparison::AtLeast};
  double threshold{0.0};
};

/// Immutable record of one stage entry and its committed decision.
struct StageRecord {
  RolloutStageId stage{};
  RolloutStageGeneration generation{};
  Sequence entered_sequence{};
  Sequence decided_sequence{};
  StageDecision decision{StageDecision::Pending};
  std::vector<Reason> reasons{};
};

/// A named set of scopes that a stage may move.
struct CohortDefinition {
  CohortId id{};
  std::string name{};
  std::vector<ScopeId> scopes{};
  std::uint32_t traffic_percent{0};
  /// Zero inherits the plan blast radius.
  std::uint32_t max_blast_radius_percent{0};
};

/// One ordered stage of a rollout plan.
struct StageDefinition {
  RolloutStageId id{};
  std::string name{};
  std::vector<CohortId> cohorts{};
  std::vector<EvidenceRequirement> required_evidence{};
  std::vector<Criterion> acceptance{};
  std::vector<Criterion> failure{};
  bool require_residency_before_entry{false};
  bool require_drain_after_commit{false};
};

/// A rollout plan. Every binding below is a staleness boundary: if any of them
/// changes materially before the plan commits, the plan is stale and must not be
/// executed.
struct RolloutPlan {
  RolloutId id{};
  RolloutGeneration generation{};
  ModelId model{};
  ModelVersionId candidate{};
  ModelGeneration candidate_generation{};
  ArtifactSetId candidate_artifact_set{};
  ArtifactGeneration candidate_artifact_generation{};
  ModelVersionId previous{};
  ModelGeneration previous_generation{};
  CompatibilityGeneration compatibility_generation{};
  PolicyGeneration policy_generation{};
  EvidenceGeneration evidence_generation{};
  ScopeId root_scope{};
  ScopeGeneration root_scope_generation{};
  RolloutStrategy strategy{RolloutStrategy::Canary};
  PromotionId promotion{};
  PromotionGeneration promotion_generation{};
  std::vector<CohortDefinition> cohorts{};
  std::vector<StageDefinition> stages{};
  ModelVersionId rollback_target{};
  ModelGeneration rollback_target_generation{};
  std::uint32_t max_blast_radius_percent{100};
  bool manual_progression{false};

  // --- durable progress ---------------------------------------------------
  std::size_t current_stage_index{0};
  RolloutStageGeneration current_stage_generation{};
  bool stage_entered{false};
  std::vector<StageRecord> history{};
  bool completed{false};
  bool failed{false};
  bool superseded{false};
  /// The single rollback that may commit against this plan. A plan accepts at
  /// most one rollback identity; further attempts are refused.
  RollbackId last_rollback{};
  RollbackGeneration last_rollback_generation{};
  Sequence created_sequence{};
  Sequence updated_sequence{};
  Provenance provenance{Provenance::Unknown};

  [[nodiscard]] const StageDefinition* stage_at(std::size_t index) const noexcept {
    return index < stages.size() ? &stages[index] : nullptr;
  }
  [[nodiscard]] const CohortDefinition* cohort(CohortId cohort_id) const noexcept {
    for (const CohortDefinition& entry : cohorts) {
      if (entry.id == cohort_id) return &entry;
    }
    return nullptr;
  }
  /// Scopes the plan has committed to so far, in deterministic order.
  [[nodiscard]] std::vector<ScopeId> committed_scopes() const;
  /// Scopes reached by stages up to and including the given index.
  [[nodiscard]] std::vector<ScopeId> scopes_through(std::size_t index) const;
  [[nodiscard]] bool live() const noexcept {
    return !completed && !failed && !superseded;
  }
};

}  // namespace mlf
