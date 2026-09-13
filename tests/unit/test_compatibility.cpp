// Compatibility model.
#include "mlf/compatibility.hpp"

#include "mlf_test.hpp"

using namespace mlf;

namespace {

ModelRequirements requirements() {
  ModelRequirements value;
  value.runtime = "tensorrt-llm";
  value.runtime_min_version = "0.12.0";
  value.backend = "cuda";
  value.architecture = "sm_120";
  value.precision = "fp8";
  value.tokenizer_generation = "tok-3";
  value.adapter_set = "lora-a";
  return value;
}

EnvironmentProfile environment() {
  EnvironmentProfile value;
  value.key = "env";
  value.runtime = "tensorrt-llm";
  value.runtime_version = "0.12.4";
  value.backend = "cuda";
  value.architecture = "sm_120";
  value.precisions.insert("fp8");
  value.tokenizer_generation = "tok-3";
  value.adapter_set = "lora-a";
  value.compute_capability = 1200;
  value.provenance = Provenance::Real;
  return value;
}

}  // namespace

MLF_TEST(compatibility, version_comparison_is_numeric_and_total) {
  MLF_CHECK_EQ(compare_versions("1.2.3", "1.2.3"), 0);
  MLF_CHECK(compare_versions("1.2.3", "1.2.4") < 0);
  MLF_CHECK(compare_versions("1.10.0", "1.9.0") > 0);
  MLF_CHECK(compare_versions("2.0", "2.0.0") < 0);
  MLF_CHECK(compare_versions("1.0.0-rc1", "1.0.0") < 0);
  MLF_CHECK(compare_versions("01.2", "1.2") == 0);
  MLF_CHECK(compare_versions("", "") == 0);
}

MLF_TEST(compatibility, architecture_token_matches_cuda_convention) {
  MLF_CHECK_EQ(cuda_architecture_token(12, 0), std::string("sm_120"));
  MLF_CHECK_EQ(cuda_architecture_token(9, 0), std::string("sm_90"));
  MLF_CHECK_EQ(cuda_architecture_token(8, 6), std::string("sm_86"));
}

MLF_TEST(compatibility, full_match_is_compatible) {
  const CompatibilityResult result = evaluate_requirements(requirements(), environment());
  MLF_CHECK_EQ(std::string(to_string(result.outcome)), std::string("COMPATIBLE"));
  MLF_CHECK(result.decision.allowed());
}

MLF_TEST(compatibility, each_dimension_produces_a_named_reason) {
  {
    EnvironmentProfile env = environment();
    env.runtime = "vllm";
    const CompatibilityResult result = evaluate_requirements(requirements(), env);
    MLF_CHECK_EQ(std::string(to_string(result.outcome)), std::string("INCOMPATIBLE_RUNTIME"));
    MLF_CHECK(result.decision.has(ReasonCode::IncompatibleRuntime));
  }
  {
    EnvironmentProfile env = environment();
    env.runtime_version = "0.11.0";
    const CompatibilityResult result = evaluate_requirements(requirements(), env);
    MLF_CHECK_EQ(std::string(to_string(result.outcome)), std::string("INCOMPATIBLE_RUNTIME"));
  }
  {
    EnvironmentProfile env = environment();
    env.backend = "rocm";
    const CompatibilityResult result = evaluate_requirements(requirements(), env);
    MLF_CHECK_EQ(std::string(to_string(result.outcome)), std::string("INCOMPATIBLE_BACKEND"));
  }
  {
    EnvironmentProfile env = environment();
    env.architecture = "sm_90";
    env.compute_capability = 900;
    const CompatibilityResult result = evaluate_requirements(requirements(), env);
    MLF_CHECK_EQ(std::string(to_string(result.outcome)),
                 std::string("INCOMPATIBLE_ARCHITECTURE"));
  }
  {
    EnvironmentProfile env = environment();
    env.precisions.clear();
    env.precisions.insert("bf16");
    const CompatibilityResult result = evaluate_requirements(requirements(), env);
    MLF_CHECK_EQ(std::string(to_string(result.outcome)), std::string("INCOMPATIBLE_PRECISION"));
  }
  {
    EnvironmentProfile env = environment();
    env.tokenizer_generation = "tok-4";
    const CompatibilityResult result = evaluate_requirements(requirements(), env);
    MLF_CHECK_EQ(std::string(to_string(result.outcome)), std::string("INCOMPATIBLE_TOKENIZER"));
  }
  {
    EnvironmentProfile env = environment();
    env.adapter_set = "lora-b";
    const CompatibilityResult result = evaluate_requirements(requirements(), env);
    MLF_CHECK_EQ(std::string(to_string(result.outcome)), std::string("INCOMPATIBLE_ADAPTER"));
  }
}

MLF_TEST(compatibility, missing_environment_facts_produce_unknown_never_compatible) {
  EnvironmentProfile env = environment();
  env.backend.clear();
  const CompatibilityResult result = evaluate_requirements(requirements(), env);
  MLF_CHECK_EQ(std::string(to_string(result.outcome)), std::string("UNKNOWN"));
  MLF_CHECK(!result.decision.allowed());

  ModelRequirements empty;
  const CompatibilityResult nothing = evaluate_requirements(empty, environment());
  MLF_CHECK_EQ(std::string(to_string(nothing.outcome)), std::string("UNKNOWN"));
}

MLF_TEST(compatibility, invalid_environment_is_unsupported) {
  EnvironmentProfile env;
  const CompatibilityResult result = evaluate_requirements(requirements(), env);
  MLF_CHECK_EQ(std::string(to_string(result.outcome)), std::string("UNKNOWN"));
  MLF_CHECK(result.decision.has(ReasonCode::CompatibilityUnknown));
}

MLF_TEST(compatibility, compute_capability_satisfies_architecture_requirement) {
  EnvironmentProfile env = environment();
  env.architecture.clear();
  const CompatibilityResult result = evaluate_requirements(requirements(), env);
  MLF_CHECK_EQ(std::string(to_string(result.outcome)), std::string("COMPATIBLE"));
}

MLF_TEST(compatibility, registry_refuses_to_overwrite_newer_truth) {
  CompatibilityRegistry registry(4);
  CompatibilityFact fact;
  fact.version = ModelVersionId(1);
  fact.model = ModelId(1);
  fact.model_generation = ModelGeneration(2);
  fact.artifact_generation = ArtifactGeneration(1);
  fact.environment_key = "env";
  fact.outcome = CompatibilityOutcome::COMPATIBLE;

  MLF_CHECK(registry.publish(fact, CompatibilityGeneration(2)).allowed());

  CompatibilityFact older = fact;
  older.model_generation = ModelGeneration(1);
  Decision refused = registry.publish(older, CompatibilityGeneration(1));
  MLF_CHECK(!refused.allowed());
  MLF_CHECK(refused.has(ReasonCode::CompatibilityGenerationMismatch));

  CompatibilityFact stale_model = fact;
  stale_model.model_generation = ModelGeneration(1);
  Decision stale = registry.publish(stale_model, CompatibilityGeneration(3));
  MLF_CHECK(!stale.allowed());
  MLF_CHECK(stale.has(ReasonCode::ModelGenerationMismatch));

  MLF_CHECK_EQ(registry.latest_generation(ModelVersionId(1)).raw(), 2u);
}

MLF_TEST(compatibility, registry_invalidation_drops_every_fact_for_a_version) {
  CompatibilityRegistry registry;
  for (std::uint64_t i = 1; i <= 3; ++i) {
    CompatibilityFact fact;
    fact.version = ModelVersionId(1);
    fact.model = ModelId(1);
    fact.model_generation = ModelGeneration(1);
    fact.artifact_generation = ArtifactGeneration(1);
    fact.environment_key = "env" + std::to_string(i);
    fact.outcome = CompatibilityOutcome::COMPATIBLE;
    MLF_CHECK(registry.publish(fact, CompatibilityGeneration(i)).allowed());
  }
  MLF_CHECK_EQ(registry.size(), 3u);
  MLF_CHECK_EQ(registry.invalidate_version(ModelVersionId(1)), 3u);
  MLF_CHECK_EQ(registry.size(), 0u);
  MLF_CHECK(registry.lookup(ModelVersionId(1), "env1") == nullptr);
}

MLF_TEST(compatibility, registry_bounds_are_enforced) {
  CompatibilityRegistry registry(2);
  for (std::uint64_t i = 1; i <= 3; ++i) {
    CompatibilityFact fact;
    fact.version = ModelVersionId(1);
    fact.model = ModelId(1);
    fact.model_generation = ModelGeneration(1);
    fact.artifact_generation = ArtifactGeneration(1);
    fact.environment_key = "env" + std::to_string(i);
    fact.outcome = CompatibilityOutcome::COMPATIBLE;
    const Decision decision = registry.publish(fact, CompatibilityGeneration(i));
    if (i <= 2) {
      MLF_CHECK(decision.allowed());
    } else {
      MLF_CHECK(!decision.allowed());
      MLF_CHECK(decision.has(ReasonCode::BoundsModelCount));
    }
  }
}

MLF_TEST(compatibility, outcome_names_round_trip) {
  for (std::size_t i = 0; i < kCompatibilityOutcomeCount; ++i) {
    const auto outcome = static_cast<CompatibilityOutcome>(i);
    CompatibilityOutcome parsed{};
    MLF_CHECK(parse_compatibility_outcome(to_string(outcome), parsed));
    MLF_CHECK(parsed == outcome);
  }
  CompatibilityOutcome ignored{};
  MLF_CHECK(!parse_compatibility_outcome("NOT_AN_OUTCOME", ignored));
}
