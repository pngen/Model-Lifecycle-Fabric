// Model Lifecycle Fabric — generation-bound evidence and freshness.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mlf/artifact.hpp"
#include "mlf/identity.hpp"
#include "mlf/provenance.hpp"
#include "mlf/status.hpp"

namespace mlf {

/// Kinds of evidence the lifecycle runtime consumes. It never generates fleet
/// evidence itself; it governs how current evidence influences authority.
enum class EvidenceKind : std::uint8_t {
  ArtifactIntegrity = 0,
  RuntimeCompatibility,
  BackendCompatibility,
  HardwareCapability,
  TokenizerMatch,
  AdapterCompatibility,
  Warmup,
  Readiness,
  ReplicaReady,
  ResidencyReady,
  HealthCheck,
  ErrorRate,
  Latency,
  Throughput,
  Correctness,
  VerificationOutcome,
  ResourcePressure,
  CrashRate,
  TestResult,
  PolicyApproval,
  ManualApproval,
  Signature,
};

inline constexpr std::size_t kEvidenceKindCount = 22;

[[nodiscard]] std::string_view to_string(EvidenceKind kind) noexcept;
[[nodiscard]] bool parse_evidence_kind(std::string_view text, EvidenceKind& out) noexcept;

/// Whether an observation satisfies, violates, or does not decide a requirement.
enum class EvidenceVerdict : std::uint8_t {
  Unknown = 0,
  Satisfied,
  Unsatisfied,
};

[[nodiscard]] std::string_view to_string(EvidenceVerdict verdict) noexcept;

/// The exact subject a piece of evidence is about. Evidence whose subject does
/// not match the generation under evaluation is stale, never silently reusable.
struct EvidenceSubject {
  ModelId model{};
  ModelVersionId version{};
  ModelGeneration model_generation{};
  ArtifactSetId artifact_set{};
  ArtifactGeneration artifact_generation{};
  ScopeId scope{};
  RolloutId rollout{};
  RolloutGeneration rollout_generation{};
  RolloutStageId stage{};
  RolloutStageGeneration stage_generation{};
  CompatibilityGeneration compatibility_generation{};

  /// Deterministic ordering key. Two subjects with the same key are the same
  /// subject for evidence purposes.
  [[nodiscard]] std::vector<std::uint64_t> key_fields() const noexcept;
  [[nodiscard]] std::string key() const;
};

/// One evidence record.
struct EvidenceRecord {
  EvidenceId id{};
  /// Monotonic per-store sequence. Strictly increasing; a record that does not
  /// advance the sequence is refused.
  EvidenceGeneration generation{};
  EvidenceKind kind{EvidenceKind::HealthCheck};
  EvidenceVerdict verdict{EvidenceVerdict::Unknown};
  EvidenceSubject subject{};
  Provenance provenance{Provenance::Unknown};
  WorkerId worker{};
  WorkerBootId boot{};
  CoordinatorEpoch epoch{};
  PolicyGeneration policy_generation{};
  double value{0.0};
  std::string unit{};
  std::uint64_t tick{0};
  std::string detail{};
};

/// Why a piece of evidence is, or is not, usable for a decision now.
enum class EvidenceCurrentness : std::uint8_t {
  Current = 0,
  Missing,
  SubjectMismatch,
  GenerationStale,
  PublisherStale,
  Expired,
  Conflicting,
  UnknownProvenance,
};

[[nodiscard]] std::string_view to_string(EvidenceCurrentness currentness) noexcept;

/// A requirement placed on evidence by a plan, a stage, or a policy.
struct EvidenceRequirement {
  EvidenceKind kind{EvidenceKind::HealthCheck};
  EvidenceVerdict required_verdict{EvidenceVerdict::Satisfied};
  /// When true, the record must be bound to exactly the evaluated subject.
  bool require_exact_subject{true};
  /// When true, the publisher's worker boot must still be current.
  bool require_current_publisher{false};
};

/// Result of resolving a requirement against the store.
struct EvidenceLookup {
  EvidenceCurrentness currentness{EvidenceCurrentness::Missing};
  const EvidenceRecord* record{nullptr};
  [[nodiscard]] bool usable() const noexcept {
    return currentness == EvidenceCurrentness::Current && record != nullptr;
  }
};

/// Bounded, deterministic evidence store.
///
/// Capacity is enforced by evicting the oldest record and incrementing an
/// explicit drop counter, so incomplete evidence stays visible instead of
/// silently disappearing.
class EvidenceStore {
 public:
  explicit EvidenceStore(std::size_t max_records = 65536) : max_records_(max_records) {}

  /// Publish a record. The store assigns nothing; the caller supplies the
  /// generation and it must strictly exceed the stored generation for the same
  /// (subject, kind).
  Decision publish(const EvidenceRecord& record);

  /// Resolve a requirement for a subject.
  [[nodiscard]] EvidenceLookup resolve(const EvidenceRequirement& requirement,
                                       const EvidenceSubject& subject, WorkerBootId publisher_boot,
                                       bool publisher_must_be_current,
                                       std::uint64_t now_tick,
                                       std::uint64_t max_age_ticks) const;

  /// Latest record for a (subject, kind) pair, ignoring freshness.
  [[nodiscard]] const EvidenceRecord* latest(const EvidenceSubject& subject,
                                             EvidenceKind kind) const;

  /// Every record for a subject, in publication order.
  [[nodiscard]] std::vector<const EvidenceRecord*> history(const EvidenceSubject& subject) const;

  [[nodiscard]] std::size_t size() const noexcept { return records_.size(); }
  [[nodiscard]] std::uint64_t dropped() const noexcept { return dropped_; }
  [[nodiscard]] std::size_t max_records() const noexcept { return max_records_; }
  void set_max_records(std::size_t value) noexcept { max_records_ = value; }

  /// Drop every record bound to a model generation older than the given one.
  /// Returns how many records were removed.
  std::size_t invalidate_model_generation(ModelVersionId version, ModelGeneration current);

  /// Forget every record published by a specific worker boot.
  std::size_t invalidate_publisher_boot(WorkerBootId boot);

  /// Forget every record whose kind is listed. Used to withdraw dynamic
  /// telemetry while preserving durable lifecycle evidence.
  std::size_t remove_kinds(const std::vector<EvidenceKind>& kinds);

  /// Replace the store contents during recovery. Dynamic health and readiness
  /// records are not durable; recovery passes only the records it intends to
  /// keep, and the engine passes none for volatile kinds.
  [[nodiscard]] bool restore(std::vector<EvidenceRecord> records, std::uint64_t dropped);

  /// Deterministic view of every bucket, keyed by (model:version, kind).
  using Key = std::pair<std::string, std::uint16_t>;
  [[nodiscard]] const std::map<Key, std::vector<EvidenceRecord>>& raw_buckets() const noexcept {
    return records_;
  }

 private:
  [[nodiscard]] static Key make_key(const EvidenceSubject& subject, EvidenceKind kind);

  std::map<Key, std::vector<EvidenceRecord>> records_{};
  std::map<std::uint64_t, Key> eviction_order_{};
  std::size_t record_count_{0};
  std::size_t max_records_{65536};
  std::uint64_t dropped_{0};
  EvidenceGeneration watermark_{};
};

}  // namespace mlf
