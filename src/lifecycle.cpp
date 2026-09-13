#include "mlf/lifecycle.hpp"

#include <array>

namespace mlf {
namespace {

struct StateName {
  LifecycleState state;
  std::string_view name;
};

constexpr StateName kStateNames[] = {
    {LifecycleState::REGISTERED, "REGISTERED"},
    {LifecycleState::VALIDATING, "VALIDATING"},
    {LifecycleState::PROMOTION_ELIGIBLE, "PROMOTION_ELIGIBLE"},
    {LifecycleState::PROMOTION_BLOCKED, "PROMOTION_BLOCKED"},
    {LifecycleState::CANARY_PENDING, "CANARY_PENDING"},
    {LifecycleState::CANARY_ACTIVE, "CANARY_ACTIVE"},
    {LifecycleState::CANARY_PASSED, "CANARY_PASSED"},
    {LifecycleState::CANARY_FAILED, "CANARY_FAILED"},
    {LifecycleState::ROLLOUT_ACTIVE, "ROLLOUT_ACTIVE"},
    {LifecycleState::PARTIALLY_PROMOTED, "PARTIALLY_PROMOTED"},
    {LifecycleState::CURRENT, "CURRENT"},
    {LifecycleState::DRAINING, "DRAINING"},
    {LifecycleState::ROLLBACK_PENDING, "ROLLBACK_PENDING"},
    {LifecycleState::ROLLING_BACK, "ROLLING_BACK"},
    {LifecycleState::ROLLED_BACK, "ROLLED_BACK"},
    {LifecycleState::RETIREMENT_PENDING, "RETIREMENT_PENDING"},
    {LifecycleState::RETIRED, "RETIRED"},
    {LifecycleState::REVALIDATION_REQUIRED, "REVALIDATION_REQUIRED"},
    {LifecycleState::FAILED, "FAILED"},
};

using SuccessorList = std::array<LifecycleState, 12>;

struct TransitionRow {
  LifecycleState from;
  SuccessorList to;
  std::size_t count;
};

// Explicit successor table. RETIRED has no outgoing edge: a retired generation is
// never revived, and resurrection is rejected before it reaches this table.
constexpr TransitionRow kTransitions[] = {
    {LifecycleState::REGISTERED,
     {LifecycleState::VALIDATING, LifecycleState::PROMOTION_BLOCKED,
      LifecycleState::REVALIDATION_REQUIRED, LifecycleState::RETIREMENT_PENDING,
      LifecycleState::FAILED},
     5},
    {LifecycleState::VALIDATING,
     {LifecycleState::PROMOTION_ELIGIBLE, LifecycleState::PROMOTION_BLOCKED,
      LifecycleState::REVALIDATION_REQUIRED, LifecycleState::RETIREMENT_PENDING,
      LifecycleState::FAILED},
     5},
    {LifecycleState::PROMOTION_ELIGIBLE,
     {LifecycleState::CANARY_PENDING, LifecycleState::VALIDATING,
      LifecycleState::PROMOTION_BLOCKED, LifecycleState::REVALIDATION_REQUIRED,
      LifecycleState::RETIREMENT_PENDING, LifecycleState::FAILED},
     6},
    {LifecycleState::PROMOTION_BLOCKED,
     {LifecycleState::VALIDATING, LifecycleState::PROMOTION_ELIGIBLE,
      LifecycleState::REVALIDATION_REQUIRED, LifecycleState::RETIREMENT_PENDING,
      LifecycleState::FAILED},
     5},
    {LifecycleState::CANARY_PENDING,
     {LifecycleState::CANARY_ACTIVE, LifecycleState::CANARY_FAILED,
      LifecycleState::PROMOTION_BLOCKED, LifecycleState::REVALIDATION_REQUIRED,
      LifecycleState::RETIREMENT_PENDING, LifecycleState::FAILED},
     6},
    {LifecycleState::CANARY_ACTIVE,
     {LifecycleState::CANARY_PASSED, LifecycleState::CANARY_FAILED,
      LifecycleState::REVALIDATION_REQUIRED, LifecycleState::ROLLBACK_PENDING,
      LifecycleState::RETIREMENT_PENDING, LifecycleState::FAILED},
     6},
    {LifecycleState::CANARY_PASSED,
     {LifecycleState::ROLLOUT_ACTIVE, LifecycleState::PARTIALLY_PROMOTED,
      LifecycleState::CURRENT, LifecycleState::CANARY_FAILED,
      LifecycleState::ROLLBACK_PENDING, LifecycleState::REVALIDATION_REQUIRED,
      LifecycleState::DRAINING, LifecycleState::RETIREMENT_PENDING, LifecycleState::FAILED},
     9},
    {LifecycleState::CANARY_FAILED,
     {LifecycleState::ROLLBACK_PENDING, LifecycleState::ROLLED_BACK, LifecycleState::DRAINING,
      LifecycleState::RETIREMENT_PENDING, LifecycleState::REVALIDATION_REQUIRED,
      LifecycleState::FAILED},
     6},
    {LifecycleState::ROLLOUT_ACTIVE,
     {LifecycleState::PARTIALLY_PROMOTED, LifecycleState::CURRENT,
      LifecycleState::ROLLBACK_PENDING, LifecycleState::REVALIDATION_REQUIRED,
      LifecycleState::DRAINING, LifecycleState::RETIREMENT_PENDING, LifecycleState::FAILED},
     7},
    {LifecycleState::PARTIALLY_PROMOTED,
     {LifecycleState::ROLLOUT_ACTIVE, LifecycleState::CURRENT, LifecycleState::ROLLBACK_PENDING,
      LifecycleState::DRAINING, LifecycleState::REVALIDATION_REQUIRED,
      LifecycleState::RETIREMENT_PENDING, LifecycleState::FAILED},
     7},
    {LifecycleState::CURRENT,
     {LifecycleState::PARTIALLY_PROMOTED, LifecycleState::ROLLBACK_PENDING,
      LifecycleState::DRAINING, LifecycleState::REVALIDATION_REQUIRED,
      LifecycleState::RETIREMENT_PENDING, LifecycleState::FAILED},
     6},
    {LifecycleState::DRAINING,
     {LifecycleState::ROLLED_BACK, LifecycleState::RETIREMENT_PENDING,
      LifecycleState::REVALIDATION_REQUIRED, LifecycleState::FAILED},
     4},
    {LifecycleState::ROLLBACK_PENDING,
     {LifecycleState::ROLLING_BACK, LifecycleState::ROLLED_BACK, LifecycleState::CURRENT,
      LifecycleState::REVALIDATION_REQUIRED, LifecycleState::FAILED},
     5},
    {LifecycleState::ROLLING_BACK,
     {LifecycleState::ROLLED_BACK, LifecycleState::REVALIDATION_REQUIRED, LifecycleState::FAILED},
     3},
    {LifecycleState::ROLLED_BACK,
     {LifecycleState::DRAINING, LifecycleState::RETIREMENT_PENDING,
      LifecycleState::REVALIDATION_REQUIRED, LifecycleState::FAILED},
     4},
    {LifecycleState::RETIREMENT_PENDING,
     {LifecycleState::RETIRED, LifecycleState::DRAINING, LifecycleState::ROLLED_BACK,
      LifecycleState::REVALIDATION_REQUIRED, LifecycleState::FAILED},
     5},
    {LifecycleState::RETIRED, {}, 0},
    {LifecycleState::REVALIDATION_REQUIRED,
     {LifecycleState::VALIDATING, LifecycleState::PROMOTION_ELIGIBLE,
      LifecycleState::PROMOTION_BLOCKED, LifecycleState::DRAINING,
      LifecycleState::ROLLBACK_PENDING, LifecycleState::RETIREMENT_PENDING,
      LifecycleState::FAILED},
     7},
    {LifecycleState::FAILED,
     {LifecycleState::RETIREMENT_PENDING, LifecycleState::DRAINING, LifecycleState::RETIRED},
     3},
};

static_assert(sizeof(kTransitions) / sizeof(kTransitions[0]) == kLifecycleStateCount,
              "transition table must cover every lifecycle state exactly once");

const LifecycleState kNoSuccessors[1] = {LifecycleState::RETIRED};

}  // namespace

std::string_view to_string(LifecycleState state) noexcept {
  for (const StateName& entry : kStateNames) {
    if (entry.state == state) return entry.name;
  }
  return "UNKNOWN_STATE";
}

bool parse_lifecycle_state(std::string_view text, LifecycleState& out) noexcept {
  for (const StateName& entry : kStateNames) {
    if (entry.name == text) {
      out = entry.state;
      return true;
    }
  }
  return false;
}

const LifecycleState* successors(LifecycleState from, std::size_t& count) noexcept {
  for (const TransitionRow& row : kTransitions) {
    if (row.from == from) {
      count = row.count;
      return row.count == 0 ? kNoSuccessors : row.to.data();
    }
  }
  count = 0;
  return kNoSuccessors;
}

bool is_transition_allowed(LifecycleState from, LifecycleState to) noexcept {
  if (from == to) return false;
  if (is_terminal(from)) return false;
  std::size_t count = 0;
  const LifecycleState* list = successors(from, count);
  for (std::size_t i = 0; i < count; ++i) {
    if (list[i] == to) return true;
  }
  return false;
}

}  // namespace mlf
