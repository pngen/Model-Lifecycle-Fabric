#include "mlf/rollout.hpp"

#include <algorithm>

namespace mlf {
namespace {

struct StrategyName {
  RolloutStrategy strategy;
  std::string_view name;
};

constexpr StrategyName kStrategyNames[] = {
    {RolloutStrategy::Canary, "canary"},
    {RolloutStrategy::Percentage, "percentage"},
    {RolloutStrategy::ScopeByScope, "scope-by-scope"},
    {RolloutStrategy::BlueGreen, "blue-green"},
    {RolloutStrategy::ManualStages, "manual-stages"},
    {RolloutStrategy::ImmediateCutover, "immediate-cutover"},
};

static_assert(sizeof(kStrategyNames) / sizeof(kStrategyNames[0]) == kRolloutStrategyCount,
              "rollout strategy table must be exhaustive");

constexpr std::string_view kStageDecisionNames[] = {"PENDING", "ENTERED", "ADVANCED",
                                                    "FAILED",  "ROLLED_BACK", "SUPERSEDED"};

}  // namespace

std::string_view to_string(RolloutStrategy strategy) noexcept {
  for (const StrategyName& entry : kStrategyNames) {
    if (entry.strategy == strategy) return entry.name;
  }
  return "unknown";
}

bool parse_rollout_strategy(std::string_view text, RolloutStrategy& out) noexcept {
  for (const StrategyName& entry : kStrategyNames) {
    if (entry.name == text) {
      out = entry.strategy;
      return true;
    }
  }
  return false;
}

std::string_view to_string(StageDecision decision) noexcept {
  const auto index = static_cast<std::size_t>(decision);
  if (index >= 6) return "UNKNOWN";
  return kStageDecisionNames[index];
}

std::string_view to_string(CriterionComparison comparison) noexcept {
  return comparison == CriterionComparison::AtLeast ? "at-least" : "at-most";
}

std::vector<ScopeId> RolloutPlan::scopes_through(std::size_t index) const {
  std::vector<ScopeId> out;
  const std::size_t limit = std::min(index + 1, stages.size());
  for (std::size_t i = 0; i < limit; ++i) {
    for (CohortId cohort_id : stages[i].cohorts) {
      const CohortDefinition* definition = cohort(cohort_id);
      if (definition == nullptr) continue;
      for (ScopeId scope : definition->scopes) out.push_back(scope);
    }
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

std::vector<ScopeId> RolloutPlan::committed_scopes() const {
  std::vector<ScopeId> out;
  for (const StageRecord& record : history) {
    if (record.decision != StageDecision::Advanced) continue;
    for (std::size_t i = 0; i < stages.size(); ++i) {
      if (stages[i].id != record.stage) continue;
      for (CohortId cohort_id : stages[i].cohorts) {
        const CohortDefinition* definition = cohort(cohort_id);
        if (definition == nullptr) continue;
        for (ScopeId scope : definition->scopes) out.push_back(scope);
      }
    }
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

}  // namespace mlf
