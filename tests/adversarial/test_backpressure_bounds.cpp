// Backpressure: bounded queues reject explicitly and account the loss.
#include "mlf/coordinator.hpp"
#include "mlf/engine.hpp"

#include "mlf_test.hpp"
#include "scenario.hpp"
#include "test_process_util.hpp"

using namespace mlf;
using mlftest::require_ok;

MLF_TEST(backpressure, the_frame_queue_is_bounded_and_accounts_rejections) {
  FrameQueue queue(3);
  Frame frame;
  frame.type = MessageType::HELLO;
  for (int i = 0; i < 3; ++i) MLF_CHECK(queue.push(frame) == ProtocolStatus::Ok);
  MLF_CHECK(queue.push(frame) == ProtocolStatus::TooManyPending);
  MLF_CHECK_EQ(queue.rejected(), 1u);
  MLF_CHECK_EQ(queue.size(), 3u);
  Frame popped;
  MLF_CHECK(queue.pop(popped));
  MLF_CHECK_EQ(queue.size(), 2u);
  MLF_CHECK(queue.push(frame) == ProtocolStatus::Ok);
}

MLF_TEST(backpressure, an_oversized_payload_is_refused_before_it_is_sent) {
  ProtocolLimits limits;
  limits.max_payload_bytes = 32;
  Frame frame;
  frame.type = MessageType::REGISTER_VERSION;
  frame.payload.assign(64, 0xAB);
  std::vector<std::uint8_t> encoded;
  MLF_CHECK(encode_frame(frame, limits, encoded) == ProtocolStatus::Oversized);
}

MLF_TEST(backpressure, an_oversized_reply_is_refused) {
  ProtocolLimits limits;
  limits.max_reasons = 2;
  Reply reply;
  reply.outcome = OutcomeCode::Rejected;
  for (int i = 0; i < 10; ++i) reply.add(ReasonCode::EvidenceMissing, "x");
  std::vector<std::uint8_t> payload;
  MLF_CHECK(encode(reply, limits, payload) == ProtocolStatus::Oversized);
}

MLF_TEST(backpressure, evidence_loss_is_visible_rather_than_silent) {
  EvidenceStore store(8);
  for (std::uint64_t i = 1; i <= 40; ++i) {
    EvidenceRecord record;
    record.kind = EvidenceKind::HealthCheck;
    record.verdict = EvidenceVerdict::Satisfied;
    record.subject.model = ModelId(1);
    record.subject.version = ModelVersionId(1);
    record.subject.model_generation = ModelGeneration(1);
    record.subject.scope = ScopeId(1);
    record.provenance = Provenance::Synthetic;
    record.generation = EvidenceGeneration(i);
    require_ok(store.publish(record), "publish");
  }
  MLF_CHECK(store.dropped() > 0);
  MLF_CHECK(store.size() <= 8);
}

MLF_TEST(backpressure, a_full_rollout_registry_rejects_rather_than_growing) {
  EngineConfig config;
  config.bounds.max_rollouts = 1;
  mlftest::Scenario scenario = mlftest::make_scenario(241, 2048, config);
  const ModelVersionRecord candidate = mlftest::version_of(*scenario.engine, scenario.v2);
  PromotionRequest promotion;
  promotion.model = candidate.model;
  promotion.candidate = candidate.version;
  promotion.scope = scenario.global;
  promotion.epoch = scenario.engine->epoch();
  promotion.environment_key = scenario.environment.key;
  promotion.strategy = RolloutStrategy::Canary;
  promotion.rollback_target = scenario.v1;
  const PromotionOutcome outcome = scenario.engine->promote(promotion);
  MLF_CHECK(outcome.decision.allowed());

  RolloutPlanRequest plan;
  plan.model = candidate.model;
  plan.candidate = candidate.version;
  plan.root_scope = scenario.global;
  plan.promotion = outcome.promotion;
  plan.rollback_target = scenario.v1;
  CohortSpec cohort;
  cohort.name = "c";
  cohort.scopes = {scenario.global};
  plan.cohorts.push_back(cohort);
  StageSpec stage;
  stage.name = "s";
  stage.cohort_indices = {0};
  plan.stages.push_back(stage);

  RolloutId rollout;
  RolloutGeneration generation;
  require_ok(scenario.engine->create_rollout(plan, rollout, generation), "first");
  const Decision second = scenario.engine->create_rollout(plan, rollout, generation);
  MLF_CHECK(!second.allowed());
  MLF_CHECK(second.has(ReasonCode::BoundsRolloutCount));
  const StoreStats stats = scenario.engine->stats();
  MLF_CHECK_EQ(stats.rollouts, 1u);
}

MLF_TEST(backpressure, a_coordinator_refuses_connections_beyond_its_bound) {
  CoordinatorConfig config;
  config.limits.max_connections = 2;
  mlftest::InProcessCoordinator coordinator;
  std::string error;
  if (!coordinator.start(error, config)) {
    MLF_CHECK_MSG(false, error);
  }
  const std::uint16_t port = coordinator.port();

  LifecycleClient first;
  LifecycleClient second;
  LifecycleClient third;
  MLF_CHECK(first.connect("127.0.0.1", port, error));
  MLF_CHECK(first.hello(WorkerId{}, WorkerBootId{}, CoordinatorEpoch{}, {}, error).ok());
  MLF_CHECK(second.connect("127.0.0.1", port, error));
  MLF_CHECK(second.hello(WorkerId{}, WorkerBootId{}, CoordinatorEpoch{}, {}, error).ok());

  // The third connection is accepted by the OS but closed by the coordinator as
  // soon as it is observed, so its first exchange fails rather than queueing.
  MLF_CHECK(third.connect("127.0.0.1", port, error));
  std::string exchange_error;
  const Reply reply =
      third.hello(WorkerId{}, WorkerBootId{}, CoordinatorEpoch{}, {}, exchange_error);
  MLF_CHECK(!reply.ok());

  first.close();
  second.close();
  third.close();
  coordinator.stop();
}

MLF_TEST(backpressure, the_engine_reports_evidence_gaps_in_its_statistics) {
  EngineConfig config;
  config.bounds.max_evidence = 4;
  mlftest::Scenario scenario = mlftest::make_scenario(242, 1024, config);
  const ModelVersionRecord record = mlftest::version_of(*scenario.engine, scenario.v1);
  for (int i = 0; i < 20; ++i) {
    static_cast<void>(mlftest::publish_evidence(*scenario.engine, record, scenario.global,
                                                EvidenceKind::HealthCheck,
                                                EvidenceVerdict::Satisfied, 1.0));
  }
  const StoreStats stats = scenario.engine->stats();
  MLF_CHECK(stats.evidence_dropped > 0);
  MLF_CHECK(stats.evidence <= 4);
}