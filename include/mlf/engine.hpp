// Model Lifecycle Fabric — the lifecycle engine.
//
// Owns the single systems question: which model version is allowed to become
// authoritative where, under what compatibility, artifact, policy, health,
// rollout, residency, and generation evidence, and how that authority evolves
// safely through promotion, canarying, rollout, rollback, drain, retirement and
// replacement.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "mlf/artifact.hpp"
#include "mlf/authority.hpp"
#include "mlf/compatibility.hpp"
#include "mlf/evidence.hpp"
#include "mlf/identity.hpp"
#include "mlf/lifecycle.hpp"
#include "mlf/model_record.hpp"
#include "mlf/policy.hpp"
#include "mlf/provenance.hpp"
#include "mlf/rollout.hpp"
#include "mlf/scope.hpp"
#include "mlf/status.hpp"

namespace mlf {

/// Every bounded resource in the runtime. Nothing grows without limit.
struct RegistryBounds {
  std::size_t max_models{4096};
  std::size_t max_versions_per_model{256};
  std::size_t max_scopes{4096};
  std::size_t max_rollouts{1024};
  std::size_t max_stages_per_rollout{32};
  std::size_t max_cohorts_per_rollout{64};
  std::size_t max_workers{256};
  std::size_t max_evidence{65536};
  std::size_t max_attempts{8192};
  std::size_t max_history_per_rollout{4096};
  std::size_t max_authority_bindings{16384};
  std::size_t max_payload_bytes{262144};
  std::size_t max_name_length{128};
  std::size_t max_digest_length{128};
  std::size_t max_query_results{1024};
  std::size_t max_connections{64};
  std::size_t max_requeued_frames{64};
};

/// External side effect the lifecycle runtime may request. Requests are attempts
/// with explicit identity and completion state; a request sent is never a
/// completed transition.
enum class AttemptKind : std::uint8_t {
  Warm = 0,
  Activate,
  Drain,
  Rollback,
  Rehydrate,
  StartCanaryReplicas,
  PromoteReplicaGroup,
  EvictRetired,
};

inline constexpr std::size_t kAttemptKindCount = 8;

[[nodiscard]] std::string_view to_string(AttemptKind kind) noexcept;
[[nodiscard]] bool parse_attempt_kind(std::string_view text, AttemptKind& out) noexcept;

/// Lifecycle of one external attempt.
enum class AttemptState : std::uint8_t {
  /// Issued; no completion observed.
  Dispatched = 0,
  /// Completion observed with success.
  Completed,
  /// Completion observed with failure.
  Failed,
  /// The performer died or the acknowledgement was lost: whether the side effect
  /// happened is unknown, so the runtime refuses to repeat it blindly.
  OutcomeUnknown,
  /// An operator or coordinator must reconcile before anything else happens.
  ReconcileRequired,
  /// Explicitly given up after reconciliation.
  Abandoned,
};

[[nodiscard]] std::string_view to_string(AttemptState state) noexcept;

/// One external side-effect attempt. Idempotent attempts may be safely
/// re-issued after an unknown outcome; non-idempotent attempts may not.
struct Attempt {
  AttemptId id{};
  AttemptKind kind{AttemptKind::Warm};
  WorkerId worker{};
  WorkerBootId boot{};
  CoordinatorEpoch epoch{};
  ModelId model{};
  ModelVersionId version{};
  ModelGeneration model_generation{};
  ArtifactGeneration artifact_generation{};
  ScopeId scope{};
  ScopeGeneration scope_generation{};
  RolloutId rollout{};
  RolloutGeneration rollout_generation{};
  RolloutStageId stage{};
  RolloutStageGeneration stage_generation{};
  ResidencyGeneration residency_generation{};
  AttemptState state{AttemptState::Dispatched};
  /// Whether repeating this side effect after an unknown outcome is safe.
  bool idempotent{false};
  Sequence issued_sequence{};
  Sequence settled_sequence{};
  std::string detail{};
};

/// Durable lease for one worker boot.
struct WorkerLease {
  WorkerId id{};
  WorkerBootId boot{};
  CoordinatorEpoch epoch{};
  std::vector<ScopeId> scopes{};
  bool fenced{false};
  Sequence registered_sequence{};
  Sequence fenced_sequence{};
  std::string detail{};
};

/// Live connection state of a worker. Volatile: never restored as current.
struct WorkerLiveness {
  WorkerId id{};
  bool connected{false};
};

// ---------------------------------------------------------------------------
// Requests
// ---------------------------------------------------------------------------

struct RegisterVersionRequest {
  ModelId model{};
  std::string label{};
  ArtifactBinding artifact{};
  ModelRequirements requirements{};
  ModelVersionId predecessor{};
  Provenance provenance{Provenance::Unknown};
};

struct ReviseVersionRequest {
  ModelId model{};
  ModelVersionId version{};
  /// Must equal the version's current ModelGeneration.
  ModelGeneration expected_generation{};
  ArtifactBinding artifact{};
  ModelRequirements requirements{};
  Provenance provenance{Provenance::Unknown};
};

struct PromotionRequest {
  ModelId model{};
  ModelVersionId candidate{};
  ModelGeneration candidate_generation{};
  ArtifactGeneration artifact_generation{};
  CompatibilityGeneration compatibility_generation{};
  PolicyGeneration policy_generation{};
  CoordinatorEpoch epoch{};
  ScopeId scope{};
  ScopeGeneration scope_generation{};
  RolloutStrategy strategy{RolloutStrategy::Canary};
  ModelVersionId rollback_target{};
  ModelGeneration rollback_target_generation{};
  EvidenceGeneration evidence_generation{};
  WorkerId worker{};
  WorkerBootId boot{};
  bool administrative_approval{false};
  /// Caller explicitly asks for immediate cutover rather than a staged rollout.
  bool request_immediate_cutover{false};
  /// Environment whose compatibility governs the promotion. When the engine
  /// holds a published fact for this key it is used; otherwise the optional
  /// structural profile below is evaluated directly.
  std::string environment_key{};
  std::optional<EnvironmentProfile> environment{};
};

struct CohortSpec {
  std::string name{};
  std::vector<ScopeId> scopes{};
  std::uint32_t traffic_percent{0};
  std::uint32_t max_blast_radius_percent{0};
};

struct StageSpec {
  std::string name{};
  std::vector<std::size_t> cohort_indices{};
  std::vector<EvidenceRequirement> required_evidence{};
  std::vector<Criterion> acceptance{};
  std::vector<Criterion> failure{};
  bool require_residency_before_entry{false};
  bool require_drain_after_commit{false};
};

struct RolloutPlanRequest {
  ModelId model{};
  ModelVersionId candidate{};
  ModelGeneration candidate_generation{};
  ArtifactGeneration artifact_generation{};
  CompatibilityGeneration compatibility_generation{};
  PolicyGeneration policy_generation{};
  ScopeId root_scope{};
  ScopeGeneration root_scope_generation{};
  RolloutStrategy strategy{RolloutStrategy::Canary};
  /// Generation the plan replaces. When omitted the engine derives it from the
  /// exclusive authority currently visible under the root scope.
  ModelVersionId previous{};
  ModelGeneration previous_generation{};
  ModelVersionId rollback_target{};
  ModelGeneration rollback_target_generation{};
  std::uint32_t max_blast_radius_percent{100};
  bool manual_progression{false};
  std::vector<CohortSpec> cohorts{};
  std::vector<StageSpec> stages{};
  CoordinatorEpoch epoch{};
  WorkerId worker{};
  WorkerBootId boot{};
  /// Promotion that authorizes this plan. Must match the candidate version's
  /// recorded promotion.
  PromotionId promotion{};
  PromotionGeneration promotion_generation{};
};

struct RolloutProgressRequest {
  RolloutId rollout{};
  RolloutGeneration rollout_generation{};
  RolloutStageId stage{};
  RolloutStageGeneration stage_generation{};
  ScopeGeneration scope_generation{};
  CoordinatorEpoch epoch{};
  WorkerId worker{};
  WorkerBootId boot{};
  bool administrative_approval{false};
  std::string detail{};
};

struct RollbackRequest {
  ModelId model{};
  ModelVersionId candidate{};
  ModelGeneration candidate_generation{};
  RolloutId rollout{};
  RolloutGeneration rollout_generation{};
  ModelVersionId target{};
  ModelGeneration target_generation{};
  ArtifactGeneration target_artifact_generation{};
  ScopeId scope{};
  ScopeGeneration scope_generation{};
  PolicyGeneration policy_generation{};
  CoordinatorEpoch epoch{};
  WorkerId worker{};
  WorkerBootId boot{};
  /// Automatic rollback commits only when policy explicitly authorizes it.
  bool automatic{false};
  std::string detail{};
};

struct RetirementRequest {
  ModelId model{};
  ModelVersionId version{};
  ModelGeneration generation{};
  PolicyGeneration policy_generation{};
  CoordinatorEpoch epoch{};
};

struct RevalidationRequest {
  ModelId model{};
  ModelVersionId version{};
  ModelGeneration generation{};
  ReasonCode cause{ReasonCode::CompatibilityStale};
  std::string detail{};
};

struct WorkerRegistration {
  WorkerId id{};
  WorkerBootId boot{};
  CoordinatorEpoch epoch{};
  std::vector<ScopeId> scopes{};
  Provenance provenance{Provenance::Unknown};
};

struct AttemptRequest {
  AttemptKind kind{AttemptKind::Warm};
  WorkerId worker{};
  WorkerBootId boot{};
  CoordinatorEpoch epoch{};
  ModelId model{};
  ModelVersionId version{};
  ModelGeneration model_generation{};
  ArtifactGeneration artifact_generation{};
  ScopeId scope{};
  RolloutId rollout{};
  RolloutGeneration rollout_generation{};
  RolloutStageId stage{};
  RolloutStageGeneration stage_generation{};
  bool idempotent{false};
  std::string detail{};
};

struct CompletionRecord {
  AttemptId attempt{};
  WorkerId worker{};
  WorkerBootId boot{};
  CoordinatorEpoch epoch{};
  bool success{false};
  /// Whether the external effect is confirmed to have happened. A worker that
  /// reports success without confirmation produces OUTCOME_UNKNOWN.
  bool confirmed{false};
  std::string detail{};
};

/// Observation of a serving replica, supplied by the replica fabric.
struct ReplicaObservation {
  ReplicaId replica{};
  ReplicaGeneration generation{};
  ModelId model{};
  ModelVersionId version{};
  ModelGeneration model_generation{};
  ArtifactGeneration artifact_generation{};
  ScopeId scope{};
  bool resident{false};
  bool ready{false};
  WorkerId worker{};
  WorkerBootId boot{};
  Provenance provenance{Provenance::Unknown};
};

/// Observation of residency for a model generation in a scope.
struct ResidencyObservation {
  ModelId model{};
  ModelVersionId version{};
  ModelGeneration model_generation{};
  ArtifactGeneration artifact_generation{};
  ScopeId scope{};
  ResidencyGeneration generation{};
  bool resident{false};
  bool ready{false};
  ReplicaId replica{};
  ReplicaGeneration replica_generation{};
  WorkerBootId boot{};
  Provenance provenance{Provenance::Unknown};
};

enum class FindingKind : std::uint8_t {
  Clean = 0,
  ExpectedResidentMissing,
  RetiredModelStillResident,
  RolloutStageWithoutWorkers,
  AuthorityWithoutReplicas,
  ReplicaForNonAuthoritativeGeneration,
  ArtifactGenerationChanged,
  CompatibilityChanged,
  UnreconciledAttempt,
  FencedWorkerStillPublishing,
  StalePlanBinding,
};

inline constexpr std::size_t kFindingKindCount = 11;

[[nodiscard]] std::string_view to_string(FindingKind kind) noexcept;

/// One deterministic reconciliation finding.
struct Finding {
  FindingKind kind{FindingKind::Clean};
  ReasonCode reason{ReasonCode::None};
  ScopeId scope{};
  ModelId model{};
  ModelVersionId version{};
  ModelGeneration model_generation{};
  std::string detail{};
};

struct ReconciliationInput {
  std::vector<ReplicaObservation> replicas{};
  std::vector<ResidencyObservation> residency{};
  std::vector<WorkerId> live_workers{};
  /// When false the runtime must not conclude that a worker died; it reports the
  /// gap instead.
  bool worker_liveness_known{true};
};

// ---------------------------------------------------------------------------
// Results
// ---------------------------------------------------------------------------

struct PromotionOutcome {
  Decision decision{};
  PromotionId promotion{};
  PromotionGeneration promotion_generation{};
  RolloutId rollout{};
  RolloutGeneration rollout_generation{};
  bool immediate{false};
};

struct RollbackOutcome {
  Decision decision{};
  RollbackId rollback{};
  RollbackGeneration rollback_generation{};
  std::vector<ScopeId> restored_scopes{};
};

struct AttemptOutcome {
  Decision decision{};
  AttemptId attempt{};
  bool reconciliation_required{false};
};

/// Bounded queue telemetry so that loss is visible rather than silent.
struct StoreStats {
  std::size_t models{0};
  std::size_t versions{0};
  std::size_t scopes{0};
  std::size_t rollouts{0};
  std::size_t evidence{0};
  std::uint64_t evidence_dropped{0};
  std::size_t attempts{0};
  std::size_t workers{0};
  std::size_t authority_bindings{0};
  std::size_t compatibility_facts{0};
  std::size_t committed_promotions{0};
  std::size_t committed_rollbacks{0};
  std::size_t requeued_frames{0};
};

struct EngineConfig {
  RegistryBounds bounds{};
  LifecyclePolicy policy{};
  Provenance default_provenance{Provenance::Synthetic};
};

// Forward declaration: the durable view lives in state_store.hpp.
struct DurableState;

/// The lifecycle engine.
///
/// Thread-safe. A single internal mutex guards all lifecycle state, and no
/// socket I/O, filesystem work, callback, or external call is ever made while it
/// is held. See docs/LOCK_ORDER.md.
class LifecycleEngine {
 public:
  explicit LifecycleEngine(EngineConfig config = {});
  ~LifecycleEngine();

  LifecycleEngine(const LifecycleEngine&) = delete;
  LifecycleEngine& operator=(const LifecycleEngine&) = delete;

  // --- registry -----------------------------------------------------------
  Decision register_model(std::string_view name, std::string_view family, Provenance provenance,
                          ModelId& out);
  Decision register_version(const RegisterVersionRequest& request, ModelVersionId& out);
  Decision revise_version(const ReviseVersionRequest& request);
  Decision register_scope(std::string_view path, ScopeId& out);
  Decision set_policy(LifecyclePolicy policy);
  Decision set_scope_policy(const ScopePolicyOverride& override_policy);

  [[nodiscard]] std::optional<ModelId> find_model_by_name(std::string_view name) const;
  [[nodiscard]] std::optional<ModelId> find_model_by_id(std::uint64_t raw) const;
  [[nodiscard]] std::optional<ModelVersionId> find_version_by_label(ModelId model,
                                                                  std::string_view label) const;
  [[nodiscard]] std::optional<ScopeId> find_scope_by_path(std::string_view path) const;

  [[nodiscard]] std::vector<ModelRecord> models() const;
  [[nodiscard]] std::vector<ModelVersionRecord> versions(ModelId model) const;
  [[nodiscard]] std::vector<ModelVersionRecord> all_versions() const;
  [[nodiscard]] std::optional<ModelVersionRecord> version(ModelId model,
                                                          ModelVersionId id) const;
  [[nodiscard]] std::optional<ModelRecord> model(ModelId id) const;
  [[nodiscard]] std::vector<ScopeRecord> scopes() const;
  [[nodiscard]] std::vector<RolloutPlan> rollouts() const;
  [[nodiscard]] std::optional<RolloutPlan> rollout(RolloutId id) const;
  [[nodiscard]] std::vector<Attempt> attempts() const;
  [[nodiscard]] std::vector<WorkerLease> workers() const;
  [[nodiscard]] std::vector<CompatibilityFact> compatibility_facts(ModelVersionId version) const;
  [[nodiscard]] std::vector<EvidenceRecord> evidence_for(ModelVersionId version) const;
  /// Every evidence record currently held, ordered by generation.
  [[nodiscard]] std::vector<EvidenceRecord> all_evidence() const;

  [[nodiscard]] const RegistryBounds& bounds() const noexcept { return config_.bounds; }
  [[nodiscard]] LifecyclePolicy current_policy() const;
  [[nodiscard]] LifecyclePolicy effective_policy(ScopeId scope) const;
  [[nodiscard]] PolicyGeneration policy_generation() const;

  // --- compatibility ------------------------------------------------------
  Decision publish_compatibility(ModelVersionId version, const EnvironmentProfile& environment,
                                 CompatibilityOutcome outcome, std::string_view detail,
                                 Provenance provenance);
  /// Evaluate declared requirements structurally, without published facts.
  [[nodiscard]] CompatibilityResult evaluate_requirements(ModelVersionId version,
                                                          const EnvironmentProfile& environment) const;
  /// Resolve the compatibility currently governing a version in an environment:
  /// a current published fact when one exists, otherwise nothing.
  [[nodiscard]] CompatibilityResult resolve_compatibility(ModelVersionId version,
                                                          const EnvironmentProfile& environment,
                                                          CompatibilityGeneration required) const;

  // --- evidence -----------------------------------------------------------
  /// Publish evidence. The engine assigns identity and generation.
  Decision publish_evidence(EvidenceRecord& record);
  [[nodiscard]] EvidenceLookup check_evidence(const EvidenceRequirement& requirement,
                                              const EvidenceSubject& subject) const;
  std::size_t expire_publisher_evidence(WorkerBootId boot);

  // --- workers ------------------------------------------------------------
  Decision register_worker(const WorkerRegistration& registration);
  Decision fence_worker(WorkerId id, WorkerBootId boot, CoordinatorEpoch epoch,
                        std::string_view reason);
  Decision set_worker_liveness(WorkerId id, bool connected);
  [[nodiscard]] std::optional<WorkerLease> worker(WorkerId id) const;
  /// True when the boot is the current, unfenced boot for its worker id.
  [[nodiscard]] bool worker_boot_current(WorkerId id, WorkerBootId boot) const;

  // --- promotion ----------------------------------------------------------
  [[nodiscard]] Decision evaluate_promotion(const PromotionRequest& request,
                                            Explanation* explanation = nullptr) const;
  PromotionOutcome promote(const PromotionRequest& request);

  // --- rollout ------------------------------------------------------------
  Decision create_rollout(const RolloutPlanRequest& request, RolloutId& out,
                          RolloutGeneration& generation);
  Decision begin_rollout(const RolloutProgressRequest& request);
  Decision advance_stage(const RolloutProgressRequest& request);
  Decision fail_stage(const RolloutProgressRequest& request);
  Decision supersede_rollout(RolloutId rollout, RolloutGeneration generation,
                             CoordinatorEpoch epoch, std::string_view detail);
  [[nodiscard]] Decision evaluate_stage_advance(const RolloutProgressRequest& request,
                                                Explanation* explanation = nullptr) const;

  // --- rollback -----------------------------------------------------------
  [[nodiscard]] Decision evaluate_rollback(const RollbackRequest& request,
                                           Explanation* explanation = nullptr) const;
  RollbackOutcome rollback(const RollbackRequest& request);

  // --- retirement / revalidation -----------------------------------------
  [[nodiscard]] Decision evaluate_retirement(const RetirementRequest& request,
                                             Explanation* explanation = nullptr) const;
  Decision retire(const RetirementRequest& request);
  Decision revalidate(const RevalidationRequest& request);

  // --- authority ----------------------------------------------------------
  [[nodiscard]] AuthorityQueryResult query_authority(ScopeId scope) const;
  [[nodiscard]] std::vector<AuthorityBinding> authority_bindings(ModelId model,
                                                                 ModelVersionId version) const;
  [[nodiscard]] std::vector<AuthorityBinding> all_authority_bindings() const;

  // --- external attempts --------------------------------------------------
  AttemptOutcome begin_attempt(const AttemptRequest& request);
  Decision complete_attempt(const CompletionRecord& completion);
  AttemptOutcome reconcile_attempt(AttemptId attempt, bool side_effect_observed,
                                   CoordinatorEpoch epoch, std::string_view detail);
  [[nodiscard]] std::optional<Attempt> attempt(AttemptId id) const;

  // --- reconciliation -----------------------------------------------------
  [[nodiscard]] std::vector<Finding> reconcile(const ReconciliationInput& input) const;

  // --- epoch, sequence, snapshot -----------------------------------------
  [[nodiscard]] CoordinatorEpoch epoch() const;
  [[nodiscard]] Sequence sequence() const;
  [[nodiscard]] SnapshotGeneration snapshot_generation() const;
  [[nodiscard]] StoreStats stats() const;

  /// Advance the coordinator epoch. Only ever moves forward.
  Decision advance_epoch(CoordinatorEpoch expected_current, CoordinatorEpoch& out);

  /// Copy the durable state out of the engine. Volatile health, readiness,
  /// worker liveness and in-flight attempts are deliberately excluded.
  [[nodiscard]] std::shared_ptr<const DurableState> export_durable_state() const;

  /// Replace engine state from a durable view. Fully validated before anything
  /// is applied: a rejected load leaves the engine exactly as it was.
  Decision import_durable_state(const DurableState& state, CoordinatorEpoch new_epoch);

  /// Release volatile execution state (worker liveness, in-flight attempts,
  /// dynamic evidence) while preserving durable lifecycle truth. Called after a
  /// coordinator restart.
  void invalidate_volatile_state(std::string_view cause);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  EngineConfig config_{};
  mutable std::mutex mutex_{};

  // Unlocked evaluation cores. Each public evaluate_* entry point locks the
  // engine mutex and delegates here; mutation paths call them while already
  // holding the lock so that validation and commit observe one consistent state.
  [[nodiscard]] Decision evaluate_promotion_locked(const PromotionRequest& request,
                                                   Explanation* explanation) const;
  [[nodiscard]] Decision evaluate_stage_advance_locked(const RolloutProgressRequest& request,
                                                       Explanation* explanation) const;
  [[nodiscard]] Decision evaluate_rollback_locked(const RollbackRequest& request,
                                                  Explanation* explanation) const;
  [[nodiscard]] Decision evaluate_retirement_locked(const RetirementRequest& request,
                                                    Explanation* explanation) const;
  /// Resolve the environment profile a compatibility decision should use.
  [[nodiscard]] std::optional<EnvironmentProfile> promotion_environment(
      const PromotionRequest& request) const;
};

}  // namespace mlf
