// Model Lifecycle Fabric — explicit lifecycle state machine.
#pragma once

#include <cstdint>
#include <string_view>

#include "mlf/identity.hpp"
#include "mlf/status.hpp"

namespace mlf {

/// Lifecycle state of one model version generation.
///
/// Every state below participates in a real transition in this runtime. RETIRED
/// is terminal: a retired generation can never regain authority, and reviving it
/// requires a distinct new model generation.
enum class LifecycleState : std::uint8_t {
  /// Identity recorded; nothing validated yet.
  REGISTERED = 0,
  /// Artifact/compatibility validation is in progress or awaiting evidence.
  VALIDATING,
  /// Hard eligibility satisfied; promotion may be requested.
  PROMOTION_ELIGIBLE,
  /// Hard eligibility failed; promotion is refused until the cause is repaired.
  PROMOTION_BLOCKED,
  /// A bounded rollout plan exists and the first stage has not been entered.
  CANARY_PENDING,
  /// A canary stage is entered and is accumulating evidence.
  CANARY_ACTIVE,
  /// Canary acceptance criteria were met; fleet-wide advance is permitted.
  CANARY_PASSED,
  /// Canary acceptance criteria were violated; the candidate must not advance.
  CANARY_FAILED,
  /// A rollout is past canary and is advancing through remaining stages.
  ROLLOUT_ACTIVE,
  /// Authoritative in at least one scope but not in every scope of its plan.
  PARTIALLY_PROMOTED,
  /// Authoritative in every scope the plan targets.
  CURRENT,
  /// Losing authority; replicas are being drained but the generation is still
  /// retained for rollback.
  DRAINING,
  /// A rollback decision has been made and the transition has not committed.
  ROLLBACK_PENDING,
  /// Rollback is executing: the previous generation is being restored.
  ROLLING_BACK,
  /// Rollback committed; this generation no longer holds the scopes it lost.
  ROLLED_BACK,
  /// Retirement requested; preconditions are being satisfied.
  RETIREMENT_PENDING,
  /// Terminal. No new authority, no resurrection, historical records only.
  RETIRED,
  /// Evidence that authority depended on became stale or incompatible; the
  /// candidate must re-establish current evidence before any further advance.
  REVALIDATION_REQUIRED,
  /// Failure; the generation may not advance and must be retired.
  FAILED,
};

inline constexpr std::size_t kLifecycleStateCount = 19;

[[nodiscard]] std::string_view to_string(LifecycleState state) noexcept;
[[nodiscard]] bool parse_lifecycle_state(std::string_view text, LifecycleState& out) noexcept;

/// Structural check only: each edge additionally carries preconditions that the
/// engine enforces before committing.
[[nodiscard]] bool is_transition_allowed(LifecycleState from, LifecycleState to) noexcept;

/// RETIRED is the only state that may never change again.
[[nodiscard]] constexpr bool is_terminal(LifecycleState state) noexcept {
  return state == LifecycleState::RETIRED;
}

/// True when the state can hold serving authority in some scope.
[[nodiscard]] constexpr bool can_hold_authority(LifecycleState state) noexcept {
  switch (state) {
    case LifecycleState::CANARY_ACTIVE:
    case LifecycleState::CANARY_PASSED:
    case LifecycleState::ROLLOUT_ACTIVE:
    case LifecycleState::PARTIALLY_PROMOTED:
    case LifecycleState::CURRENT:
    case LifecycleState::DRAINING:
    case LifecycleState::ROLLBACK_PENDING:
    case LifecycleState::ROLLING_BACK:
      return true;
    default:
      return false;
  }
}

/// True when the state may still progress toward promotion.
[[nodiscard]] constexpr bool is_promotable_from(LifecycleState state) noexcept {
  switch (state) {
    case LifecycleState::REGISTERED:
    case LifecycleState::VALIDATING:
    case LifecycleState::PROMOTION_ELIGIBLE:
    case LifecycleState::REVALIDATION_REQUIRED:
      return true;
    default:
      return false;
  }
}

/// The states reachable in one transition from a state, in a deterministic order.
[[nodiscard]] const LifecycleState* successors(LifecycleState from, std::size_t& count) noexcept;

}  // namespace mlf
