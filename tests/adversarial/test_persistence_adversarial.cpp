// Persistence hardening: no partial apply under any corruption.
#include <fstream>

#include "mlf/state_store.hpp"

#include "mlf_test.hpp"
#include "scenario.hpp"

using namespace mlf;
using mlftest::require_ok;

namespace {

/// Produce a valid encoding of a small but complete durable state.
std::vector<std::uint8_t> valid_state(std::uint64_t seed) {
  mlftest::Scenario scenario = mlftest::make_scenario(seed);
  require_ok(mlftest::promote_immediately(*scenario.engine, scenario, scenario.v1, scenario.global,
                                          ModelVersionId{})
                 .decision,
             "promote");
  const auto state = scenario.engine->export_durable_state();
  std::vector<std::uint8_t> bytes;
  const StateStoreResult result = serialize_durable_state(
      *state, DecodeLimits::from_bounds(scenario.engine->bounds()), bytes);
  require_ok(Decision(result.ok() ? OutcomeCode::Ok : OutcomeCode::Rejected), "serialize");
  return bytes;
}

StateStoreStatus decode_status(const std::vector<std::uint8_t>& bytes) {
  DurableState state;
  return deserialize_durable_state(bytes.data(), bytes.size(), DecodeLimits{}, state).status;
}

}  // namespace

MLF_TEST(persistence_adversarial, an_empty_file_is_truncated) {
  DurableState state;
  const StateStoreResult result = deserialize_durable_state(nullptr, 0, DecodeLimits{}, state);
  MLF_CHECK(result.status == StateStoreStatus::Truncated);
}

MLF_TEST(persistence_adversarial, a_truncated_header_is_detected_at_every_prefix) {
  const std::vector<std::uint8_t> bytes = valid_state(201);
  for (std::size_t prefix = 0; prefix < bytes.size(); ++prefix) {
    DurableState state;
    const StateStoreResult result =
        deserialize_durable_state(bytes.data(), prefix, DecodeLimits{}, state);
    MLF_CHECK_MSG(!result.ok(), "prefix " + std::to_string(prefix) + " unexpectedly loaded");
  }
}

MLF_TEST(persistence_adversarial, a_wrong_magic_is_rejected) {
  std::vector<std::uint8_t> bytes = valid_state(202);
  bytes[0] = 'X';
  MLF_CHECK(decode_status(bytes) == StateStoreStatus::BadMagic);
}

MLF_TEST(persistence_adversarial, an_unsupported_version_is_rejected) {
  std::vector<std::uint8_t> bytes = valid_state(203);
  bytes[4] = 99;
  MLF_CHECK(decode_status(bytes) == StateStoreStatus::UnsupportedVersion);
}

MLF_TEST(persistence_adversarial, a_corrupt_integrity_check_is_rejected) {
  std::vector<std::uint8_t> bytes = valid_state(204);
  // Flip one payload byte without touching the CRC.
  bytes[bytes.size() / 2] ^= 0x40;
  MLF_CHECK(decode_status(bytes) == StateStoreStatus::IntegrityFailure);
}

MLF_TEST(persistence_adversarial, a_modified_crc_does_not_help) {
  std::vector<std::uint8_t> bytes = valid_state(205);
  bytes[kFrameHeaderSize] ^= 0x01;
  bytes.back() ^= 0xFF;
  const StateStoreStatus status = decode_status(bytes);
  MLF_CHECK(status == StateStoreStatus::IntegrityFailure);
}

MLF_TEST(persistence_adversarial, every_single_byte_flip_is_rejected) {
  const std::vector<std::uint8_t> bytes = valid_state(206);
  std::size_t accepted = 0;
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    std::vector<std::uint8_t> mutated = bytes;
    mutated[i] ^= 0x80;
    DurableState state;
    const StateStoreResult result =
        deserialize_durable_state(mutated.data(), mutated.size(), DecodeLimits{}, state);
    if (result.ok()) ++accepted;
  }
  MLF_CHECK_MSG(accepted == 0, "byte flips accepted: " + std::to_string(accepted));
}

MLF_TEST(persistence_adversarial, a_declared_payload_beyond_the_bound_is_rejected) {
  std::vector<std::uint8_t> bytes = valid_state(207);
  bytes[12] = 0xFF;
  bytes[13] = 0xFF;
  bytes[14] = 0xFF;
  bytes[15] = 0x7F;
  const StateStoreStatus status = decode_status(bytes);
  MLF_CHECK(status == StateStoreStatus::BoundsExceeded || status == StateStoreStatus::Truncated ||
            status == StateStoreStatus::IntegrityFailure);
}

MLF_TEST(persistence_adversarial, decode_limits_reject_an_oversized_registry) {
  mlftest::Scenario scenario = mlftest::make_scenario(208);
  const auto state = scenario.engine->export_durable_state();
  std::vector<std::uint8_t> bytes;
  require_ok(Decision(serialize_durable_state(*state, DecodeLimits::from_bounds(scenario.engine->bounds()),
                                              bytes)
                          .ok()
                          ? OutcomeCode::Ok
                          : OutcomeCode::Rejected),
             "serialize");

  DecodeLimits restrictive;
  restrictive.max_models = 0;
  DurableState decoded;
  const StateStoreResult result =
      deserialize_durable_state(bytes.data(), bytes.size(), restrictive, decoded);
  MLF_CHECK(!result.ok());
  MLF_CHECK(result.status == StateStoreStatus::BoundsExceeded ||
            result.status == StateStoreStatus::InvalidContent);
}

MLF_TEST(persistence_adversarial, a_rejected_load_leaves_the_engine_untouched) {
  mlftest::Scenario scenario = mlftest::make_scenario(209);
  require_ok(mlftest::promote_immediately(*scenario.engine, scenario, scenario.v1, scenario.global,
                                          ModelVersionId{})
                 .decision,
             "promote");
  const std::size_t models_before = scenario.engine->models().size();
  const std::size_t versions_before = scenario.engine->all_versions().size();
  const std::string authority_before =
      std::string(to_string(scenario.engine->query_authority(scenario.global).outcome));

  DurableState hostile;
  hostile.format_version = kStateFormatVersion;
  hostile.epoch = next_generation(scenario.engine->epoch());
  // A version that references a model which does not exist.
  ModelVersionRecord orphan;
  orphan.model = ModelId(999);
  orphan.version = ModelVersionId(999);
  orphan.generation = ModelGeneration(1);
  orphan.lifecycle_generation = LifecycleGeneration(1);
  orphan.artifact.set_id = ArtifactSetId(1);
  orphan.artifact.generation = ArtifactGeneration(1);
  orphan.artifact.digest = "d";
  hostile.versions[std::make_pair(999ull, 999ull)] = orphan;

  const Decision decision = scenario.engine->import_durable_state(hostile, hostile.epoch);
  MLF_CHECK(!decision.allowed());
  MLF_CHECK_EQ(scenario.engine->models().size(), models_before);
  MLF_CHECK_EQ(scenario.engine->all_versions().size(), versions_before);
  MLF_CHECK_EQ(std::string(to_string(scenario.engine->query_authority(scenario.global).outcome)),
               authority_before);
}

MLF_TEST(persistence_adversarial, a_retired_generation_cannot_be_persisted_as_authoritative) {
  mlftest::Scenario scenario = mlftest::make_scenario(210);
  require_ok(mlftest::promote_immediately(*scenario.engine, scenario, scenario.v2, scenario.global,
                                          ModelVersionId{})
                 .decision,
             "v2");
  RetirementRequest retirement;
  retirement.model = scenario.model;
  retirement.version = scenario.v1;
  retirement.epoch = scenario.engine->epoch();
  require_ok(scenario.engine->retire(retirement), "retire");

  const auto state = scenario.engine->export_durable_state();
  DurableState hostile = *state;
  const ModelVersionRecord retired = mlftest::version_of(*scenario.engine, scenario.v1);
  MLF_CHECK(retired.retired());
  AuthorityBinding binding;
  binding.scope = scenario.global;
  binding.scope_generation = mlftest::generation_of(*scenario.engine, scenario.global);
  binding.model = scenario.model;
  binding.version = scenario.v1;
  binding.model_generation = retired.generation;
  binding.kind = AuthorityKind::Exclusive;
  binding.granted_sequence = Sequence(1);
  hostile.authority[scenario.global.raw()] = {binding};

  LifecycleEngine fresh;
  const Decision decision = fresh.import_durable_state(hostile, next_generation(hostile.epoch));
  MLF_CHECK(!decision.allowed());
}

MLF_TEST(persistence_adversarial, an_impossible_lifecycle_state_is_rejected) {
  mlftest::Scenario scenario = mlftest::make_scenario(212);
  DurableState hostile = *scenario.engine->export_durable_state();
  for (auto& entry : hostile.versions) {
    entry.second.state = static_cast<LifecycleState>(200);
  }
  LifecycleEngine fresh;
  const Decision decision = fresh.import_durable_state(hostile, next_generation(hostile.epoch));
  MLF_CHECK(!decision.allowed());
  MLF_CHECK(decision.has(ReasonCode::InvalidLifecycleTransition));
  // Nothing was applied.
  MLF_CHECK(fresh.all_versions().empty());
}

MLF_TEST(persistence_adversarial, a_scope_identity_hole_is_rejected) {
  mlftest::Scenario scenario = mlftest::make_scenario(213);
  DurableState hostile = *scenario.engine->export_durable_state();
  // Removing an interior scope leaves the remaining identities non-contiguous,
  // which would let a later registration collide with an existing identity.
  hostile.scopes.erase(hostile.scopes.begin() + 1);
  LifecycleEngine fresh;
  const Decision decision = fresh.import_durable_state(hostile, next_generation(hostile.epoch));
  MLF_CHECK(!decision.allowed());
}

MLF_TEST(persistence_adversarial, duplicate_scope_paths_are_rejected) {
  mlftest::Scenario scenario = mlftest::make_scenario(214);
  DurableState hostile = *scenario.engine->export_durable_state();
  hostile.scopes.back().path = hostile.scopes.front().path;
  LifecycleEngine fresh;
  MLF_CHECK(!fresh.import_durable_state(hostile, next_generation(hostile.epoch)).allowed());
}

MLF_TEST(persistence_adversarial, an_authority_binding_without_a_granting_sequence_is_rejected) {
  mlftest::Scenario scenario = mlftest::make_scenario(216);
  require_ok(mlftest::promote_immediately(*scenario.engine, scenario, scenario.v1, scenario.global,
                                          ModelVersionId{})
                 .decision,
             "promote");
  DurableState hostile = *scenario.engine->export_durable_state();
  for (auto& entry : hostile.authority) {
    for (AuthorityBinding& binding : entry.second) binding.granted_sequence = Sequence{};
  }
  LifecycleEngine fresh;
  const Decision decision = fresh.import_durable_state(hostile, next_generation(hostile.epoch));
  MLF_CHECK(!decision.allowed());
  MLF_CHECK(decision.has(ReasonCode::SequenceRegression));
}

MLF_TEST(persistence_adversarial, an_authority_binding_for_an_unknown_rollout_is_rejected) {
  mlftest::Scenario scenario = mlftest::make_scenario(215);
  require_ok(mlftest::promote_immediately(*scenario.engine, scenario, scenario.v1, scenario.global,
                                          ModelVersionId{})
                 .decision,
             "promote");
  DurableState hostile = *scenario.engine->export_durable_state();
  for (auto& entry : hostile.authority) {
    for (AuthorityBinding& binding : entry.second) {
      binding.rollout = RolloutId(9999);
      binding.rollout_generation = RolloutGeneration(1);
    }
  }
  LifecycleEngine fresh;
  MLF_CHECK(!fresh.import_durable_state(hostile, next_generation(hostile.epoch)).allowed());
}

MLF_TEST(persistence_adversarial, a_partial_temporary_file_is_never_loaded) {
  const std::string path = mlftest::scratch_path("partial", ".mlfs");
  {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << "MLFS";
    stream.flush();
  }
  DurableState state;
  const StateStoreResult result = load_durable_state(path, DecodeLimits{}, state);
  MLF_CHECK(!result.ok());
  MLF_CHECK(result.status == StateStoreStatus::Truncated);
  mlftest::remove_file(path);
}
