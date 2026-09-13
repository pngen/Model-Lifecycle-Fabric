#include "mlf/synthetic_backend.hpp"

#include <algorithm>

namespace mlf {

Decision SyntheticLifecycleBackend::add_replica(ScopeId scope, ModelId model,
                                                ModelVersionId version,
                                                ModelGeneration model_generation,
                                                ArtifactGeneration artifact_generation,
                                                ReplicaId& out) {
  std::lock_guard<std::mutex> guard(mutex_);
  out = ReplicaId{};
  Decision decision(OutcomeCode::Accepted);
  if (!scope.valid() || !model.valid() || !version.valid()) {
    decision.set_outcome(OutcomeCode::Rejected);
    return decision.add(ReasonCode::UnknownScope, "replica needs scope, model and version");
  }
  if (replicas_.size() >= max_replicas_) {
    decision.set_outcome(OutcomeCode::BOUNDS_EXCEEDED);
    return decision.add(ReasonCode::BoundsModelCount, "synthetic replica bound reached");
  }
  SyntheticReplica replica;
  replica.id = ReplicaId(next_replica_id_++);
  replica.scope = scope;
  replica.model = model;
  replica.version = version;
  replica.model_generation = model_generation;
  replica.artifact_generation = artifact_generation;
  replica.owner = owner_;
  replica.boot = boot_;
  out = replica.id;
  replicas_.emplace(replica.id.raw(), std::move(replica));
  return decision;
}

SyntheticReplica* SyntheticLifecycleBackend::find_locked(ReplicaId id) {
  const auto found = replicas_.find(id.raw());
  return found == replicas_.end() ? nullptr : &found->second;
}

Decision SyntheticLifecycleBackend::remove_replica(ReplicaId id) {
  std::lock_guard<std::mutex> guard(mutex_);
  Decision decision(OutcomeCode::Accepted);
  const auto found = replicas_.find(id.raw());
  if (found == replicas_.end()) {
    decision.set_outcome(OutcomeCode::NOT_FOUND);
    return decision.add(ReasonCode::UnknownVersion, "synthetic replica is not registered");
  }
  replicas_.erase(found);
  return decision;
}

Decision SyntheticLifecycleBackend::lose_replica(ReplicaId id) { return remove_replica(id); }

Decision SyntheticLifecycleBackend::set_healthy(ReplicaId id, bool healthy) {
  std::lock_guard<std::mutex> guard(mutex_);
  Decision decision(OutcomeCode::Accepted);
  SyntheticReplica* replica = find_locked(id);
  if (replica == nullptr) {
    decision.set_outcome(OutcomeCode::NOT_FOUND);
    return decision.add(ReasonCode::UnknownVersion, "synthetic replica is not registered");
  }
  replica->healthy = healthy;
  if (!healthy) replica->ready = false;
  ++tick_;
  return decision;
}

Decision SyntheticLifecycleBackend::warm(ReplicaId id) {
  std::lock_guard<std::mutex> guard(mutex_);
  Decision decision(OutcomeCode::Accepted);
  SyntheticReplica* replica = find_locked(id);
  if (replica == nullptr) {
    decision.set_outcome(OutcomeCode::NOT_FOUND);
    return decision.add(ReasonCode::UnknownVersion, "synthetic replica is not registered");
  }
  if (!replica->healthy) {
    decision.set_outcome(OutcomeCode::Rejected);
    return decision.add(ReasonCode::WarmupIncomplete, "replica is not healthy");
  }
  replica->warm = true;
  replica->resident = true;
  replica->residency_generation = next_generation(replica->residency_generation);
  ++tick_;
  return decision;
}

Decision SyntheticLifecycleBackend::activate(ReplicaId id) {
  std::lock_guard<std::mutex> guard(mutex_);
  Decision decision(OutcomeCode::Accepted);
  SyntheticReplica* replica = find_locked(id);
  if (replica == nullptr) {
    decision.set_outcome(OutcomeCode::NOT_FOUND);
    return decision.add(ReasonCode::UnknownVersion, "synthetic replica is not registered");
  }
  if (!replica->warm) {
    decision.set_outcome(OutcomeCode::Rejected);
    return decision.add(ReasonCode::ReadinessMissing,
                        "a cold replica cannot become ready; warm it first");
  }
  replica->resident = true;
  replica->ready = true;
  replica->draining = false;
  replica->generation = next_generation(replica->generation);
  replica->residency_generation = next_generation(replica->residency_generation);
  ++tick_;
  return decision;
}

Decision SyntheticLifecycleBackend::drain(ReplicaId id) {
  std::lock_guard<std::mutex> guard(mutex_);
  Decision decision(OutcomeCode::Accepted);
  SyntheticReplica* replica = find_locked(id);
  if (replica == nullptr) {
    decision.set_outcome(OutcomeCode::NOT_FOUND);
    return decision.add(ReasonCode::UnknownVersion, "synthetic replica is not registered");
  }
  replica->ready = false;
  replica->draining = true;
  replica->generation = next_generation(replica->generation);
  ++tick_;
  return decision;
}

Decision SyntheticLifecycleBackend::evict(ReplicaId id) {
  std::lock_guard<std::mutex> guard(mutex_);
  Decision decision(OutcomeCode::Accepted);
  SyntheticReplica* replica = find_locked(id);
  if (replica == nullptr) {
    decision.set_outcome(OutcomeCode::NOT_FOUND);
    return decision.add(ReasonCode::UnknownVersion, "synthetic replica is not registered");
  }
  replicas_.erase(replica->id.raw());
  ++tick_;
  return decision;
}

std::size_t SyntheticLifecycleBackend::lose_generation_in_scope(ModelId model,
                                                                ModelVersionId version,
                                                                ModelGeneration generation,
                                                                ScopeId scope) {
  std::lock_guard<std::mutex> guard(mutex_);
  std::size_t removed = 0;
  for (auto it = replicas_.begin(); it != replicas_.end();) {
    const SyntheticReplica& replica = it->second;
    if (replica.model == model && replica.version == version &&
        replica.model_generation == generation && replica.scope == scope) {
      it = replicas_.erase(it);
      ++removed;
    } else {
      ++it;
    }
  }
  ++tick_;
  return removed;
}

std::vector<ReplicaObservation> SyntheticLifecycleBackend::replicas() const {
  std::lock_guard<std::mutex> guard(mutex_);
  std::vector<ReplicaObservation> out;
  out.reserve(replicas_.size());
  for (const auto& entry : replicas_) {
    const SyntheticReplica& replica = entry.second;
    ReplicaObservation observation;
    observation.replica = replica.id;
    observation.generation = replica.generation;
    observation.model = replica.model;
    observation.version = replica.version;
    observation.model_generation = replica.model_generation;
    observation.artifact_generation = replica.artifact_generation;
    observation.scope = replica.scope;
    observation.resident = replica.resident;
    observation.ready = replica.ready;
    observation.worker = replica.owner;
    observation.boot = replica.boot;
    observation.provenance = replica.provenance;
    out.push_back(observation);
  }
  return out;
}

std::vector<ResidencyObservation> SyntheticLifecycleBackend::residency() const {
  std::lock_guard<std::mutex> guard(mutex_);
  std::vector<ResidencyObservation> out;
  out.reserve(replicas_.size());
  for (const auto& entry : replicas_) {
    const SyntheticReplica& replica = entry.second;
    ResidencyObservation observation;
    observation.model = replica.model;
    observation.version = replica.version;
    observation.model_generation = replica.model_generation;
    observation.artifact_generation = replica.artifact_generation;
    observation.scope = replica.scope;
    observation.generation = replica.residency_generation;
    observation.resident = replica.resident && !replica.draining;
    observation.ready = replica.ready && replica.healthy;
    observation.replica = replica.id;
    observation.replica_generation = replica.generation;
    observation.boot = replica.boot;
    observation.provenance = replica.provenance;
    out.push_back(observation);
  }
  return out;
}

std::vector<SyntheticReplica> SyntheticLifecycleBackend::raw_replicas() const {
  std::lock_guard<std::mutex> guard(mutex_);
  std::vector<SyntheticReplica> out;
  out.reserve(replicas_.size());
  for (const auto& entry : replicas_) out.push_back(entry.second);
  return out;
}

std::size_t SyntheticLifecycleBackend::replica_count() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return replicas_.size();
}

std::optional<SyntheticReplica> SyntheticLifecycleBackend::replica(ReplicaId id) const {
  std::lock_guard<std::mutex> guard(mutex_);
  const auto found = replicas_.find(id.raw());
  if (found == replicas_.end()) return std::nullopt;
  return found->second;
}

CommandResult SyntheticLifecycleBackend::execute(const CommandMessage& command) {
  CommandResult result;
  switch (command.kind) {
    case AttemptKind::Warm: {
      // A warm request without an existing replica materialises one so that a
      // coordinator can drive first-time warmup through the same command path.
      std::lock_guard<std::mutex> guard(mutex_);
      SyntheticReplica* target = nullptr;
      for (auto& entry : replicas_) {
        SyntheticReplica& replica = entry.second;
        if (replica.model != command.model || replica.version != command.version) continue;
        if (replica.scope != command.scope) continue;
        if (replica.model_generation != command.model_generation) continue;
        if (replica.artifact_generation != command.artifact_generation) continue;
        target = &replica;
        break;
      }
      if (target == nullptr) {
        if (replicas_.size() >= max_replicas_) {
          result.decision.set_outcome(OutcomeCode::BOUNDS_EXCEEDED);
          result.decision.add(ReasonCode::BoundsModelCount, "replica bound reached");
          return result;
        }
        SyntheticReplica replica;
        replica.id = ReplicaId(next_replica_id_++);
        replica.scope = command.scope;
        replica.model = command.model;
        replica.version = command.version;
        replica.model_generation = command.model_generation;
        replica.artifact_generation = command.artifact_generation;
        replica.owner = owner_;
        replica.boot = boot_;
        target = &replicas_.emplace(replica.id.raw(), replica).first->second;
      }
      if (!target->healthy) {
        result.decision.set_outcome(OutcomeCode::Rejected);
        result.decision.add(ReasonCode::WarmupIncomplete, "replica is unhealthy");
        result.effect_applied = false;
        return result;
      }
      target->warm = true;
      target->resident = true;
      target->residency_generation = next_generation(target->residency_generation);
      ++tick_;
      result.effect_applied = true;
      result.detail = "replica warmed and resident";
      break;
    }
    case AttemptKind::Activate:
    case AttemptKind::PromoteReplicaGroup: {
      std::lock_guard<std::mutex> guard(mutex_);
      bool touched = false;
      for (auto& entry : replicas_) {
        SyntheticReplica& replica = entry.second;
        if (replica.model != command.model || replica.version != command.version) continue;
        if (replica.scope != command.scope) continue;
        if (replica.model_generation != command.model_generation) continue;
        if (!replica.healthy) continue;
        replica.warm = true;
        replica.resident = true;
        replica.ready = true;
        replica.draining = false;
        replica.generation = next_generation(replica.generation);
        replica.residency_generation = next_generation(replica.residency_generation);
        touched = true;
      }
      ++tick_;
      result.effect_applied = touched;
      result.detail = touched ? "replicas activated" : "no matching replica was activatable";
      break;
    }
    case AttemptKind::Drain: {
      std::lock_guard<std::mutex> guard(mutex_);
      bool touched = false;
      for (auto& entry : replicas_) {
        SyntheticReplica& replica = entry.second;
        if (replica.model != command.model || replica.version != command.version) continue;
        if (replica.scope != command.scope) continue;
        if (replica.model_generation != command.model_generation) continue;
        replica.ready = false;
        replica.draining = true;
        replica.generation = next_generation(replica.generation);
        touched = true;
      }
      ++tick_;
      result.effect_applied = touched;
      result.detail = touched ? "replicas drained" : "no matching replica needed draining";
      break;
    }
    case AttemptKind::Rollback:
    case AttemptKind::Rehydrate: {
      std::lock_guard<std::mutex> guard(mutex_);
      bool touched = false;
      for (auto& entry : replicas_) {
        SyntheticReplica& replica = entry.second;
        if (replica.model != command.model || replica.version != command.version) continue;
        if (replica.scope != command.scope) continue;
        if (replica.model_generation != command.model_generation) continue;
        if (!replica.healthy) continue;
        replica.warm = true;
        replica.resident = true;
        replica.ready = true;
        replica.draining = false;
        replica.generation = next_generation(replica.generation);
        replica.residency_generation = next_generation(replica.residency_generation);
        touched = true;
      }
      ++tick_;
      result.effect_applied = touched;
      result.detail = touched ? "previous generation restored" : "no replica to restore";
      break;
    }
    case AttemptKind::StartCanaryReplicas: {
      std::lock_guard<std::mutex> guard(mutex_);
      if (replicas_.size() >= max_replicas_) {
        result.decision.set_outcome(OutcomeCode::BOUNDS_EXCEEDED);
        result.decision.add(ReasonCode::BoundsModelCount, "replica bound reached");
        return result;
      }
      SyntheticReplica replica;
      replica.id = ReplicaId(next_replica_id_++);
      replica.scope = command.scope;
      replica.model = command.model;
      replica.version = command.version;
      replica.model_generation = command.model_generation;
      replica.artifact_generation = command.artifact_generation;
      replica.warm = true;
      replica.resident = true;
      replica.ready = true;
      replica.owner = owner_;
      replica.boot = boot_;
      replicas_.emplace(replica.id.raw(), std::move(replica));
      ++tick_;
      result.effect_applied = true;
      result.detail = "canary replica started";
      break;
    }
    case AttemptKind::EvictRetired: {
      std::lock_guard<std::mutex> guard(mutex_);
      std::size_t removed = 0;
      for (auto it = replicas_.begin(); it != replicas_.end();) {
        const SyntheticReplica& replica = it->second;
        if (replica.model == command.model && replica.version == command.version &&
            replica.model_generation == command.model_generation) {
          it = replicas_.erase(it);
          ++removed;
        } else {
          ++it;
        }
      }
      ++tick_;
      result.effect_applied = removed > 0;
      result.detail = "evicted " + std::to_string(removed) + " retired replicas";
      break;
    }
    default: {
      result.decision.set_outcome(OutcomeCode::Rejected);
      result.decision.add(ReasonCode::ProtocolUnknownType, "unsupported command kind");
      return result;
    }
  }
  if (ambiguous_completion_) {
    // The effect happened but the performer will not confirm it. The caller must
    // treat the outcome as unknown.
    result.detail += " (completion deliberately left unconfirmed)";
  }
  return result;
}

}  // namespace mlf
