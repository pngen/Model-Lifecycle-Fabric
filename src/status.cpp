#include "mlf/status.hpp"

namespace mlf {
namespace {

struct ReasonName {
  ReasonCode code;
  std::string_view name;
};

// Single source of truth for reason names. Ordered by enumeration value so the
// table is deterministic and auditable against the header.
constexpr ReasonName kReasonNames[] = {
    {ReasonCode::None, "NONE"},
    {ReasonCode::DuplicateModel, "DUPLICATE_MODEL"},
    {ReasonCode::DuplicateVersion, "DUPLICATE_VERSION"},
    {ReasonCode::UnknownModel, "UNKNOWN_MODEL"},
    {ReasonCode::UnknownVersion, "UNKNOWN_VERSION"},
    {ReasonCode::UnknownScope, "UNKNOWN_SCOPE"},
    {ReasonCode::InvalidName, "INVALID_NAME"},
    {ReasonCode::InvalidVersionLabel, "INVALID_VERSION_LABEL"},
    {ReasonCode::InvalidDigest, "INVALID_DIGEST"},
    {ReasonCode::ParentVersionMissing, "PARENT_VERSION_MISSING"},
    {ReasonCode::PredecessorCycle, "PREDECESSOR_CYCLE"},
    {ReasonCode::VersionSuperseded, "VERSION_SUPERSEDED"},
    {ReasonCode::BoundsModelCount, "BOUNDS_MODEL_COUNT"},
    {ReasonCode::BoundsVersionCount, "BOUNDS_VERSION_COUNT"},
    {ReasonCode::BoundsScopeCount, "BOUNDS_SCOPE_COUNT"},
    {ReasonCode::BoundsRolloutCount, "BOUNDS_ROLLOUT_COUNT"},
    {ReasonCode::BoundsStageCount, "BOUNDS_STAGE_COUNT"},
    {ReasonCode::BoundsCohortCount, "BOUNDS_COHORT_COUNT"},
    {ReasonCode::BoundsWorkerCount, "BOUNDS_WORKER_COUNT"},
    {ReasonCode::BoundsEvidenceCount, "BOUNDS_EVIDENCE_COUNT"},
    {ReasonCode::BoundsAttemptCount, "BOUNDS_ATTEMPT_COUNT"},
    {ReasonCode::BoundsHistoryCount, "BOUNDS_HISTORY_COUNT"},
    {ReasonCode::BoundsPayloadSize, "BOUNDS_PAYLOAD_SIZE"},
    {ReasonCode::BoundsStringLength, "BOUNDS_STRING_LENGTH"},
    {ReasonCode::InvalidLifecycleTransition, "INVALID_LIFECYCLE_TRANSITION"},
    {ReasonCode::LifecycleGenerationMismatch, "LIFECYCLE_GENERATION_MISMATCH"},
    {ReasonCode::VersionRetired, "VERSION_RETIRED"},
    {ReasonCode::VersionFailed, "VERSION_FAILED"},
    {ReasonCode::VersionNotPromotionEligible, "VERSION_NOT_PROMOTION_ELIGIBLE"},
    {ReasonCode::VersionAlreadyCurrent, "VERSION_ALREADY_CURRENT"},
    {ReasonCode::ArtifactSetMissing, "ARTIFACT_SET_MISSING"},
    {ReasonCode::ArtifactGenerationMismatch, "ARTIFACT_GENERATION_MISMATCH"},
    {ReasonCode::ArtifactDigestMismatch, "ARTIFACT_DIGEST_MISMATCH"},
    {ReasonCode::ArtifactIntegrityStale, "ARTIFACT_INTEGRITY_STALE"},
    {ReasonCode::CompatibilityMissing, "COMPATIBILITY_MISSING"},
    {ReasonCode::CompatibilityStale, "COMPATIBILITY_STALE"},
    {ReasonCode::CompatibilityGenerationMismatch, "COMPATIBILITY_GENERATION_MISMATCH"},
    {ReasonCode::IncompatibleArtifact, "INCOMPATIBLE_ARTIFACT"},
    {ReasonCode::IncompatibleRuntime, "INCOMPATIBLE_RUNTIME"},
    {ReasonCode::IncompatibleBackend, "INCOMPATIBLE_BACKEND"},
    {ReasonCode::IncompatibleArchitecture, "INCOMPATIBLE_ARCHITECTURE"},
    {ReasonCode::IncompatiblePrecision, "INCOMPATIBLE_PRECISION"},
    {ReasonCode::IncompatibleTokenizer, "INCOMPATIBLE_TOKENIZER"},
    {ReasonCode::IncompatibleAdapter, "INCOMPATIBLE_ADAPTER"},
    {ReasonCode::IncompatiblePolicy, "INCOMPATIBLE_POLICY"},
    {ReasonCode::CompatibilityUnknown, "COMPATIBILITY_UNKNOWN"},
    {ReasonCode::CompatibilityRequiresRebuild, "COMPATIBILITY_REQUIRES_REBUILD"},
    {ReasonCode::CompatibilityRequiresRecompile, "COMPATIBILITY_REQUIRES_RECOMPILE"},
    {ReasonCode::CompatibilityRequiresConversion, "COMPATIBILITY_REQUIRES_CONVERSION"},
    {ReasonCode::HardwareCapabilityInsufficient, "HARDWARE_CAPABILITY_INSUFFICIENT"},
    {ReasonCode::PolicyGenerationMismatch, "POLICY_GENERATION_MISMATCH"},
    {ReasonCode::PolicyDeniesPromotion, "POLICY_DENIES_PROMOTION"},
    {ReasonCode::PolicyDeniesCoexistence, "POLICY_DENIES_COEXISTENCE"},
    {ReasonCode::PolicyDeniesAutoRollback, "POLICY_DENIES_AUTO_ROLLBACK"},
    {ReasonCode::PolicyRequiresRollbackTarget, "POLICY_REQUIRES_ROLLBACK_TARGET"},
    {ReasonCode::PolicyScopeNotTargetable, "POLICY_SCOPE_NOT_TARGETABLE"},
    {ReasonCode::PolicyRequiresEvidence, "POLICY_REQUIRES_EVIDENCE"},
    {ReasonCode::PolicyRequiresDrain, "POLICY_REQUIRES_DRAIN"},
    {ReasonCode::PolicyRequiresApproval, "POLICY_REQUIRES_APPROVAL"},
    {ReasonCode::CoexistenceLimitExceeded, "COEXISTENCE_LIMIT_EXCEEDED"},
    {ReasonCode::BlastRadiusExceeded, "BLAST_RADIUS_EXCEEDED"},
    {ReasonCode::EvidenceMissing, "EVIDENCE_MISSING"},
    {ReasonCode::EvidenceStale, "EVIDENCE_STALE"},
    {ReasonCode::EvidenceSubjectMismatch, "EVIDENCE_SUBJECT_MISMATCH"},
    {ReasonCode::EvidenceGenerationMismatch, "EVIDENCE_GENERATION_MISMATCH"},
    {ReasonCode::EvidencePublisherUnknown, "EVIDENCE_PUBLISHER_UNKNOWN"},
    {ReasonCode::EvidencePublisherBootStale, "EVIDENCE_PUBLISHER_BOOT_STALE"},
    {ReasonCode::EvidenceSubjectModelGenerationMismatch, "EVIDENCE_SUBJECT_MODEL_GENERATION_MISMATCH"},
    {ReasonCode::EvidenceSubjectArtifactMismatch, "EVIDENCE_SUBJECT_ARTIFACT_MISMATCH"},
    {ReasonCode::EvidenceSubjectCompatibilityMismatch, "EVIDENCE_SUBJECT_COMPATIBILITY_MISMATCH"},
    {ReasonCode::EvidenceUnsatisfiedCriteria, "EVIDENCE_UNSATISFIED_CRITERIA"},
    {ReasonCode::EvidenceConflicting, "EVIDENCE_CONFLICTING"},
    {ReasonCode::EvidenceUnknownProvenance, "EVIDENCE_UNKNOWN_PROVENANCE"},
    {ReasonCode::ModelGenerationMismatch, "MODEL_GENERATION_MISMATCH"},
    {ReasonCode::ScopeGenerationMismatch, "SCOPE_GENERATION_MISMATCH"},
    {ReasonCode::RolloutGenerationMismatch, "ROLLOUT_GENERATION_MISMATCH"},
    {ReasonCode::RolloutStageGenerationMismatch, "ROLLOUT_STAGE_GENERATION_MISMATCH"},
    {ReasonCode::PromotionGenerationMismatch, "PROMOTION_GENERATION_MISMATCH"},
    {ReasonCode::CoordinatorEpochStale, "COORDINATOR_EPOCH_STALE"},
    {ReasonCode::SnapshotGenerationStale, "SNAPSHOT_GENERATION_STALE"},
    {ReasonCode::SequenceRegression, "SEQUENCE_REGRESSION"},
    {ReasonCode::PromotionNotAuthorized, "PROMOTION_NOT_AUTHORIZED"},
    {ReasonCode::PromotionCommitDuplicate, "PROMOTION_COMMIT_DUPLICATE"},
    {ReasonCode::PromotionCandidateRetired, "PROMOTION_CANDIDATE_RETIRED"},
    {ReasonCode::PromotionScopeInvalid, "PROMOTION_SCOPE_INVALID"},
    {ReasonCode::PromotionRequiresRolloutPlan, "PROMOTION_REQUIRES_ROLLOUT_PLAN"},
    {ReasonCode::RolloutPlanStale, "ROLLOUT_PLAN_STALE"},
    {ReasonCode::RolloutNotActive, "ROLLOUT_NOT_ACTIVE"},
    {ReasonCode::RolloutAlreadyComplete, "ROLLOUT_ALREADY_COMPLETE"},
    {ReasonCode::StageOrderViolation, "STAGE_ORDER_VIOLATION"},
    {ReasonCode::StageEntryConditionUnmet, "STAGE_ENTRY_CONDITION_UNMET"},
    {ReasonCode::StageAcceptanceNotMet, "STAGE_ACCEPTANCE_NOT_MET"},
    {ReasonCode::StageFailureThresholdExceeded, "STAGE_FAILURE_THRESHOLD_EXCEEDED"},
    {ReasonCode::StageSkipped, "STAGE_SKIPPED"},
    {ReasonCode::CohortUnknown, "COHORT_UNKNOWN"},
    {ReasonCode::CohortScopeInvalid, "COHORT_SCOPE_INVALID"},
    {ReasonCode::StageCompletionDuplicate, "STAGE_COMPLETION_DUPLICATE"},
    {ReasonCode::RolloutSuperseded, "ROLLOUT_SUPERSEDED"},
    {ReasonCode::StageFeedbackIncomplete, "STAGE_FEEDBACK_INCOMPLETE"},
    {ReasonCode::DrainRequired, "DRAIN_REQUIRED"},
    {ReasonCode::RollbackTargetRequired, "ROLLBACK_TARGET_REQUIRED"},
    {ReasonCode::RollbackTargetUnknown, "ROLLBACK_TARGET_UNKNOWN"},
    {ReasonCode::RollbackTargetRetired, "ROLLBACK_TARGET_RETIRED"},
    {ReasonCode::RollbackTargetArtifactMissing, "ROLLBACK_TARGET_ARTIFACT_MISSING"},
    {ReasonCode::RollbackTargetIncompatible, "ROLLBACK_TARGET_INCOMPATIBLE"},
    {ReasonCode::RollbackCommitDuplicate, "ROLLBACK_COMMIT_DUPLICATE"},
    {ReasonCode::RollbackGenerationMismatch, "ROLLBACK_GENERATION_MISMATCH"},
    {ReasonCode::NoCurrentAuthority, "NO_CURRENT_AUTHORITY"},
    {ReasonCode::RollbackNotAuthorized, "ROLLBACK_NOT_AUTHORIZED"},
    {ReasonCode::RollbackSupersededByNewerGeneration, "ROLLBACK_SUPERSEDED_BY_NEWER_GENERATION"},
    {ReasonCode::RetirementScopeDependency, "RETIREMENT_SCOPE_DEPENDENCY"},
    {ReasonCode::RetirementRolloutActive, "RETIREMENT_ROLLOUT_ACTIVE"},
    {ReasonCode::RetirementRetentionUnsatisfied, "RETIREMENT_RETENTION_UNSATISFIED"},
    {ReasonCode::RetirementDrainIncomplete, "RETIREMENT_DRAIN_INCOMPLETE"},
    {ReasonCode::RetirementDependencyUnresolved, "RETIREMENT_DEPENDENCY_UNRESOLVED"},
    {ReasonCode::RetirementAlreadyRetired, "RETIREMENT_ALREADY_RETIRED"},
    {ReasonCode::ResidencyIntentMissing, "RESIDENCY_INTENT_MISSING"},
    {ReasonCode::ResidencyGenerationMismatch, "RESIDENCY_GENERATION_MISMATCH"},
    {ReasonCode::ResidencyNotResident, "RESIDENCY_NOT_RESIDENT"},
    {ReasonCode::ReadinessMissing, "READINESS_MISSING"},
    {ReasonCode::ReadinessStale, "READINESS_STALE"},
    {ReasonCode::ReadinessSubjectMismatch, "READINESS_SUBJECT_MISMATCH"},
    {ReasonCode::WarmupIncomplete, "WARMUP_INCOMPLETE"},
    {ReasonCode::WorkerUnknown, "WORKER_UNKNOWN"},
    {ReasonCode::WorkerBootStale, "WORKER_BOOT_STALE"},
    {ReasonCode::WorkerNotRegistered, "WORKER_NOT_REGISTERED"},
    {ReasonCode::WorkerNotCurrent, "WORKER_NOT_CURRENT"},
    {ReasonCode::WorkerScopeNotOwned, "WORKER_SCOPE_NOT_OWNED"},
    {ReasonCode::CoordinatorEpochMismatch, "COORDINATOR_EPOCH_MISMATCH"},
    {ReasonCode::AttemptUnknown, "ATTEMPT_UNKNOWN"},
    {ReasonCode::AttemptAlreadyCompleted, "ATTEMPT_ALREADY_COMPLETED"},
    {ReasonCode::AttemptConflictingCompletion, "ATTEMPT_CONFLICTING_COMPLETION"},
    {ReasonCode::AttemptOutcomeUnknown, "ATTEMPT_OUTCOME_UNKNOWN"},
    {ReasonCode::ReplicaEvidenceMissing, "REPLICA_EVIDENCE_MISSING"},
    {ReasonCode::ExternalStateDiverged, "EXTERNAL_STATE_DIVERGED"},
    {ReasonCode::ExpectedResidentMissing, "EXPECTED_RESIDENT_MISSING"},
    {ReasonCode::RetiredModelStillResident, "RETIRED_MODEL_STILL_RESIDENT"},
    {ReasonCode::RolloutStageWithoutWorkers, "ROLLOUT_STAGE_WITHOUT_WORKERS"},
    {ReasonCode::UnreconciledAttempt, "UNRECONCILED_ATTEMPT"},
    {ReasonCode::ProtocolWrongMagic, "PROTOCOL_WRONG_MAGIC"},
    {ReasonCode::ProtocolUnsupportedVersion, "PROTOCOL_UNSUPPORTED_VERSION"},
    {ReasonCode::ProtocolUnknownType, "PROTOCOL_UNKNOWN_TYPE"},
    {ReasonCode::ProtocolOversized, "PROTOCOL_OVERSIZED"},
    {ReasonCode::ProtocolTruncated, "PROTOCOL_TRUNCATED"},
    {ReasonCode::ProtocolIntegrityFailure, "PROTOCOL_INTEGRITY_FAILURE"},
    {ReasonCode::ProtocolDuplicateHello, "PROTOCOL_DUPLICATE_HELLO"},
    {ReasonCode::ProtocolInvalidScope, "PROTOCOL_INVALID_SCOPE"},
    {ReasonCode::ProtocolInvalidTransition, "PROTOCOL_INVALID_TRANSITION"},
    {ReasonCode::ProtocolDecodeFailure, "PROTOCOL_DECODE_FAILURE"},
    {ReasonCode::TransactionRolledBack, "TRANSACTION_ROLLED_BACK"},
    {ReasonCode::UnsupportedCapability, "UNSUPPORTED_CAPABILITY"},
};

static_assert(sizeof(kReasonNames) / sizeof(kReasonNames[0]) > 100,
              "reason name table must stay exhaustive");

}  // namespace

std::string_view to_string(ReasonCode code) noexcept {
  for (const ReasonName& entry : kReasonNames) {
    if (entry.code == code) return entry.name;
  }
  return "UNKNOWN_REASON";
}

std::string Decision::render() const {
  std::string out;
  out.reserve(64 + reasons_.size() * 48);
  out.append(to_string(outcome_));
  if (reasons_.empty()) return out;
  out.append(" [");
  bool first = true;
  for (const Reason& r : reasons_) {
    if (!first) out.append(", ");
    first = false;
    out.append(to_string(r.code));
    if (!r.detail.empty()) {
      out.push_back('(');
      out.append(r.detail);
      out.push_back(')');
    }
  }
  out.push_back(']');
  return out;
}

std::string Explanation::render() const {
  std::string out;
  out.reserve(128 + bindings_.size() * 40 + reasons_.size() * 48);
  out.append(to_string(outcome_));
  out.push_back('\n');
  for (const auto& kv : bindings_) {
    out.append("  ").append(kv.first).append(" = ").append(kv.second).push_back('\n');
  }
  if (!reasons_.empty()) {
    out.append("  reasons:\n");
    for (const Reason& r : reasons_) {
      out.append("    - ").append(to_string(r.code));
      if (!r.detail.empty()) out.append(": ").append(r.detail);
      out.push_back('\n');
    }
  }
  return out;
}

}  // namespace mlf
