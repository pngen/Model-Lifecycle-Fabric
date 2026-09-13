// Model Lifecycle Fabric — durable state, canonical serialization, atomic storage.
//
// Only semantically durable lifecycle state is persisted. Volatile health,
// readiness, publisher liveness, replica liveness and in-flight attempt progress
// are deliberately excluded: after a restart they must be re-established, never
// restored as current.
#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "mlf/engine.hpp"
#include "mlf/version.hpp"

namespace mlf {

/// Complete durable view of the lifecycle runtime.
struct DurableState {
  std::uint16_t format_version{kStateFormatVersion};
  SnapshotGeneration generation{};
  CoordinatorEpoch epoch{};
  Sequence sequence{Sequence(kFirstGeneration)};

  std::map<std::uint64_t, ModelRecord> models{};
  std::map<std::pair<std::uint64_t, std::uint64_t>, ModelVersionRecord> versions{};

  std::vector<ScopeRecord> scopes{};
  ScopeId root_scope{};
  std::map<std::uint64_t, ScopePolicyOverride> policy_overrides{};
  LifecyclePolicy base_policy{};

  std::map<std::pair<std::uint64_t, std::string>, CompatibilityFact> compatibility{};

  std::map<std::uint64_t, RolloutPlan> rollouts{};
  std::map<std::uint64_t, std::vector<AuthorityBinding>> authority{};

  /// Worker leases survive only as fenced history: a lease whose worker is gone
  /// is persisted as fenced so that recovery cannot resurrect its authority.
  std::map<std::uint64_t, WorkerLease> worker_leases{};

  /// Durable evidence summaries: artifact, compatibility, policy approval and
  /// test-result records only. Dynamic health, latency, error-rate and readiness
  /// records are not durable.
  std::vector<EvidenceRecord> evidence{};

  /// Attempts that never settled. Recovery keeps them conservative.
  std::map<std::uint64_t, Attempt> unsettled_attempts{};

  std::set<std::uint64_t> committed_promotions{};
  std::set<std::pair<std::uint64_t, std::uint64_t>> committed_rollbacks{};

  // Monotonic identity allocators.
  std::uint64_t next_model_id{1};
  std::uint64_t next_version_id{1};
  std::uint64_t next_rollout_id{1};
  std::uint64_t next_stage_id{1};
  std::uint64_t next_cohort_id{1};
  std::uint64_t next_promotion_id{1};
  std::uint64_t next_rollback_id{1};
  std::uint64_t next_attempt_id{1};
  std::uint64_t next_worker_id{1};
  std::uint64_t next_evidence_id{1};
  std::uint64_t next_evidence_generation{1};
  std::uint64_t next_compatibility_generation{1};
};

/// Outcome of a persistence operation.
enum class StateStoreStatus : std::uint8_t {
  Ok = 0,
  IoError,
  BadMagic,
  UnsupportedVersion,
  IntegrityFailure,
  Truncated,
  BoundsExceeded,
  InvalidContent,
  AtomicReplaceFailed,
};

[[nodiscard]] std::string_view to_string(StateStoreStatus status) noexcept;

struct StateStoreResult {
  StateStoreStatus status{StateStoreStatus::Ok};
  std::string detail{};
  [[nodiscard]] bool ok() const noexcept { return status == StateStoreStatus::Ok; }
};

/// Limits applied while decoding. A file that claims more than the runtime is
/// willing to hold is rejected before anything is allocated.
struct DecodeLimits {
  std::size_t max_models{4096};
  std::size_t max_versions{1u << 20};
  std::size_t max_scopes{4096};
  std::size_t max_rollouts{1024};
  std::size_t max_authority_bindings{16384};
  std::size_t max_workers{256};
  std::size_t max_evidence{65536};
  std::size_t max_attempts{8192};
  std::size_t max_compatibility_facts{8192};
  std::size_t max_stages_per_rollout{32};
  std::size_t max_cohorts_per_rollout{64};
  std::size_t max_history_per_rollout{4096};
  std::size_t max_string{256};
  std::size_t max_digest{128};
  std::size_t max_payload_bytes{16u * 1024u * 1024u};

  [[nodiscard]] static DecodeLimits from_bounds(const RegistryBounds& bounds);
};

/// Encode a durable view into its canonical byte representation.
[[nodiscard]] StateStoreResult serialize_durable_state(const DurableState& state,
                                                       const DecodeLimits& limits,
                                                       std::vector<std::uint8_t>& out);

/// Decode a durable view. Fully validated: on failure the output is untouched.
[[nodiscard]] StateStoreResult deserialize_durable_state(const std::uint8_t* data, std::size_t size,
                                                         const DecodeLimits& limits,
                                                         DurableState& out);

/// Write atomically: a temporary file in the same directory, fully written and
/// flushed, then renamed over the destination.
[[nodiscard]] StateStoreResult save_durable_state(const DurableState& state,
                                                  const DecodeLimits& limits,
                                                  const std::string& path);

/// Read and decode. A partial temporary file, a truncated header, a wrong magic,
/// an unsupported version or a corrupt integrity check all fail without applying
/// anything.
[[nodiscard]] StateStoreResult load_durable_state(const std::string& path, const DecodeLimits& limits,
                                                  DurableState& out);

/// CRC-32 (IEEE 802.3) used as the frame and file integrity check.
[[nodiscard]] std::uint32_t crc32(const void* data, std::size_t size) noexcept;

}  // namespace mlf
