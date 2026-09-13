#include "mlf/compatibility.hpp"

#include <algorithm>
#include <cstdio>

namespace mlf {
namespace {

struct OutcomeName {
  CompatibilityOutcome outcome;
  std::string_view name;
};

// Order matches the enumeration so the table is auditable against the header.
constexpr OutcomeName kOutcomeNames[] = {
    {CompatibilityOutcome::COMPATIBLE, "COMPATIBLE"},
    {CompatibilityOutcome::COMPATIBLE_WITH_REBUILD, "COMPATIBLE_WITH_REBUILD"},
    {CompatibilityOutcome::COMPATIBLE_WITH_RECOMPILE, "COMPATIBLE_WITH_RECOMPILE"},
    {CompatibilityOutcome::COMPATIBLE_WITH_CONVERSION, "COMPATIBLE_WITH_CONVERSION"},
    {CompatibilityOutcome::INCOMPATIBLE_ARTIFACT, "INCOMPATIBLE_ARTIFACT"},
    {CompatibilityOutcome::INCOMPATIBLE_RUNTIME, "INCOMPATIBLE_RUNTIME"},
    {CompatibilityOutcome::INCOMPATIBLE_BACKEND, "INCOMPATIBLE_BACKEND"},
    {CompatibilityOutcome::INCOMPATIBLE_ARCHITECTURE, "INCOMPATIBLE_ARCHITECTURE"},
    {CompatibilityOutcome::INCOMPATIBLE_PRECISION, "INCOMPATIBLE_PRECISION"},
    {CompatibilityOutcome::INCOMPATIBLE_TOKENIZER, "INCOMPATIBLE_TOKENIZER"},
    {CompatibilityOutcome::INCOMPATIBLE_ADAPTER, "INCOMPATIBLE_ADAPTER"},
    {CompatibilityOutcome::INCOMPATIBLE_POLICY, "INCOMPATIBLE_POLICY"},
    {CompatibilityOutcome::UNKNOWN, "UNKNOWN"},
    {CompatibilityOutcome::STALE_EVIDENCE, "STALE_EVIDENCE"},
    {CompatibilityOutcome::UNSUPPORTED, "UNSUPPORTED"},
};

static_assert(sizeof(kOutcomeNames) / sizeof(kOutcomeNames[0]) == kCompatibilityOutcomeCount,
              "compatibility outcome table must be exhaustive");

}  // namespace

std::string_view to_string(CompatibilityOutcome outcome) noexcept {
  for (const OutcomeName& entry : kOutcomeNames) {
    if (entry.outcome == outcome) return entry.name;
  }
  return "UNKNOWN";
}

bool parse_compatibility_outcome(std::string_view text, CompatibilityOutcome& out) noexcept {
  for (const OutcomeName& entry : kOutcomeNames) {
    if (entry.name == text) {
      out = entry.outcome;
      return true;
    }
  }
  return false;
}

ReasonCode reason_for(CompatibilityOutcome outcome) noexcept {
  switch (outcome) {
    case CompatibilityOutcome::COMPATIBLE:                   return ReasonCode::None;
    case CompatibilityOutcome::COMPATIBLE_WITH_REBUILD:      return ReasonCode::CompatibilityRequiresRebuild;
    case CompatibilityOutcome::COMPATIBLE_WITH_RECOMPILE:    return ReasonCode::CompatibilityRequiresRecompile;
    case CompatibilityOutcome::COMPATIBLE_WITH_CONVERSION:   return ReasonCode::CompatibilityRequiresConversion;
    case CompatibilityOutcome::INCOMPATIBLE_ARTIFACT:        return ReasonCode::IncompatibleArtifact;
    case CompatibilityOutcome::INCOMPATIBLE_RUNTIME:         return ReasonCode::IncompatibleRuntime;
    case CompatibilityOutcome::INCOMPATIBLE_BACKEND:         return ReasonCode::IncompatibleBackend;
    case CompatibilityOutcome::INCOMPATIBLE_ARCHITECTURE:    return ReasonCode::IncompatibleArchitecture;
    case CompatibilityOutcome::INCOMPATIBLE_PRECISION:       return ReasonCode::IncompatiblePrecision;
    case CompatibilityOutcome::INCOMPATIBLE_TOKENIZER:       return ReasonCode::IncompatibleTokenizer;
    case CompatibilityOutcome::INCOMPATIBLE_ADAPTER:         return ReasonCode::IncompatibleAdapter;
    case CompatibilityOutcome::INCOMPATIBLE_POLICY:          return ReasonCode::IncompatiblePolicy;
    case CompatibilityOutcome::UNKNOWN:                      return ReasonCode::CompatibilityUnknown;
    case CompatibilityOutcome::STALE_EVIDENCE:               return ReasonCode::CompatibilityStale;
    case CompatibilityOutcome::UNSUPPORTED:                  return ReasonCode::UnsupportedCapability;
  }
  return ReasonCode::CompatibilityUnknown;
}

namespace {

/// Split a component into its leading decimal run and the remainder. A component
/// with no leading digits has an empty numeric part.
void split_component(std::string_view component, std::string_view& numeric,
                     std::string_view& remainder) noexcept {
  std::size_t digits = 0;
  while (digits < component.size() && component[digits] >= '0' && component[digits] <= '9') {
    ++digits;
  }
  numeric = component.substr(0, digits);
  remainder = component.substr(digits);
}

/// Numeric comparison of two digit runs without overflow.
int compare_numeric(std::string_view a, std::string_view b) noexcept {
  std::size_t za = 0;
  while (za + 1 < a.size() && a[za] == '0') ++za;
  std::size_t zb = 0;
  while (zb + 1 < b.size() && b[zb] == '0') ++zb;
  const std::string_view na = a.substr(za);
  const std::string_view nb = b.substr(zb);
  if (na.size() != nb.size()) return na.size() < nb.size() ? -1 : 1;
  if (na != nb) return na < nb ? -1 : 1;
  return 0;
}

}  // namespace

int compare_versions(std::string_view a, std::string_view b) noexcept {
  std::size_t i = 0;
  std::size_t j = 0;
  while (i < a.size() || j < b.size()) {
    std::size_t ai = i;
    while (ai < a.size() && a[ai] != '.') ++ai;
    std::size_t bj = j;
    while (bj < b.size() && b[bj] != '.') ++bj;
    const std::string_view pa = a.substr(i, ai - i);
    const std::string_view pb = b.substr(j, bj - j);

    std::string_view na;
    std::string_view ra;
    std::string_view nb;
    std::string_view rb;
    split_component(pa, na, ra);
    split_component(pb, nb, rb);

    if (!na.empty() && !nb.empty()) {
      const int numeric = compare_numeric(na, nb);
      if (numeric != 0) return numeric;
      // Equal numeric prefixes: a release sorts after its pre-release, so a
      // component with no remainder outranks one that carries "-rc1" or similar.
      if (ra != rb) {
        if (ra.empty()) return 1;
        if (rb.empty()) return -1;
        return ra < rb ? -1 : 1;
      }
    } else if (pa != pb) {
      return pa < pb ? -1 : 1;
    }

    const bool a_done = ai >= a.size();
    const bool b_done = bj >= b.size();
    if (a_done && b_done) return 0;
    if (a_done) return -1;
    if (b_done) return 1;
    i = ai + 1;
    j = bj + 1;
  }
  return 0;
}

std::string cuda_architecture_token(std::uint32_t major, std::uint32_t minor) {
  std::string out = "sm_";
  out.append(std::to_string(major));
  out.append(std::to_string(minor));
  return out;
}

CompatibilityResult evaluate_requirements(const ModelRequirements& requirements,
                                          const EnvironmentProfile& environment) {
  CompatibilityResult result;
  Decision& decision = result.decision;

  if (!environment.valid()) {
    result.outcome = CompatibilityOutcome::UNKNOWN;
    decision.set_outcome(OutcomeCode::UNSUPPORTED);
    decision.add(ReasonCode::CompatibilityUnknown, "environment profile missing or invalid");
    return result;
  }

  std::vector<std::pair<CompatibilityOutcome, Reason>> incompatibilities;
  std::vector<Reason> unknowns;

  const auto fail = [&incompatibilities](CompatibilityOutcome outcome, ReasonCode code,
                                         std::string detail) {
    incompatibilities.emplace_back(outcome, Reason(code, std::move(detail)));
  };
  const auto unknown = [&unknowns](ReasonCode code, std::string detail) {
    unknowns.emplace_back(code, std::move(detail));
  };

  if (requirements.empty()) {
    unknown(ReasonCode::CompatibilityUnknown,
            "model version declares no runtime, backend, architecture or precision requirement");
  }

  // --- runtime ------------------------------------------------------------
  if (!requirements.runtime.empty()) {
    if (environment.runtime.empty()) {
      unknown(ReasonCode::CompatibilityUnknown, "environment reports no runtime identity");
    } else if (environment.runtime != requirements.runtime) {
      fail(CompatibilityOutcome::INCOMPATIBLE_RUNTIME, ReasonCode::IncompatibleRuntime,
           "environment runtime '" + environment.runtime + "' != required '" +
               requirements.runtime + "'");
    } else if (!requirements.runtime_min_version.empty()) {
      if (environment.runtime_version.empty()) {
        unknown(ReasonCode::CompatibilityUnknown, "environment reports no runtime version");
      } else if (compare_versions(environment.runtime_version, requirements.runtime_min_version) <
                 0) {
        fail(CompatibilityOutcome::INCOMPATIBLE_RUNTIME, ReasonCode::IncompatibleRuntime,
             "runtime version " + environment.runtime_version + " < required " +
                 requirements.runtime_min_version);
      }
    }
  }

  // --- backend ------------------------------------------------------------
  if (!requirements.backend.empty()) {
    if (environment.backend.empty()) {
      unknown(ReasonCode::CompatibilityUnknown, "environment reports no backend identity");
    } else if (environment.backend != requirements.backend) {
      fail(CompatibilityOutcome::INCOMPATIBLE_BACKEND, ReasonCode::IncompatibleBackend,
           "environment backend '" + environment.backend + "' != required '" +
               requirements.backend + "'");
    }
  }

  // --- architecture -------------------------------------------------------
  if (!requirements.architecture.empty()) {
    std::vector<std::string> acceptable;
    if (!environment.architecture.empty()) acceptable.push_back(environment.architecture);
    if (environment.compute_capability != 0) {
      const std::uint32_t major = environment.compute_capability / 100;
      const std::uint32_t minor = environment.compute_capability % 100;
      acceptable.push_back(cuda_architecture_token(major, minor));
    }
    if (acceptable.empty()) {
      unknown(ReasonCode::CompatibilityUnknown,
              "environment reports no architecture or compute capability");
    } else if (std::find(acceptable.begin(), acceptable.end(), requirements.architecture) ==
               acceptable.end()) {
      fail(CompatibilityOutcome::INCOMPATIBLE_ARCHITECTURE, ReasonCode::IncompatibleArchitecture,
           "required architecture '" + requirements.architecture + "' not in environment set");
    }
  }

  // --- precision ----------------------------------------------------------
  if (!requirements.precision.empty()) {
    if (environment.precisions.empty()) {
      unknown(ReasonCode::CompatibilityUnknown, "environment reports no supported precisions");
    } else if (environment.precisions.find(requirements.precision) ==
               environment.precisions.end()) {
      fail(CompatibilityOutcome::INCOMPATIBLE_PRECISION, ReasonCode::IncompatiblePrecision,
           "required precision '" + requirements.precision + "' unsupported by environment");
    }
  }

  // --- tokenizer / config generation --------------------------------------
  if (!requirements.tokenizer_generation.empty()) {
    if (environment.tokenizer_generation.empty()) {
      unknown(ReasonCode::CompatibilityUnknown, "environment reports no tokenizer generation");
    } else if (environment.tokenizer_generation != requirements.tokenizer_generation) {
      fail(CompatibilityOutcome::INCOMPATIBLE_TOKENIZER, ReasonCode::IncompatibleTokenizer,
           "tokenizer generation mismatch");
    }
  }

  // --- adapters -----------------------------------------------------------
  if (!requirements.adapter_set.empty()) {
    if (environment.adapter_set.empty()) {
      unknown(ReasonCode::CompatibilityUnknown, "environment reports no adapter set");
    } else if (environment.adapter_set != requirements.adapter_set) {
      fail(CompatibilityOutcome::INCOMPATIBLE_ADAPTER, ReasonCode::IncompatibleAdapter,
           "adapter set mismatch");
    }
  }

  if (!incompatibilities.empty()) {
    result.outcome = incompatibilities.front().first;
    decision.set_outcome(OutcomeCode::INCOMPATIBLE);
    for (const auto& entry : incompatibilities) decision.add(entry.second);
    decision.normalize();
    return result;
  }

  if (!unknowns.empty()) {
    result.outcome = CompatibilityOutcome::UNKNOWN;
    decision.set_outcome(OutcomeCode::UNSUPPORTED);
    for (const Reason& reason : unknowns) decision.add(reason);
    decision.normalize();
    return result;
  }

  result.outcome = CompatibilityOutcome::COMPATIBLE;
  decision.set_outcome(OutcomeCode::Ok);
  return result;
}

Decision CompatibilityRegistry::publish(const CompatibilityFact& fact,
                                        CompatibilityGeneration assigned_generation) {
  Decision decision(OutcomeCode::Accepted);
  if (!fact.version.valid()) {
    decision.set_outcome(OutcomeCode::Rejected);
    return decision.add(ReasonCode::UnknownVersion, "compatibility fact has no model version");
  }
  if (fact.environment_key.empty() || fact.environment_key.size() > 128) {
    decision.set_outcome(OutcomeCode::Rejected);
    return decision.add(ReasonCode::InvalidName, "environment key must be 1..128 characters");
  }
  if (!assigned_generation.valid()) {
    decision.set_outcome(OutcomeCode::Rejected);
    return decision.add(ReasonCode::CompatibilityGenerationMismatch,
                        "assigned compatibility generation must be non-zero");
  }

  const auto key = std::make_pair(fact.version.raw(), fact.environment_key);
  const auto existing = facts_.find(key);
  if (existing != facts_.end()) {
    const CompatibilityFact& current = existing->second;
    if (current.generation >= assigned_generation) {
      decision.set_outcome(OutcomeCode::CONFLICT);
      return decision.add(ReasonCode::CompatibilityGenerationMismatch,
                          "published generation does not advance the stored generation");
    }
    if (current.model_generation > fact.model_generation) {
      decision.set_outcome(OutcomeCode::CONFLICT);
      return decision.add(ReasonCode::ModelGenerationMismatch,
                          "fact targets a model generation older than the stored fact");
    }
    if (current.model_generation == fact.model_generation &&
        current.artifact_generation > fact.artifact_generation) {
      decision.set_outcome(OutcomeCode::CONFLICT);
      return decision.add(ReasonCode::ArtifactGenerationMismatch,
                          "fact targets an artifact generation older than the stored fact");
    }
  } else if (facts_.size() >= max_facts_) {
    decision.set_outcome(OutcomeCode::BOUNDS_EXCEEDED);
    return decision.add(ReasonCode::BoundsModelCount, "compatibility fact bound reached");
  }

  CompatibilityFact stored = fact;
  stored.generation = assigned_generation;
  facts_[key] = std::move(stored);
  return decision;
}

const CompatibilityFact* CompatibilityRegistry::lookup(ModelVersionId version,
                                                       std::string_view environment_key) const {
  const auto found = facts_.find(std::make_pair(version.raw(), std::string(environment_key)));
  return found == facts_.end() ? nullptr : &found->second;
}

std::vector<const CompatibilityFact*> CompatibilityRegistry::facts_for(
    ModelVersionId version) const {
  std::vector<const CompatibilityFact*> out;
  for (const auto& entry : facts_) {
    if (entry.first.first == version.raw()) out.push_back(&entry.second);
  }
  return out;
}

CompatibilityGeneration CompatibilityRegistry::latest_generation(ModelVersionId version) const {
  CompatibilityGeneration latest{};
  for (const auto& entry : facts_) {
    if (entry.first.first != version.raw()) continue;
    if (entry.second.generation > latest) latest = entry.second.generation;
  }
  return latest;
}

std::size_t CompatibilityRegistry::invalidate_version(ModelVersionId version) {
  std::size_t removed = 0;
  for (auto it = facts_.begin(); it != facts_.end();) {
    if (it->first.first == version.raw()) {
      it = facts_.erase(it);
      ++removed;
    } else {
      ++it;
    }
  }
  return removed;
}

bool CompatibilityRegistry::restore(
    std::map<std::pair<std::uint64_t, std::string>, CompatibilityFact> facts) {
  if (facts.size() > max_facts_) return false;
  for (const auto& entry : facts) {
    const CompatibilityFact& fact = entry.second;
    if (entry.first.first != fact.version.raw()) return false;
    if (entry.first.second != fact.environment_key) return false;
    if (!fact.version.valid()) return false;
    if (fact.environment_key.empty() || fact.environment_key.size() > 128) return false;
    if (!fact.generation.valid()) return false;
    if (!fact.model_generation.valid()) return false;
    if (!fact.artifact_generation.valid()) return false;
  }
  facts_ = std::move(facts);
  return true;
}

}  // namespace mlf
