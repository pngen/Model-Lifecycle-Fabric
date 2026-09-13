// Model Lifecycle Fabric — structured outcomes, named reasons, explanations.
//
// Callers never parse prose: every decision is a stable enumerated outcome plus
// a deterministically ordered list of named reasons.
#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace mlf {

/// Coarse decision outcome. Stable, machine-readable, never prose.
enum class OutcomeCode : std::uint8_t {
  Ok = 0,
  Accepted,
  Rejected,
  PROMOTION_ALLOWED,
  PROMOTION_BLOCKED,
  CANARY_ALLOWED,
  CANARY_BLOCKED,
  STAGE_ADVANCE_ALLOWED,
  STAGE_ADVANCE_BLOCKED,
  ROLLBACK_ALLOWED,
  ROLLBACK_BLOCKED,
  RETIREMENT_ALLOWED,
  RETIREMENT_BLOCKED,
  REVALIDATION_REQUIRED,
  ROLLBACK_DECISION_REQUIRED,
  STALE_PLAN,
  STALE_EVIDENCE,
  INCOMPATIBLE,
  NO_AUTHORITATIVE_MODEL,
  AMBIGUOUS,
  OUTCOME_UNKNOWN,
  RECONCILIATION_REQUIRED,
  CONFLICT,
  BOUNDS_EXCEEDED,
  NOT_FOUND,
  UNSUPPORTED,
};

[[nodiscard]] constexpr std::string_view to_string(OutcomeCode code) noexcept {
  switch (code) {
    case OutcomeCode::Ok:                       return "OK";
    case OutcomeCode::Accepted:                 return "ACCEPTED";
    case OutcomeCode::Rejected:                 return "REJECTED";
    case OutcomeCode::PROMOTION_ALLOWED:        return "PROMOTION_ALLOWED";
    case OutcomeCode::PROMOTION_BLOCKED:        return "PROMOTION_BLOCKED";
    case OutcomeCode::CANARY_ALLOWED:           return "CANARY_ALLOWED";
    case OutcomeCode::CANARY_BLOCKED:           return "CANARY_BLOCKED";
    case OutcomeCode::STAGE_ADVANCE_ALLOWED:    return "STAGE_ADVANCE_ALLOWED";
    case OutcomeCode::STAGE_ADVANCE_BLOCKED:    return "STAGE_ADVANCE_BLOCKED";
    case OutcomeCode::ROLLBACK_ALLOWED:         return "ROLLBACK_ALLOWED";
    case OutcomeCode::ROLLBACK_BLOCKED:         return "ROLLBACK_BLOCKED";
    case OutcomeCode::RETIREMENT_ALLOWED:       return "RETIREMENT_ALLOWED";
    case OutcomeCode::RETIREMENT_BLOCKED:       return "RETIREMENT_BLOCKED";
    case OutcomeCode::REVALIDATION_REQUIRED:    return "REVALIDATION_REQUIRED";
    case OutcomeCode::ROLLBACK_DECISION_REQUIRED: return "ROLLBACK_DECISION_REQUIRED";
    case OutcomeCode::STALE_PLAN:               return "STALE_PLAN";
    case OutcomeCode::STALE_EVIDENCE:           return "STALE_EVIDENCE";
    case OutcomeCode::INCOMPATIBLE:             return "INCOMPATIBLE";
    case OutcomeCode::NO_AUTHORITATIVE_MODEL:   return "NO_AUTHORITATIVE_MODEL";
    case OutcomeCode::AMBIGUOUS:                return "AMBIGUOUS";
    case OutcomeCode::OUTCOME_UNKNOWN:          return "OUTCOME_UNKNOWN";
    case OutcomeCode::RECONCILIATION_REQUIRED:  return "RECONCILIATION_REQUIRED";
    case OutcomeCode::CONFLICT:                 return "CONFLICT";
    case OutcomeCode::BOUNDS_EXCEEDED:          return "BOUNDS_EXCEEDED";
    case OutcomeCode::NOT_FOUND:                return "NOT_FOUND";
    case OutcomeCode::UNSUPPORTED:              return "UNSUPPORTED";
  }
  return "REJECTED";
}

[[nodiscard]] constexpr bool is_positive(OutcomeCode code) noexcept {
  return code == OutcomeCode::Ok || code == OutcomeCode::Accepted ||
         code == OutcomeCode::PROMOTION_ALLOWED || code == OutcomeCode::CANARY_ALLOWED ||
         code == OutcomeCode::STAGE_ADVANCE_ALLOWED || code == OutcomeCode::ROLLBACK_ALLOWED ||
         code == OutcomeCode::RETIREMENT_ALLOWED;
}

/// Named reason a decision came out the way it did. Exhaustive and stable.
enum class ReasonCode : std::uint16_t {
  None = 0,

  // --- registration / registry -------------------------------------------
  DuplicateModel, DuplicateVersion, UnknownModel, UnknownVersion, UnknownScope,
  InvalidName, InvalidVersionLabel, InvalidDigest, ParentVersionMissing,
  PredecessorCycle, VersionSuperseded,

  // --- bounds -------------------------------------------------------------
  BoundsModelCount, BoundsVersionCount, BoundsScopeCount, BoundsRolloutCount,
  BoundsStageCount, BoundsCohortCount, BoundsWorkerCount, BoundsEvidenceCount,
  BoundsAttemptCount, BoundsHistoryCount, BoundsPayloadSize, BoundsStringLength,

  // --- lifecycle ----------------------------------------------------------
  InvalidLifecycleTransition, LifecycleGenerationMismatch, VersionRetired,
  VersionFailed, VersionNotPromotionEligible, VersionAlreadyCurrent,

  // --- artifacts ----------------------------------------------------------
  ArtifactSetMissing, ArtifactGenerationMismatch, ArtifactDigestMismatch,
  ArtifactIntegrityStale,

  // --- compatibility ------------------------------------------------------
  CompatibilityMissing, CompatibilityStale, CompatibilityGenerationMismatch,
  IncompatibleArtifact, IncompatibleRuntime, IncompatibleBackend,
  IncompatibleArchitecture, IncompatiblePrecision, IncompatibleTokenizer,
  IncompatibleAdapter, IncompatiblePolicy, CompatibilityUnknown,
  CompatibilityRequiresRebuild, CompatibilityRequiresRecompile,
  CompatibilityRequiresConversion, HardwareCapabilityInsufficient,

  // --- policy -------------------------------------------------------------
  PolicyGenerationMismatch, PolicyDeniesPromotion, PolicyDeniesCoexistence,
  PolicyDeniesAutoRollback, PolicyRequiresRollbackTarget, PolicyScopeNotTargetable,
  PolicyRequiresEvidence, PolicyRequiresDrain, PolicyRequiresApproval,
  CoexistenceLimitExceeded, BlastRadiusExceeded,

  // --- evidence -----------------------------------------------------------
  EvidenceMissing, EvidenceStale, EvidenceSubjectMismatch,
  EvidenceGenerationMismatch, EvidencePublisherUnknown, EvidencePublisherBootStale,
  EvidenceSubjectModelGenerationMismatch, EvidenceSubjectArtifactMismatch,
  EvidenceSubjectCompatibilityMismatch, EvidenceUnsatisfiedCriteria,
  EvidenceConflicting, EvidenceUnknownProvenance,

  // --- ordering / generation freshness ------------------------------------
  ModelGenerationMismatch, ScopeGenerationMismatch, RolloutGenerationMismatch,
  RolloutStageGenerationMismatch, PromotionGenerationMismatch,
  CoordinatorEpochStale, SnapshotGenerationStale, SequenceRegression,

  // --- promotion ----------------------------------------------------------
  PromotionNotAuthorized, PromotionCommitDuplicate, PromotionCandidateRetired,
  PromotionScopeInvalid, PromotionRequiresRolloutPlan,

  // --- rollout / canary ---------------------------------------------------
  RolloutPlanStale, RolloutNotActive, RolloutAlreadyComplete, StageOrderViolation,
  StageEntryConditionUnmet, StageAcceptanceNotMet, StageFailureThresholdExceeded,
  StageSkipped, CohortUnknown, CohortScopeInvalid, StageCompletionDuplicate,
  RolloutSuperseded, StageFeedbackIncomplete, DrainRequired,

  // --- rollback -----------------------------------------------------------
  RollbackTargetRequired, RollbackTargetUnknown, RollbackTargetRetired,
  RollbackTargetArtifactMissing, RollbackTargetIncompatible, RollbackCommitDuplicate,
  RollbackGenerationMismatch, NoCurrentAuthority, RollbackNotAuthorized,
  RollbackSupersededByNewerGeneration,

  // --- retirement ---------------------------------------------------------
  RetirementScopeDependency, RetirementRolloutActive, RetirementRetentionUnsatisfied,
  RetirementDrainIncomplete, RetirementDependencyUnresolved, RetirementAlreadyRetired,

  // --- residency / readiness ---------------------------------------------
  ResidencyIntentMissing, ResidencyGenerationMismatch, ResidencyNotResident,
  ReadinessMissing, ReadinessStale, ReadinessSubjectMismatch, WarmupIncomplete,

  // --- workers / distributed authority ------------------------------------
  WorkerUnknown, WorkerBootStale, WorkerNotRegistered, WorkerNotCurrent,
  WorkerScopeNotOwned, CoordinatorEpochMismatch, AttemptUnknown,
  AttemptAlreadyCompleted, AttemptConflictingCompletion, AttemptOutcomeUnknown,

  // --- reconciliation -----------------------------------------------------
  ReplicaEvidenceMissing, ExternalStateDiverged, ExpectedResidentMissing,
  RetiredModelStillResident, RolloutStageWithoutWorkers, UnreconciledAttempt,

  // --- protocol -----------------------------------------------------------
  ProtocolWrongMagic, ProtocolUnsupportedVersion, ProtocolUnknownType,
  ProtocolOversized, ProtocolTruncated, ProtocolIntegrityFailure,
  ProtocolDuplicateHello, ProtocolInvalidScope, ProtocolInvalidTransition,
  ProtocolDecodeFailure,

  // --- transaction --------------------------------------------------------
  TransactionRolledBack, UnsupportedCapability,
};

[[nodiscard]] std::string_view to_string(ReasonCode code) noexcept;

/// A named reason plus bounded human-readable detail. Detail never carries
/// decision semantics; the code does.
struct Reason {
  ReasonCode code{ReasonCode::None};
  std::string detail{};

  Reason() = default;
  explicit Reason(ReasonCode c) : code(c) {}
  Reason(ReasonCode c, std::string d) : code(c), detail(std::move(d)) {}

  friend bool operator==(const Reason& a, const Reason& b) {
    return a.code == b.code && a.detail == b.detail;
  }
  /// Deterministic total order: by code, then detail. Stable state produces
  /// stable reason ordering.
  friend bool operator<(const Reason& a, const Reason& b) {
    if (a.code != b.code) return static_cast<std::uint16_t>(a.code) <
                                 static_cast<std::uint16_t>(b.code);
    return a.detail < b.detail;
  }
};

/// A structured decision.
class Decision {
 public:
  Decision() = default;
  explicit Decision(OutcomeCode outcome) : outcome_(outcome) {}
  Decision(OutcomeCode outcome, Reason reason) : outcome_(outcome) {
    add(std::move(reason));
  }

  Decision& add(ReasonCode code) {
    add(Reason(code));
    return *this;
  }
  Decision& add(ReasonCode code, std::string detail) {
    add(Reason(code, std::move(detail)));
    return *this;
  }
  Decision& add(Reason reason) {
    if (reason.code == ReasonCode::None) return *this;
    reasons_.push_back(std::move(reason));
    return *this;
  }
  /// Add a reason only when the condition holds.
  Decision& add_if(bool condition, ReasonCode code) {
    if (condition) add(code);
    return *this;
  }

  [[nodiscard]] OutcomeCode outcome() const noexcept { return outcome_; }
  void set_outcome(OutcomeCode outcome) noexcept { outcome_ = outcome; }

  [[nodiscard]] bool allowed() const noexcept { return is_positive(outcome_); }
  [[nodiscard]] bool has(ReasonCode code) const noexcept {
    for (const Reason& r : reasons_) {
      if (r.code == code) return true;
    }
    return false;
  }
  [[nodiscard]] std::size_t size() const noexcept { return reasons_.size(); }
  [[nodiscard]] bool empty() const noexcept { return reasons_.empty(); }
  [[nodiscard]] const std::vector<Reason>& reasons() const noexcept { return reasons_; }

  /// Canonical ordering. Two decisions built in different orders but carrying
  /// the same reasons compare equal after normalization.
  void normalize() {
    std::sort(reasons_.begin(), reasons_.end());
    reasons_.erase(std::unique(reasons_.begin(), reasons_.end()), reasons_.end());
  }

  [[nodiscard]] std::string render() const;

 private:
  OutcomeCode outcome_{OutcomeCode::Rejected};
  std::vector<Reason> reasons_{};
};

/// Deterministic, ordered key/value bindings describing why an outcome holds.
class Explanation {
 public:
  Explanation() = default;
  explicit Explanation(OutcomeCode outcome) : outcome_(outcome) {}

  Explanation& set(std::string key, std::string value) {
    for (auto& kv : bindings_) {
      if (kv.first == key) {
        kv.second = std::move(value);
        return *this;
      }
    }
    bindings_.emplace_back(std::move(key), std::move(value));
    return *this;
  }
  Explanation& set(std::string key, const char* value) {
    return set(std::move(key), std::string(value == nullptr ? "" : value));
  }
  /// Numeric bindings are stringified. Restricted to arithmetic types so that a
  /// string literal or a string view can never be captured by accident.
  template <class T>
    requires std::is_arithmetic_v<T>
  Explanation& set(std::string key, T value) {
    return set(std::move(key), std::to_string(value));
  }

  Explanation& add(Reason reason) {
    if (reason.code == ReasonCode::None) return *this;
    reasons_.push_back(std::move(reason));
    return *this;
  }
  Explanation& add(ReasonCode code) { return add(Reason(code)); }
  Explanation& add(ReasonCode code, std::string detail) {
    return add(Reason(code, std::move(detail)));
  }

  [[nodiscard]] OutcomeCode outcome() const noexcept { return outcome_; }
  void set_outcome(OutcomeCode outcome) noexcept { outcome_ = outcome; }

  [[nodiscard]] const std::vector<std::pair<std::string, std::string>>& bindings() const noexcept {
    return bindings_;
  }
  [[nodiscard]] const std::vector<Reason>& reasons() const noexcept { return reasons_; }

  [[nodiscard]] const std::string* find(std::string_view key) const noexcept {
    for (const auto& kv : bindings_) {
      if (kv.first == key) return &kv.second;
    }
    return nullptr;
  }

  void normalize() {
    std::sort(bindings_.begin(), bindings_.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    std::sort(reasons_.begin(), reasons_.end());
    reasons_.erase(std::unique(reasons_.begin(), reasons_.end()), reasons_.end());
  }

  /// Stable multi-line rendering. Deterministic for stable state.
  [[nodiscard]] std::string render() const;

 private:
  OutcomeCode outcome_{OutcomeCode::Rejected};
  std::vector<std::pair<std::string, std::string>> bindings_{};
  std::vector<Reason> reasons_{};
};

}  // namespace mlf
