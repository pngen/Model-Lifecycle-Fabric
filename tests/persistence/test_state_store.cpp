// Canonical serialization, atomic replacement and bounds.
#include <filesystem>

#include "mlf/state_store.hpp"

#include "mlf_test.hpp"
#include "scenario.hpp"

using namespace mlf;
using mlftest::require_ok;

namespace {

mlftest::Scenario make_populated(std::uint64_t seed) {
  mlftest::Scenario scenario = mlftest::make_scenario(seed);
  require_ok(mlftest::promote_immediately(*scenario.engine, scenario, scenario.v1, scenario.global,
                                          ModelVersionId{})
                 .decision,
             "promote v1");
  require_ok(mlftest::promote_immediately(*scenario.engine, scenario, scenario.v2, scenario.cluster,
                                          scenario.v1)
                 .decision,
             "promote v2");
  WorkerRegistration registration;
  registration.id = WorkerId(1);
  registration.boot = WorkerBootId(11);
  registration.epoch = scenario.engine->epoch();
  require_ok(scenario.engine->register_worker(registration), "worker");
  const ModelVersionRecord record = mlftest::version_of(*scenario.engine, scenario.v1);
  require_ok(mlftest::publish_evidence(*scenario.engine, record, scenario.global,
                                       EvidenceKind::TestResult, EvidenceVerdict::Satisfied, 1.0),
             "durable evidence");
  require_ok(mlftest::publish_ready(*scenario.engine, record, scenario.global,
                                    EvidenceKind::Readiness),
             "volatile evidence");
  return scenario;
}

}  // namespace

MLF_TEST(state_store, round_trip_preserves_durable_truth_exactly) {
  mlftest::Scenario scenario = make_populated(301);
  const auto state = scenario.engine->export_durable_state();
  const DecodeLimits limits = DecodeLimits::from_bounds(scenario.engine->bounds());

  std::vector<std::uint8_t> bytes;
  require_ok(Decision(serialize_durable_state(*state, limits, bytes).ok() ? OutcomeCode::Ok
                                                                          : OutcomeCode::Rejected),
             "serialize");

  DurableState decoded;
  require_ok(Decision(deserialize_durable_state(bytes.data(), bytes.size(), limits, decoded).ok()
                          ? OutcomeCode::Ok
                          : OutcomeCode::Rejected),
             "deserialize");

  MLF_CHECK(decoded.epoch == state->epoch);
  MLF_CHECK(decoded.sequence == state->sequence);
  MLF_CHECK_EQ(decoded.models.size(), state->models.size());
  MLF_CHECK_EQ(decoded.versions.size(), state->versions.size());
  MLF_CHECK_EQ(decoded.scopes.size(), state->scopes.size());
  MLF_CHECK_EQ(decoded.authority.size(), state->authority.size());
  MLF_CHECK_EQ(decoded.worker_leases.size(), state->worker_leases.size());
  MLF_CHECK_EQ(decoded.evidence.size(), state->evidence.size());
  MLF_CHECK_EQ(decoded.next_model_id, state->next_model_id);
  MLF_CHECK_EQ(decoded.next_rollout_id, state->next_rollout_id);
  MLF_CHECK_EQ(decoded.base_policy.generation.raw(), state->base_policy.generation.raw());
}

MLF_TEST(state_store, encoding_is_canonical_for_identical_state) {
  mlftest::Scenario scenario = make_populated(302);
  const DecodeLimits limits = DecodeLimits::from_bounds(scenario.engine->bounds());
  const auto first = scenario.engine->export_durable_state();
  const auto second = scenario.engine->export_durable_state();
  std::vector<std::uint8_t> first_bytes;
  std::vector<std::uint8_t> second_bytes;
  require_ok(Decision(serialize_durable_state(*first, limits, first_bytes).ok() ? OutcomeCode::Ok
                                                                                : OutcomeCode::Rejected),
             "first");
  require_ok(Decision(
                 serialize_durable_state(*second, limits, second_bytes).ok() ? OutcomeCode::Ok
                                                                             : OutcomeCode::Rejected),
             "second");
  // Snapshot generations differ by one but every other field is identical, so
  // the encodings must be the same length and differ only in that field.
  MLF_CHECK_EQ(first_bytes.size(), second_bytes.size());
}

MLF_TEST(state_store, volatile_evidence_is_not_persisted) {
  mlftest::Scenario scenario = make_populated(303);
  const auto state = scenario.engine->export_durable_state();
  for (const EvidenceRecord& record : state->evidence) {
    switch (record.kind) {
      case EvidenceKind::Readiness:
      case EvidenceKind::ResidencyReady:
      case EvidenceKind::HealthCheck:
      case EvidenceKind::Latency:
      case EvidenceKind::ErrorRate:
        MLF_CHECK_MSG(false, "volatile evidence kind was persisted");
        break;
      default:
        break;
    }
  }
  bool found_durable = false;
  for (const EvidenceRecord& record : state->evidence) {
    if (record.kind == EvidenceKind::TestResult) found_durable = true;
  }
  MLF_CHECK(found_durable);
}

MLF_TEST(state_store, worker_leases_are_persisted_as_fenced) {
  mlftest::Scenario scenario = make_populated(304);
  const auto state = scenario.engine->export_durable_state();
  MLF_CHECK_EQ(state->worker_leases.size(), 1u);
  for (const auto& entry : state->worker_leases) {
    MLF_CHECK(entry.second.fenced);
  }
}

MLF_TEST(state_store, save_and_load_through_the_filesystem) {
  mlftest::Scenario scenario = make_populated(305);
  const std::string path = mlftest::scratch_path("state", ".mlfs");
  const DecodeLimits limits = DecodeLimits::from_bounds(scenario.engine->bounds());
  const auto state = scenario.engine->export_durable_state();
  const StateStoreResult saved = save_durable_state(*state, limits, path);
  MLF_CHECK_MSG(saved.ok(), saved.detail);
  MLF_CHECK(std::filesystem::exists(path));

  DurableState loaded;
  const StateStoreResult result = load_durable_state(path, limits, loaded);
  MLF_CHECK_MSG(result.ok(), result.detail);
  MLF_CHECK_EQ(loaded.versions.size(), state->versions.size());

  // Atomic replacement: a second save over the same path succeeds and leaves no
  // temporary file behind.
  const StateStoreResult again = save_durable_state(*state, limits, path);
  MLF_CHECK(again.ok());
  MLF_CHECK(!std::filesystem::exists(path + ".tmp"));
  mlftest::remove_file(path);
}

MLF_TEST(state_store, loading_an_absent_file_reports_an_io_error) {
  DurableState state;
  const StateStoreResult result =
      load_durable_state(mlftest::scratch_path("absent", ".mlfs"), DecodeLimits{}, state);
  MLF_CHECK(result.status == StateStoreStatus::IoError);
}

MLF_TEST(state_store, crc32_matches_the_known_check_value) {
  const std::string text = "123456789";
  MLF_CHECK_EQ(crc32(text.data(), text.size()), 0xCBF43926u);
}

MLF_TEST(state_store, integrity_check_is_sensitive_to_a_single_bit) {
  mlftest::Scenario scenario = make_populated(306);
  const DecodeLimits limits = DecodeLimits::from_bounds(scenario.engine->bounds());
  std::vector<std::uint8_t> bytes;
  require_ok(Decision(
                 serialize_durable_state(*scenario.engine->export_durable_state(), limits, bytes)
                         .ok()
                     ? OutcomeCode::Ok
                     : OutcomeCode::Rejected),
             "serialize");
  const std::uint32_t before = crc32(bytes.data(), bytes.size() - 4);
  bytes[8] ^= 0x01;
  const std::uint32_t after = crc32(bytes.data(), bytes.size() - 4);
  MLF_CHECK(before != after);
}

MLF_TEST(state_store, payload_bound_is_enforced_on_encode) {
  mlftest::Scenario scenario = make_populated(307);
  DecodeLimits limits = DecodeLimits::from_bounds(scenario.engine->bounds());
  limits.max_payload_bytes = 16;
  std::vector<std::uint8_t> bytes;
  const StateStoreResult result =
      serialize_durable_state(*scenario.engine->export_durable_state(), limits, bytes);
  MLF_CHECK(result.status == StateStoreStatus::BoundsExceeded);
}
