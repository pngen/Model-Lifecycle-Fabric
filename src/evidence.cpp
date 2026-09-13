#include "mlf/evidence.hpp"

#include <algorithm>

namespace mlf {
namespace {

struct KindName {
  EvidenceKind kind;
  std::string_view name;
};

constexpr KindName kKindNames[] = {
    {EvidenceKind::ArtifactIntegrity, "ARTIFACT_INTEGRITY"},
    {EvidenceKind::RuntimeCompatibility, "RUNTIME_COMPATIBILITY"},
    {EvidenceKind::BackendCompatibility, "BACKEND_COMPATIBILITY"},
    {EvidenceKind::HardwareCapability, "HARDWARE_CAPABILITY"},
    {EvidenceKind::TokenizerMatch, "TOKENIZER_MATCH"},
    {EvidenceKind::AdapterCompatibility, "ADAPTER_COMPATIBILITY"},
    {EvidenceKind::Warmup, "WARMUP"},
    {EvidenceKind::Readiness, "READINESS"},
    {EvidenceKind::ReplicaReady, "REPLICA_READY"},
    {EvidenceKind::ResidencyReady, "RESIDENCY_READY"},
    {EvidenceKind::HealthCheck, "HEALTH_CHECK"},
    {EvidenceKind::ErrorRate, "ERROR_RATE"},
    {EvidenceKind::Latency, "LATENCY"},
    {EvidenceKind::Throughput, "THROUGHPUT"},
    {EvidenceKind::Correctness, "CORRECTNESS"},
    {EvidenceKind::VerificationOutcome, "VERIFICATION_OUTCOME"},
    {EvidenceKind::ResourcePressure, "RESOURCE_PRESSURE"},
    {EvidenceKind::CrashRate, "CRASH_RATE"},
    {EvidenceKind::TestResult, "TEST_RESULT"},
    {EvidenceKind::PolicyApproval, "POLICY_APPROVAL"},
    {EvidenceKind::ManualApproval, "MANUAL_APPROVAL"},
    {EvidenceKind::Signature, "SIGNATURE"},
};

static_assert(sizeof(kKindNames) / sizeof(kKindNames[0]) == kEvidenceKindCount,
              "evidence kind table must be exhaustive");

struct CurrentnessName {
  EvidenceCurrentness value;
  std::string_view name;
};

constexpr CurrentnessName kCurrentnessNames[] = {
    {EvidenceCurrentness::Current, "CURRENT"},
    {EvidenceCurrentness::Missing, "MISSING"},
    {EvidenceCurrentness::SubjectMismatch, "SUBJECT_MISMATCH"},
    {EvidenceCurrentness::GenerationStale, "GENERATION_STALE"},
    {EvidenceCurrentness::PublisherStale, "PUBLISHER_STALE"},
    {EvidenceCurrentness::Expired, "EXPIRED"},
    {EvidenceCurrentness::Conflicting, "CONFLICTING"},
    {EvidenceCurrentness::UnknownProvenance, "UNKNOWN_PROVENANCE"},
};

}  // namespace

std::string_view to_string(EvidenceKind kind) noexcept {
  for (const KindName& entry : kKindNames) {
    if (entry.kind == kind) return entry.name;
  }
  return "UNKNOWN_KIND";
}

bool parse_evidence_kind(std::string_view text, EvidenceKind& out) noexcept {
  for (const KindName& entry : kKindNames) {
    if (entry.name == text) {
      out = entry.kind;
      return true;
    }
  }
  return false;
}

std::string_view to_string(EvidenceVerdict verdict) noexcept {
  switch (verdict) {
    case EvidenceVerdict::Satisfied:   return "SATISFIED";
    case EvidenceVerdict::Unsatisfied: return "UNSATISFIED";
    case EvidenceVerdict::Unknown:     break;
  }
  return "UNKNOWN";
}

std::string_view to_string(EvidenceCurrentness currentness) noexcept {
  for (const CurrentnessName& entry : kCurrentnessNames) {
    if (entry.value == currentness) return entry.name;
  }
  return "UNKNOWN";
}

std::vector<std::uint64_t> EvidenceSubject::key_fields() const noexcept {
  return {model.raw(),
          version.raw(),
          model_generation.raw(),
          artifact_set.raw(),
          artifact_generation.raw(),
          scope.raw(),
          rollout.raw(),
          rollout_generation.raw(),
          stage.raw(),
          stage_generation.raw(),
          compatibility_generation.raw()};
}

std::string EvidenceSubject::key() const {
  std::string out;
  out.reserve(128);
  const std::vector<std::uint64_t> fields = key_fields();
  for (std::size_t i = 0; i < fields.size(); ++i) {
    if (i != 0) out.push_back(':');
    out.append(std::to_string(fields[i]));
  }
  return out;
}

EvidenceStore::Key EvidenceStore::make_key(const EvidenceSubject& subject, EvidenceKind kind) {
  std::string bucket;
  bucket.reserve(48);
  bucket.append(std::to_string(subject.model.raw()));
  bucket.push_back(':');
  bucket.append(std::to_string(subject.version.raw()));
  return {std::move(bucket), static_cast<std::uint16_t>(kind)};
}

Decision EvidenceStore::publish(const EvidenceRecord& record) {
  Decision decision(OutcomeCode::Accepted);
  const auto kind_index = static_cast<std::size_t>(record.kind);
  if (kind_index >= kEvidenceKindCount) {
    decision.set_outcome(OutcomeCode::Rejected);
    return decision.add(ReasonCode::ProtocolUnknownType, "evidence kind out of range");
  }
  if (!record.subject.version.valid()) {
    decision.set_outcome(OutcomeCode::Rejected);
    return decision.add(ReasonCode::UnknownVersion, "evidence subject has no model version");
  }
  if (!record.generation.valid()) {
    decision.set_outcome(OutcomeCode::Rejected);
    return decision.add(ReasonCode::EvidenceGenerationMismatch,
                        "evidence generation must be non-zero");
  }
  if (record.detail.size() > 256 || record.unit.size() > 32) {
    decision.set_outcome(OutcomeCode::BOUNDS_EXCEEDED);
    return decision.add(ReasonCode::BoundsStringLength, "evidence metadata oversized");
  }
  const Key key = make_key(record.subject, record.kind);
  const auto existing = records_.find(key);
  if (existing != records_.end() && !existing->second.empty()) {
    const EvidenceRecord& newest = existing->second.back();
    if (record.generation < newest.generation) {
      decision.set_outcome(OutcomeCode::CONFLICT);
      return decision.add(ReasonCode::EvidenceGenerationMismatch,
                          "evidence generation is older than the stored record");
    }
    if (record.generation == newest.generation) {
      if (record.verdict != newest.verdict) {
        decision.set_outcome(OutcomeCode::CONFLICT);
        return decision.add(ReasonCode::EvidenceConflicting,
                            "two verdicts published for the same evidence generation");
      }
      decision.set_outcome(OutcomeCode::CONFLICT);
      return decision.add(ReasonCode::EvidenceGenerationMismatch,
                          "duplicate evidence generation for the same subject");
    }
  }
  if (watermark_.valid() && record.generation <= watermark_) {
    decision.set_outcome(OutcomeCode::CONFLICT);
    return decision.add(ReasonCode::SequenceRegression,
                        "evidence generation does not advance the store watermark");
  }

  // Bounded growth: make room before inserting, and make the loss visible.
  while (record_count_ >= max_records_ && !eviction_order_.empty()) {
    const auto oldest = eviction_order_.begin();
    const std::uint64_t generation = oldest->first;
    const Key victim = oldest->second;
    eviction_order_.erase(oldest);
    const auto victim_it = records_.find(victim);
    if (victim_it != records_.end()) {
      auto& bucket = victim_it->second;
      const auto removed = std::remove_if(bucket.begin(), bucket.end(),
                                          [generation](const EvidenceRecord& candidate) {
                                            return candidate.generation.raw() == generation;
                                          });
      const std::size_t removed_count = static_cast<std::size_t>(bucket.end() - removed);
      bucket.erase(removed, bucket.end());
      record_count_ -= removed_count;
      dropped_ += removed_count;
      if (bucket.empty()) records_.erase(victim_it);
    }
  }

  records_[key].push_back(record);
  eviction_order_.emplace(record.generation.raw(), key);
  ++record_count_;
  watermark_ = record.generation;
  return decision;
}

namespace {

/// Classify why a stored record does not exactly match the evaluated subject.
EvidenceCurrentness classify_mismatch(const EvidenceSubject& stored,
                                       const EvidenceSubject& requested) {
  if (stored.model_generation != requested.model_generation) {
    return stored.model_generation < requested.model_generation ? EvidenceCurrentness::GenerationStale
                                                               : EvidenceCurrentness::SubjectMismatch;
  }
  if (stored.artifact_generation != requested.artifact_generation ||
      stored.artifact_set != requested.artifact_set) {
    return stored.artifact_generation < requested.artifact_generation
               ? EvidenceCurrentness::GenerationStale
               : EvidenceCurrentness::SubjectMismatch;
  }
  if (stored.scope != requested.scope) return EvidenceCurrentness::SubjectMismatch;
  if (stored.rollout != requested.rollout ||
      stored.rollout_generation != requested.rollout_generation) {
    return stored.rollout_generation < requested.rollout_generation
               ? EvidenceCurrentness::GenerationStale
               : EvidenceCurrentness::SubjectMismatch;
  }
  if (stored.stage != requested.stage || stored.stage_generation != requested.stage_generation) {
    return stored.stage_generation < requested.stage_generation
               ? EvidenceCurrentness::GenerationStale
               : EvidenceCurrentness::SubjectMismatch;
  }
  if (stored.compatibility_generation != requested.compatibility_generation) {
    return stored.compatibility_generation < requested.compatibility_generation
               ? EvidenceCurrentness::GenerationStale
               : EvidenceCurrentness::SubjectMismatch;
  }
  return EvidenceCurrentness::SubjectMismatch;
}

bool subjects_equal(const EvidenceSubject& a, const EvidenceSubject& b) {
  return a.model == b.model && a.version == b.version && a.model_generation == b.model_generation &&
         a.artifact_set == b.artifact_set && a.artifact_generation == b.artifact_generation &&
         a.scope == b.scope && a.rollout == b.rollout &&
         a.rollout_generation == b.rollout_generation && a.stage == b.stage &&
         a.stage_generation == b.stage_generation &&
         a.compatibility_generation == b.compatibility_generation;
}

}  // namespace

EvidenceLookup EvidenceStore::resolve(const EvidenceRequirement& requirement,
                                      const EvidenceSubject& subject, WorkerBootId publisher_boot,
                                      bool publisher_must_be_current, std::uint64_t now_tick,
                                      std::uint64_t max_age_ticks) const {
  EvidenceLookup lookup;
  if (subject.version.raw() == 0) {
    lookup.currentness = EvidenceCurrentness::Missing;
    return lookup;
  }
  const Key key = make_key(subject, requirement.kind);
  const auto found = records_.find(key);
  if (found == records_.end() || found->second.empty()) {
    lookup.currentness = EvidenceCurrentness::Missing;
    return lookup;
  }

  const std::vector<EvidenceRecord>& bucket = found->second;
  const EvidenceRecord* candidate = nullptr;
  EvidenceCurrentness fallback = EvidenceCurrentness::Missing;
  bool have_fallback = false;

  for (auto it = bucket.rbegin(); it != bucket.rend(); ++it) {
    const EvidenceRecord& record = *it;
    if (subjects_equal(record.subject, subject)) {
      candidate = &record;
      break;
    }
    if (!have_fallback) {
      if (!requirement.require_exact_subject) {
        candidate = &record;
        break;
      }
      fallback = classify_mismatch(record.subject, subject);
      have_fallback = true;
    }
  }

  if (candidate == nullptr) {
    lookup.currentness = have_fallback ? fallback : EvidenceCurrentness::Missing;
    return lookup;
  }

  // Conflicting publications: the same subject and generation with two verdicts.
  for (const EvidenceRecord& record : bucket) {
    if (&record == candidate) continue;
    if (record.generation == candidate->generation && subjects_equal(record.subject, subject) &&
        record.verdict != candidate->verdict) {
      lookup.currentness = EvidenceCurrentness::Conflicting;
      lookup.record = candidate;
      return lookup;
    }
  }

  if (candidate->provenance == Provenance::Unknown) {
    lookup.currentness = EvidenceCurrentness::UnknownProvenance;
    lookup.record = candidate;
    return lookup;
  }

  const bool needs_publisher =
      publisher_must_be_current || requirement.require_current_publisher;
  if (needs_publisher && candidate->boot.valid() && candidate->boot != publisher_boot) {
    lookup.currentness = EvidenceCurrentness::PublisherStale;
    lookup.record = candidate;
    return lookup;
  }

  if (max_age_ticks > 0 && now_tick > candidate->tick &&
      (now_tick - candidate->tick) > max_age_ticks) {
    lookup.currentness = EvidenceCurrentness::Expired;
    lookup.record = candidate;
    return lookup;
  }

  lookup.currentness = EvidenceCurrentness::Current;
  lookup.record = candidate;
  return lookup;
}

const EvidenceRecord* EvidenceStore::latest(const EvidenceSubject& subject,
                                            EvidenceKind kind) const {
  const auto found = records_.find(make_key(subject, kind));
  if (found == records_.end() || found->second.empty()) return nullptr;
  return &found->second.back();
}

std::vector<const EvidenceRecord*> EvidenceStore::history(const EvidenceSubject& subject) const {
  std::vector<const EvidenceRecord*> out;
  for (const auto& entry : records_) {
    if (entry.first.first != make_key(subject, EvidenceKind::HealthCheck).first) continue;
    for (const EvidenceRecord& record : entry.second) out.push_back(&record);
  }
  return out;
}

std::size_t EvidenceStore::invalidate_model_generation(ModelVersionId version,
                                                       ModelGeneration current) {
  std::size_t removed = 0;
  for (auto it = records_.begin(); it != records_.end();) {
    auto& bucket = it->second;
    const auto keep = std::remove_if(bucket.begin(), bucket.end(),
                                     [version, current](const EvidenceRecord& record) {
                                       return record.subject.version == version &&
                                              record.subject.model_generation < current;
                                     });
    const std::size_t removed_here = static_cast<std::size_t>(bucket.end() - keep);
    bucket.erase(keep, bucket.end());
    removed += removed_here;
    if (bucket.empty()) {
      it = records_.erase(it);
    } else {
      ++it;
    }
  }
  record_count_ -= removed;
  dropped_ += removed;
  for (auto it = eviction_order_.begin(); it != eviction_order_.end();) {
    if (records_.find(it->second) == records_.end()) {
      it = eviction_order_.erase(it);
    } else {
      ++it;
    }
  }
  return removed;
}

std::size_t EvidenceStore::invalidate_publisher_boot(WorkerBootId boot) {
  if (!boot.valid()) return 0;
  std::size_t removed = 0;
  for (auto it = records_.begin(); it != records_.end();) {
    auto& bucket = it->second;
    const auto keep = std::remove_if(bucket.begin(), bucket.end(),
                                     [boot](const EvidenceRecord& record) {
                                       return record.boot == boot;
                                     });
    const std::size_t removed_here = static_cast<std::size_t>(bucket.end() - keep);
    bucket.erase(keep, bucket.end());
    removed += removed_here;
    if (bucket.empty()) {
      it = records_.erase(it);
    } else {
      ++it;
    }
  }
  record_count_ -= removed;
  dropped_ += removed;
  for (auto it = eviction_order_.begin(); it != eviction_order_.end();) {
    if (records_.find(it->second) == records_.end()) {
      it = eviction_order_.erase(it);
    } else {
      ++it;
    }
  }
  return removed;
}

std::size_t EvidenceStore::remove_kinds(const std::vector<EvidenceKind>& kinds) {
  std::size_t removed = 0;
  for (auto it = records_.begin(); it != records_.end();) {
    const bool drop_bucket =
        std::find(kinds.begin(), kinds.end(), static_cast<EvidenceKind>(it->first.second)) !=
        kinds.end();
    auto& bucket = it->second;
    if (drop_bucket) {
      removed += bucket.size();
      it = records_.erase(it);
      continue;
    }
    ++it;
  }
  record_count_ -= removed;
  dropped_ += removed;
  for (auto it = eviction_order_.begin(); it != eviction_order_.end();) {
    if (records_.find(it->second) == records_.end()) {
      it = eviction_order_.erase(it);
    } else {
      ++it;
    }
  }
  return removed;
}

bool EvidenceStore::restore(std::vector<EvidenceRecord> records, std::uint64_t dropped) {
  std::map<Key, std::vector<EvidenceRecord>> rebuilt;
  std::map<std::uint64_t, Key> order;
  for (const EvidenceRecord& record : records) {
    if (static_cast<std::size_t>(record.kind) >= kEvidenceKindCount) return false;
    if (!record.subject.version.valid()) return false;
    if (!record.generation.valid()) return false;
    if (record.detail.size() > 256 || record.unit.size() > 32) return false;
    const Key key = make_key(record.subject, record.kind);
    rebuilt[key].push_back(record);
    if (!order.emplace(record.generation.raw(), key).second) {
      // Duplicate generations across subjects are legal; across the same key they
      // are retained deliberately so that resolve() can report a conflict.
      if (rebuilt[key].size() < 2) return false;
    }
  }
  if (records.size() > max_records_) return false;
  for (auto& entry : rebuilt) {
    std::sort(entry.second.begin(), entry.second.end(),
              [](const EvidenceRecord& a, const EvidenceRecord& b) {
                return a.generation < b.generation;
              });
  }
  records_ = std::move(rebuilt);
  eviction_order_ = std::move(order);
  record_count_ = records.size();
  dropped_ = dropped;
  watermark_ = EvidenceGeneration{};
  for (const auto& entry : records_) {
    if (!entry.second.empty() && entry.second.back().generation > watermark_) {
      watermark_ = entry.second.back().generation;
    }
  }
  return true;
}

}  // namespace mlf
