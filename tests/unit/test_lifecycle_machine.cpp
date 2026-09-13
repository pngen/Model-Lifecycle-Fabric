// Lifecycle state machine.
#include <set>
#include <vector>

#include "mlf/lifecycle.hpp"

#include "mlf_test.hpp"

using namespace mlf;

MLF_TEST(lifecycle, every_state_has_a_name_and_round_trips) {
  for (std::size_t i = 0; i < kLifecycleStateCount; ++i) {
    const auto state = static_cast<LifecycleState>(i);
    const std::string_view name = to_string(state);
    MLF_CHECK(name != std::string_view("UNKNOWN_STATE"));
    LifecycleState parsed{};
    MLF_CHECK(parse_lifecycle_state(name, parsed));
    MLF_CHECK(parsed == state);
  }
  LifecycleState ignored{};
  MLF_CHECK(!parse_lifecycle_state("NOT_A_STATE", ignored));
}

MLF_TEST(lifecycle, retired_is_terminal) {
  MLF_CHECK(is_terminal(LifecycleState::RETIRED));
  std::size_t count = 99;
  const LifecycleState* list = successors(LifecycleState::RETIRED, count);
  MLF_CHECK_EQ(count, 0u);
  static_cast<void>(list);
  for (std::size_t i = 0; i < kLifecycleStateCount; ++i) {
    MLF_CHECK(!is_transition_allowed(LifecycleState::RETIRED, static_cast<LifecycleState>(i)));
  }
  MLF_CHECK(!is_transition_allowed(LifecycleState::RETIRED, LifecycleState::RETIRED));
}

MLF_TEST(lifecycle, transitions_are_asymmetric_where_they_must_be) {
  MLF_CHECK(is_transition_allowed(LifecycleState::REGISTERED, LifecycleState::VALIDATING));
  MLF_CHECK(!is_transition_allowed(LifecycleState::VALIDATING, LifecycleState::REGISTERED));
  MLF_CHECK(is_transition_allowed(LifecycleState::CANARY_ACTIVE, LifecycleState::CANARY_PASSED));
  MLF_CHECK(!is_transition_allowed(LifecycleState::CANARY_PASSED, LifecycleState::CANARY_ACTIVE));
  MLF_CHECK(is_transition_allowed(LifecycleState::CURRENT, LifecycleState::ROLLBACK_PENDING));
  MLF_CHECK(is_transition_allowed(LifecycleState::CANARY_PASSED, LifecycleState::CURRENT));
  // Immediate cutover reaches CURRENT in one step from a passed canary, not
  // directly from eligibility; the state machine never skips a state.
  MLF_CHECK(!is_transition_allowed(LifecycleState::PROMOTION_ELIGIBLE, LifecycleState::CURRENT));
  MLF_CHECK(is_transition_allowed(LifecycleState::PROMOTION_ELIGIBLE,
                                  LifecycleState::CANARY_PENDING));
}

MLF_TEST(lifecycle, authority_capability_matches_the_design) {
  MLF_CHECK(can_hold_authority(LifecycleState::CURRENT));
  MLF_CHECK(can_hold_authority(LifecycleState::CANARY_ACTIVE));
  MLF_CHECK(can_hold_authority(LifecycleState::DRAINING));
  MLF_CHECK(!can_hold_authority(LifecycleState::RETIRED));
  MLF_CHECK(!can_hold_authority(LifecycleState::ROLLED_BACK));
  MLF_CHECK(!can_hold_authority(LifecycleState::REGISTERED));
  MLF_CHECK(!can_hold_authority(LifecycleState::CANARY_FAILED));
}

MLF_TEST(lifecycle, promotable_states_are_bounded) {
  MLF_CHECK(is_promotable_from(LifecycleState::REGISTERED));
  MLF_CHECK(is_promotable_from(LifecycleState::PROMOTION_ELIGIBLE));
  MLF_CHECK(is_promotable_from(LifecycleState::REVALIDATION_REQUIRED));
  MLF_CHECK(!is_promotable_from(LifecycleState::CURRENT));
  MLF_CHECK(!is_promotable_from(LifecycleState::RETIRED));
  MLF_CHECK(!is_promotable_from(LifecycleState::CANARY_ACTIVE));
}

MLF_TEST(lifecycle, successor_table_is_closed_over_known_states) {
  for (std::size_t i = 0; i < kLifecycleStateCount; ++i) {
    const auto state = static_cast<LifecycleState>(i);
    std::size_t count = 0;
    const LifecycleState* list = successors(state, count);
    MLF_CHECK(list != nullptr);
    for (std::size_t k = 0; k < count; ++k) {
      MLF_CHECK(static_cast<std::size_t>(list[k]) < kLifecycleStateCount);
      MLF_CHECK(is_transition_allowed(state, list[k]));
    }
  }
}

MLF_TEST(lifecycle, every_state_is_reachable_from_registered) {
  // A state that is unreachable would be a state the runtime can never produce.
  std::vector<LifecycleState> frontier{LifecycleState::REGISTERED};
  std::set<std::uint8_t> seen{static_cast<std::uint8_t>(LifecycleState::REGISTERED)};
  while (!frontier.empty()) {
    const LifecycleState current = frontier.back();
    frontier.pop_back();
    std::size_t count = 0;
    const LifecycleState* list = successors(current, count);
    for (std::size_t i = 0; i < count; ++i) {
      if (seen.insert(static_cast<std::uint8_t>(list[i])).second) frontier.push_back(list[i]);
    }
  }
  for (std::size_t i = 0; i < kLifecycleStateCount; ++i) {
    MLF_CHECK_MSG(seen.count(static_cast<std::uint8_t>(i)) == 1,
                  std::string("unreachable state: ") +
                      std::string(to_string(static_cast<LifecycleState>(i))));
  }
}
