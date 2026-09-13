// Model Lifecycle Fabric — deterministic synthetic lifecycle backend.
//
// Models replicas, residency, warmup, activation and drain with explicit
// generations. It is the production interface used by lifecycle workers; there
// is no test-only shortcut path. Everything it produces is labelled SYNTHETIC
// and never claims to describe physical infrastructure.
#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "mlf/engine.hpp"
#include "mlf/identity.hpp"
#include "mlf/protocol.hpp"
#include "mlf/provenance.hpp"
#include "mlf/scope.hpp"
#include "mlf/status.hpp"

namespace mlf {

/// One synthetic replica.
struct SyntheticReplica {
  ReplicaId id{};
  ReplicaGeneration generation{ReplicaGeneration(kFirstGeneration)};
  ScopeId scope{};
  ModelId model{};
  ModelVersionId version{};
  ModelGeneration model_generation{};
  ArtifactGeneration artifact_generation{};
  bool warm{false};
  bool resident{false};
  bool ready{false};
  bool healthy{true};
  bool draining{false};
  ResidencyGeneration residency_generation{ResidencyGeneration(kFirstGeneration)};
  WorkerId owner{};
  WorkerBootId boot{};
  Provenance provenance{Provenance::Synthetic};
};

/// Result of executing one external command against the backend.
struct CommandResult {
  Decision decision{};
  /// True when the backend actually applied the effect. A caller that never
  /// receives this result must treat the effect as possibly applied.
  bool effect_applied{false};
  std::string detail{};
};

/// Deterministic synthetic lifecycle backend.
class SyntheticLifecycleBackend {
 public:
  SyntheticLifecycleBackend() = default;

  void set_owner(WorkerId worker, WorkerBootId boot, CoordinatorEpoch epoch) {
    std::lock_guard<std::mutex> guard(mutex_);
    owner_ = worker;
    boot_ = boot;
    epoch_ = epoch;
  }
  void clear_owner() {
    std::lock_guard<std::mutex> guard(mutex_);
    owner_ = WorkerId{};
    boot_ = WorkerBootId{};
  }
  [[nodiscard]] WorkerId owner() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return owner_;
  }
  [[nodiscard]] WorkerBootId boot() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return boot_;
  }

  /// Create a replica in a scope. Replicas start cold: present but neither
  /// resident nor ready.
  Decision add_replica(ScopeId scope, ModelId model, ModelVersionId version,
                       ModelGeneration model_generation, ArtifactGeneration artifact_generation,
                       ReplicaId& out);

  Decision remove_replica(ReplicaId id);

  /// Simulate an unexpected replica loss: the replica disappears without a
  /// drain, exactly as a crashed pod would.
  Decision lose_replica(ReplicaId id);

  Decision set_healthy(ReplicaId id, bool healthy);
  Decision warm(ReplicaId id);
  Decision activate(ReplicaId id);
  Decision drain(ReplicaId id);
  Decision evict(ReplicaId id);

  /// Remove every replica serving a model generation in a scope. Used to model
  /// an external eviction that the lifecycle runtime must reconcile against.
  std::size_t lose_generation_in_scope(ModelId model, ModelVersionId version,
                                       ModelGeneration generation, ScopeId scope);

  [[nodiscard]] std::vector<ReplicaObservation> replicas() const;
  [[nodiscard]] std::vector<ResidencyObservation> residency() const;
  [[nodiscard]] std::vector<SyntheticReplica> raw_replicas() const;
  [[nodiscard]] std::size_t replica_count() const;
  [[nodiscard]] std::optional<SyntheticReplica> replica(ReplicaId id) const;

  /// Execute a lifecycle command issued by the coordinator.
  CommandResult execute(const CommandMessage& command);

  /// When enabled the backend still applies the effect but reports it as
  /// unconfirmed, exercising the ambiguous-completion path without any timing
  /// dependence.
  void set_ambiguous_completion(bool enabled) {
    std::lock_guard<std::mutex> guard(mutex_);
    ambiguous_completion_ = enabled;
  }
  [[nodiscard]] bool ambiguous_completion() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return ambiguous_completion_;
  }

  void set_max_replicas(std::size_t value) {
    std::lock_guard<std::mutex> guard(mutex_);
    max_replicas_ = value;
  }

  /// Deterministic evidence tick. The backend never reads a wall clock.
  [[nodiscard]] std::uint64_t tick() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return tick_;
  }
  void set_tick(std::uint64_t value) {
    std::lock_guard<std::mutex> guard(mutex_);
    tick_ = value;
  }

 private:
  [[nodiscard]] SyntheticReplica* find_locked(ReplicaId id);

  mutable std::mutex mutex_{};
  std::map<std::uint64_t, SyntheticReplica> replicas_{};
  std::uint64_t next_replica_id_{1};
  std::size_t max_replicas_{4096};
  bool ambiguous_completion_{false};
  std::uint64_t tick_{0};
  WorkerId owner_{};
  WorkerBootId boot_{};
  CoordinatorEpoch epoch_{};
};

}  // namespace mlf
