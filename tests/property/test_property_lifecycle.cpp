// Randomized lifecycle property tests over deterministic seeds.
//
// Each test prints nothing on success; a failure reports the seed and the
// operation log needed to reproduce it.
#include <string>
#include <vector>

#include "mlf/engine.hpp"
#include "mlf/state_store.hpp"

#include "mlf_test.hpp"
#include "scenario.hpp"

using namespace mlf;
using mlftest::require_ok;

namespace {

struct Harness {
  mlftest::Scenario scenario;
  std::vector<std::string> log;
  mlftest::Rng rng;
  bool exclusive_conflict{false};
  bool retired_holds_authority{false};
  bool stale_rollout_committed{false};

  explicit Harness(std::uint64_t seed)
      : scenario(mlftest::make_scenario(seed, 2048)), rng(seed * 2654435761ull + 1) {}

  void note(const std::string& text) { log.push_back(text); }

  /// Verify the invariants that must hold after every operation.
  void check_invariants() {
    // 1. At most one exclusive authoritative generation per scope.
    std::map<std::uint64_t, int> exclusive_per_scope;
    for (const AuthorityBinding& binding : scenario.engine->all_authority_bindings()) {
      if (binding.superseded) continue;
      if (binding.kind != AuthorityKind::Exclusive) continue;
      ++exclusive_per_scope[binding.scope.raw()];
    }
    for (const auto& entry : exclusive_per_scope) {
      if (entry.second > 1) exclusive_conflict = true;
    }
    // 2. Retired generations hold no live authority.
    for (const ModelVersionRecord& record : scenario.engine->all_versions()) {
      if (!record.retired()) continue;
      for (const AuthorityBinding& binding :
           scenario.engine->authority_bindings(record.model, record.version)) {
        if (!binding.superseded) retired_holds_authority = true;
      }
    }
    // 3. Authority always cites the current generation of its version.
    for (const AuthorityBinding& binding : scenario.engine->all_authority_bindings()) {
      if (binding.superseded) continue;
      const ModelVersionRecord record =
          mlftest::version_of(*scenario.engine, binding.version);
      if (record.version.valid() && binding.model_generation > record.generation) {
        stale_rollout_committed = true;
      }
    }
  }
};

}  // namespace

MLF_TEST(property, randomized_operation_sequences_preserve_the_invariants) {
  for (std::uint64_t seed = 1; seed <= 40; ++seed) {
    Harness harness(seed);
    LifecycleEngine& engine = *harness.scenario.engine;

    for (int step = 0; step < 60; ++step) {
      const std::uint64_t choice = harness.rng.below(9);
      switch (choice) {
        case 0: {
          // Publish a compatibility fact.
          EnvironmentProfile environment = harness.scenario.environment;
          environment.key = "env-" + std::to_string(harness.rng.below(3));
          static_cast<void>(engine.publish_compatibility(
              harness.scenario.v1, environment, CompatibilityOutcome::COMPATIBLE, "property",
              Provenance::Synthetic));
          harness.note("compat v1");
          break;
        }
        case 1: {
          // Revise an artifact generation and revalidate.
          const ModelVersionRecord record =
              mlftest::version_of(engine, harness.scenario.v1);
          ReviseVersionRequest revise;
          revise.model = record.model;
          revise.version = record.version;
          revise.expected_generation = record.generation;
          revise.artifact.set_id = record.artifact.set_id;
          revise.artifact.generation =
              ArtifactGeneration(record.artifact.generation.raw() + 1);
          revise.artifact.digest = "property-" + std::to_string(step);
          revise.requirements = record.requirements;
          static_cast<void>(engine.revise_version(revise));
          static_cast<void>(engine.publish_compatibility(
              harness.scenario.v1, harness.scenario.environment,
              CompatibilityOutcome::COMPATIBLE, "recomputed", Provenance::Synthetic));
          harness.note("revise v1");
          break;
        }
        case 2: {
          static_cast<void>(mlftest::promote_immediately(engine, harness.scenario,
                                                         harness.scenario.v1,
                                                         harness.scenario.global,
                                                         ModelVersionId{}));
          harness.note("promote v1 global");
          break;
        }
        case 3: {
          static_cast<void>(mlftest::promote_immediately(engine, harness.scenario,
                                                         harness.scenario.v2,
                                                         harness.scenario.cluster,
                                                         harness.scenario.v1));
          harness.note("promote v2 cluster");
          break;
        }
        case 4: {
          RollbackRequest rollback;
          rollback.model = harness.scenario.model;
          rollback.candidate = harness.scenario.v2;
          rollback.target = harness.scenario.v1;
          rollback.scope = harness.scenario.cluster;
          rollback.epoch = engine.epoch();
          static_cast<void>(engine.rollback(rollback));
          harness.note("rollback v2");
          break;
        }
        case 5: {
          RetirementRequest retirement;
          retirement.model = harness.scenario.model;
          retirement.version = harness.scenario.v1;
          retirement.epoch = engine.epoch();
          static_cast<void>(engine.retire(retirement));
          harness.note("retire v1");
          break;
        }
        case 6: {
          // Snapshot and reload into a fresh engine.
          const auto state = engine.export_durable_state();
          LifecycleEngine reloaded;
          const Decision decision =
              reloaded.import_durable_state(*state, next_generation(state->epoch));
          if (decision.allowed()) {
            for (const ModelVersionRecord& record : engine.all_versions()) {
              const ModelVersionRecord after = mlftest::version_of(reloaded, record.version);
              if (after.version.valid()) {
                if (after.generation != record.generation ||
                    after.state != record.state) {
                  throw ::mlftest::Failure(
                      "reload changed durable truth: seed " + std::to_string(seed) + " step " +
                      std::to_string(step));
                }
              }
            }
          }
          harness.note("reload");
          break;
        }
        case 7: {
          CoordinatorEpoch next;
          static_cast<void>(engine.advance_epoch(engine.epoch(), next));
          harness.note("advance epoch");
          break;
        }
        default: {
          WorkerRegistration registration;
          registration.id = WorkerId(harness.rng.below(3) + 1);
          registration.boot = WorkerBootId(harness.rng.next() | 1u);
          registration.epoch = engine.epoch();
          static_cast<void>(engine.register_worker(registration));
          static_cast<void>(engine.fence_worker(registration.id, registration.boot,
                                                engine.epoch(), "property"));
          harness.note("worker churn");
          break;
        }
      }
      harness.check_invariants();
      if (harness.exclusive_conflict) {
        throw ::mlftest::Failure("two exclusive bindings in one scope at seed " +
                                 std::to_string(seed) + " step " + std::to_string(step));
      }
      if (harness.retired_holds_authority) {
        throw ::mlftest::Failure("retired generation holds authority at seed " +
                                 std::to_string(seed) + " step " + std::to_string(step));
      }
      if (harness.stale_rollout_committed) {
        throw ::mlftest::Failure("authority cites a future generation at seed " +
                                 std::to_string(seed) + " step " + std::to_string(step));
      }
    }
  }
}

MLF_TEST(property, promotion_never_skips_the_hard_gate) {
  for (std::uint64_t seed = 100; seed <= 130; ++seed) {
    mlftest::Scenario scenario = mlftest::make_scenario(seed, 1024);
    LifecycleEngine& engine = *scenario.engine;
    const ModelVersionRecord record = mlftest::version_of(engine, scenario.v2);

    PromotionRequest request;
    request.model = record.model;
    request.candidate = record.version;
    request.candidate_generation = record.generation;
    request.artifact_generation = record.artifact.generation;
    request.compatibility_generation = record.compatibility_generation;
    request.scope = scenario.global;
    request.epoch = engine.epoch();
    request.environment_key = scenario.environment.key;
    request.strategy = RolloutStrategy::ImmediateCutover;
    request.request_immediate_cutover = true;

    const bool require_target = engine.current_policy().require_rollback_target;
    if (require_target) request.rollback_target = scenario.v1;

    Explanation explanation;
    const Decision gate = engine.evaluate_promotion(request, &explanation);
    const PromotionOutcome outcome = engine.promote(request);
    if (gate.allowed() != outcome.decision.allowed()) {
      throw ::mlftest::Failure("gate and commit disagree at seed " + std::to_string(seed));
    }
    if (outcome.decision.allowed()) {
      const AuthorityQueryResult authority = engine.query_authority(scenario.global);
      if (authority.binding == nullptr || authority.binding->version != scenario.v2) {
        throw ::mlftest::Failure("committed promotion did not grant authority at seed " +
                                 std::to_string(seed));
      }
    } else {
      const AuthorityQueryResult authority = engine.query_authority(scenario.global);
      if (authority.binding != nullptr) {
        throw ::mlftest::Failure("refused promotion granted authority at seed " +
                                 std::to_string(seed));
      }
    }
  }
}

MLF_TEST(property, a_failed_transaction_leaves_state_unchanged) {
  for (std::uint64_t seed = 200; seed <= 230; ++seed) {
    mlftest::Scenario scenario = mlftest::make_scenario(seed, 1024);
    LifecycleEngine& engine = *scenario.engine;
    const Sequence before = engine.sequence();
    const std::size_t bindings_before = engine.all_authority_bindings().size();
    const std::size_t rollouts_before = engine.rollouts().size();

    // A promotion citing a superseded compatibility generation is refused;
    // nothing may change.
    PromotionRequest request;
    const ModelVersionRecord record = mlftest::version_of(engine, scenario.v2);
    request.model = record.model;
    request.candidate = record.version;
    request.compatibility_generation =
        CompatibilityGeneration(record.compatibility_generation.raw() + 5);
    request.scope = scenario.global;
    request.epoch = engine.epoch();
    request.environment_key = scenario.environment.key;
    const PromotionOutcome outcome = engine.promote(request);
    if (outcome.decision.allowed()) {
      throw ::mlftest::Failure("a promotion without a rollback target was allowed at seed " +
                               std::to_string(seed));
    }
    if (engine.sequence() != before) {
      throw ::mlftest::Failure("a refused promotion advanced the sequence at seed " +
                               std::to_string(seed));
    }
    if (engine.all_authority_bindings().size() != bindings_before ||
        engine.rollouts().size() != rollouts_before) {
      throw ::mlftest::Failure("a refused promotion mutated state at seed " +
                               std::to_string(seed));
    }
  }
}

MLF_TEST(property, deterministic_state_produces_deterministic_explanations) {
  for (std::uint64_t seed = 300; seed <= 315; ++seed) {
    mlftest::Scenario first = mlftest::make_scenario(seed, 1024);
    mlftest::Scenario second = mlftest::make_scenario(seed, 1024);
    const ModelVersionRecord record = mlftest::version_of(*first.engine, first.v2);

    PromotionRequest request;
    request.model = record.model;
    request.candidate = record.version;
    request.scope = first.global;
    request.epoch = first.engine->epoch();
    request.environment_key = first.environment.key;
    request.rollback_target = first.v1;

    Explanation a;
    Explanation b;
    const Decision first_decision = first.engine->evaluate_promotion(request, &a);
    const Decision second_decision = second.engine->evaluate_promotion(request, &b);
    if (first_decision.render() != second_decision.render()) {
      throw ::mlftest::Failure("decisions differ at seed " + std::to_string(seed));
    }
    if (a.render() != b.render()) {
      throw ::mlftest::Failure("explanations differ at seed " + std::to_string(seed));
    }
  }
}

MLF_TEST(property, persistence_round_trips_under_randomized_state) {
  for (std::uint64_t seed = 400; seed <= 425; ++seed) {
    mlftest::Scenario scenario = mlftest::make_scenario(seed, 1024);
    LifecycleEngine& engine = *scenario.engine;
    mlftest::Rng rng(seed);
    for (int step = 0; step < 20; ++step) {
      switch (rng.below(3)) {
        case 0:
          static_cast<void>(mlftest::promote_immediately(engine, scenario, scenario.v1,
                                                         scenario.global, ModelVersionId{}));
          break;
        case 1:
          static_cast<void>(mlftest::promote_immediately(engine, scenario, scenario.v2,
                                                         scenario.cluster, scenario.v1));
          break;
        default: {
          const ModelVersionRecord record = mlftest::version_of(engine, scenario.v1);
          static_cast<void>(mlftest::publish_evidence(engine, record, scenario.global,
                                                      EvidenceKind::TestResult,
                                                      EvidenceVerdict::Satisfied, 1.0));
          break;
        }
      }
    }
    const auto state = engine.export_durable_state();
    std::vector<std::uint8_t> bytes;
    const DecodeLimits limits = DecodeLimits::from_bounds(engine.bounds());
    if (!serialize_durable_state(*state, limits, bytes).ok()) {
      throw ::mlftest::Failure("serialize failed at seed " + std::to_string(seed));
    }
    DurableState decoded;
    if (!deserialize_durable_state(bytes.data(), bytes.size(), limits, decoded).ok()) {
      throw ::mlftest::Failure("deserialize failed at seed " + std::to_string(seed));
    }
    LifecycleEngine reloaded;
    const Decision decision = reloaded.import_durable_state(decoded, next_generation(decoded.epoch));
    if (!decision.allowed()) {
      throw ::mlftest::Failure("import refused at seed " + std::to_string(seed) + ": " +
                               decision.render());
    }
    for (const AuthorityBinding& binding : engine.all_authority_bindings()) {
      if (binding.superseded) continue;
      bool found = false;
      for (const AuthorityBinding& reloaded_binding : reloaded.all_authority_bindings()) {
        if (reloaded_binding.scope == binding.scope &&
            reloaded_binding.version == binding.version &&
            reloaded_binding.model_generation == binding.model_generation) {
          found = true;
        }
      }
      if (!found) {
        throw ::mlftest::Failure("authority lost across reload at seed " +
                                 std::to_string(seed));
      }
    }
  }
}
