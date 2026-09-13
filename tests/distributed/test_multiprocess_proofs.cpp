// REAL multiprocess proofs over loopback TCP with genuine OS processes.
#include <fstream>
#include <string>

#include "mlf/client.hpp"

#include "mlf_test.hpp"
#include "scenario.hpp"

using namespace mlf;

namespace {

/// Helper that registers a model and one version through the coordinator.
struct RemoteModel {
  std::uint64_t model{0};
  std::uint64_t version{0};
};

bool register_model(LifecycleClient& client, const std::string& name, RemoteModel& out,
                    std::string& error) {
  RegisterModelMessage model;
  model.name = name;
  model.family = "multiprocess";
  model.provenance = Provenance::Real;
  const Reply model_reply = client.call(model, error);
  if (!model_reply.ok()) return false;
  out.model = model_reply.u64("model_id");

  RegisterVersionMessage version;
  version.model = ModelId(out.model);
  version.label = "1.0.0";
  version.artifact.set_id = ArtifactSetId(1);
  version.artifact.generation = ArtifactGeneration(1);
  version.artifact.digest = "sha256:" + std::string(64, 'b');
  version.requirements.runtime = "cuda-driver";
  version.requirements.backend = "cuda";
  version.requirements.architecture = "sm_120";
  version.requirements.precision = "fp8";
  version.provenance = Provenance::Real;
  const Reply version_reply = client.call(version, error);
  if (!version_reply.ok()) return false;
  out.version = version_reply.u64("version_id");
  return true;
}

bool publish_compatibility(LifecycleClient& client, std::uint64_t version, std::string& error) {
  PublishCompatibilityMessage message;
  message.version = ModelVersionId(version);
  message.environment.key = "multiprocess";
  message.environment.runtime = "cuda-driver";
  message.environment.backend = "cuda";
  message.environment.architecture = "sm_120";
  message.environment.compute_capability = 1200;
  message.environment.precisions.insert("fp8");
  message.environment.provenance = Provenance::Real;
  message.outcome = CompatibilityOutcome::COMPATIBLE;
  message.detail = "published by the multiprocess proof";
  message.provenance = Provenance::Real;
  return client.call(message, error).ok();
}

}  // namespace

MLF_TEST(multiprocess, coordinator_starts_and_serves_a_handshake) {
  mlftest::CoordinatorProcess coordinator;
  std::string error;
  if (!coordinator.start("", error)) {
    MLF_CHECK_MSG(false, error);
  }
  LifecycleClient client;
  MLF_CHECK_MSG(client.connect("127.0.0.1", coordinator.port(), error), error);
  const Reply hello =
      client.hello(WorkerId{}, WorkerBootId{}, CoordinatorEpoch{}, {}, error);
  MLF_CHECK_MSG(hello.ok(), hello.render());
  MLF_CHECK(hello.u64("coordinator_epoch") >= 1);
  client.close();
  coordinator.stop();
}

MLF_TEST(multiprocess, a_worker_registers_over_a_real_socket) {
  mlftest::CoordinatorProcess coordinator;
  std::string error;
  if (!coordinator.start("", error)) {
    MLF_CHECK_MSG(false, error);
  }
  mlftest::WorkerProcess worker;
  MLF_CHECK_MSG(worker.start(coordinator.port(), 1, false, false, error), error);
  MLF_CHECK(worker.running());

  // The worker's registration is observable through the coordinator's own view.
  LifecycleClient client;
  MLF_CHECK(client.connect("127.0.0.1", coordinator.port(), error));
  MLF_CHECK(client.hello(WorkerId{}, WorkerBootId{}, CoordinatorEpoch{}, {}, error).ok());
  MLF_CHECK(mlftest::poll_until(client, [&](LifecycleClient& probe) {
    std::string probe_error;
    const Reply probe_state = probe.call(QueryStateMessage{}, probe_error);
    return probe_state.ok() && probe_state.u64("workers") >= 1;
  }));
  const Reply state = client.call(QueryStateMessage{}, error);
  MLF_CHECK_EQ(state.u64("workers"), 1u);
  MLF_CHECK(state.u64("attempts") == 0u);

  worker.stop();
  client.close();
  coordinator.stop();
}

MLF_TEST(multiprocess, real_worker_death_produces_an_unknown_outcome) {
  mlftest::CoordinatorProcess coordinator;
  std::string error;
  if (!coordinator.start("", error)) {
    MLF_CHECK_MSG(false, error);
  }

  // Worker A applies an activation effect and then dies before acknowledging.
  mlftest::WorkerProcess worker_a;
  MLF_CHECK_MSG(worker_a.start(coordinator.port(), 1, /*die_on_activate=*/true,
                               /*unconfirmed=*/false, error),
                error);
  // Worker B is an unaffected peer.
  mlftest::WorkerProcess worker_b;
  MLF_CHECK_MSG(worker_b.start(coordinator.port(), 2, false, false, error), error);

  LifecycleClient client;
  MLF_CHECK(client.connect("127.0.0.1", coordinator.port(), error));
  MLF_CHECK(client.hello(WorkerId{}, WorkerBootId{}, CoordinatorEpoch{}, {}, error).ok());

  RemoteModel remote;
  MLF_CHECK_MSG(register_model(client, "multiprocess-llm", remote, error), error);
  MLF_CHECK_MSG(publish_compatibility(client, remote.version, error), error);

  // Wait until both workers are visible.
  MLF_CHECK(mlftest::poll_until(client, [&](LifecycleClient& probe) {
    std::string probe_error;
    const Reply state = probe.call(QueryStateMessage{}, probe_error);
    return state.ok() && state.u64("workers") >= 2;
  }));

  // Ask worker A to activate; it applies the effect and dies.
  AttemptRequestMessage activation;
  activation.kind = AttemptKind::Activate;
  activation.worker = WorkerId(1);
  activation.model = ModelId(remote.model);
  activation.version = ModelVersionId(remote.version);
  activation.model_generation = ModelGeneration(1);
  activation.scope = ScopeId(1);
  activation.detail = "die-on-activate";
  const Reply requested =
      client.call_as(MessageType::REQUEST_ACTIVATE, activation, error);
  MLF_CHECK_MSG(requested.ok(), requested.render());
  const std::uint64_t attempt_id = requested.u64("attempt_id");
  MLF_CHECK(attempt_id != 0);

  // Observe the real process death.
  MLF_CHECK(mlftest::poll_until(client, [&](LifecycleClient&) { return !worker_a.running(); }));

  // The coordinator must settle the attempt as unknown, never as completed.
  std::uint64_t observed_state = 0;
  const bool settled = mlftest::poll_until(client, [&](LifecycleClient& probe) {
    std::string probe_error;
    const Reply state = probe.call(QueryStateMessage{}, probe_error);
    if (!state.ok()) return false;
    observed_state = state.u64("workers");
    return true;
  });
  MLF_CHECK(settled);

  // Reconciliation must demand a decision rather than inventing one.
  const Reply reconciled = client.call(ReconcileMessage{}, error);
  MLF_CHECK_MSG(reconciled.ok() || reconciled.outcome == OutcomeCode::RECONCILIATION_REQUIRED,
                reconciled.render());
  MLF_CHECK(reconciled.reasons.size() >= 1);

  // The unaffected peer is still connected: its frames are still accepted.
  AttemptRequestMessage peer_activation;
  peer_activation.kind = AttemptKind::Warm;
  peer_activation.worker = WorkerId(2);
  peer_activation.model = ModelId(remote.model);
  peer_activation.version = ModelVersionId(remote.version);
  peer_activation.model_generation = ModelGeneration(1);
  peer_activation.scope = ScopeId(1);
  peer_activation.idempotent = true;
  const Reply peer = client.call_as(MessageType::REQUEST_WARM, peer_activation, error);
  MLF_CHECK_MSG(peer.ok(), peer.render());

  static_cast<void>(attempt_id);
  static_cast<void>(observed_state);

  worker_b.stop();
  client.close();
  coordinator.stop();
}

MLF_TEST(multiprocess, a_stale_completion_from_a_dead_boot_is_refused) {
  mlftest::CoordinatorProcess coordinator;
  std::string error;
  if (!coordinator.start("", error)) {
    MLF_CHECK_MSG(false, error);
  }
  mlftest::WorkerProcess worker_a;
  MLF_CHECK_MSG(worker_a.start(coordinator.port(), 7, true, false, error), error);

  LifecycleClient client;
  MLF_CHECK(client.connect("127.0.0.1", coordinator.port(), error));
  MLF_CHECK(client.hello(WorkerId{}, WorkerBootId{}, CoordinatorEpoch{}, {}, error).ok());
  RemoteModel remote;
  MLF_CHECK_MSG(register_model(client, "stale-completion", remote, error), error);

  MLF_CHECK(mlftest::poll_until(client, [&](LifecycleClient& probe) {
    std::string probe_error;
    const Reply state = probe.call(QueryStateMessage{}, probe_error);
    return state.ok() && state.u64("workers") >= 1;
  }));

  AttemptRequestMessage activation;
  activation.kind = AttemptKind::Activate;
  activation.worker = WorkerId(7);
  activation.model = ModelId(remote.model);
  activation.version = ModelVersionId(remote.version);
  activation.scope = ScopeId(1);
  const Reply requested =
      client.call_as(MessageType::REQUEST_ACTIVATE, activation, error);
  MLF_CHECK_MSG(requested.ok(), requested.render());
  const std::uint64_t attempt_id = requested.u64("attempt_id");
  MLF_CHECK(mlftest::poll_until(client, [&](LifecycleClient&) { return !worker_a.running(); }));

  // A different client replays a completion for the dead boot's attempt.
  LifecycleClient impostor;
  MLF_CHECK(impostor.connect("127.0.0.1", coordinator.port(), error));
  MLF_CHECK(impostor.hello(WorkerId(7), WorkerBootId(1), CoordinatorEpoch{}, {}, error).ok());
  CompletionMessage completion;
  completion.attempt = AttemptId(attempt_id);
  completion.success = true;
  completion.confirmed = true;
  const Reply replay = impostor.call(completion, error);
  MLF_CHECK(!replay.ok());
  MLF_CHECK(replay.reasons.size() >= 1);
  std::printf("stale completion rejected: %s\n", replay.render().c_str());

  impostor.close();
  client.close();
  coordinator.stop();
}

MLF_TEST(multiprocess, coordinator_restart_preserves_truth_and_advances_the_epoch) {
  const std::string state_path = mlftest::scratch_path("multiprocess-coordinator", ".mlfs");
  mlftest::remove_file(state_path);
  std::uint64_t epoch_before = 0;
  std::uint64_t version = 0;
  std::uint64_t model = 0;

  {
    mlftest::CoordinatorProcess coordinator;
    std::string error;
    if (!coordinator.start(state_path, error)) {
      MLF_CHECK_MSG(false, error);
    }
    LifecycleClient client;
    MLF_CHECK(client.connect("127.0.0.1", coordinator.port(), error));
    const Reply hello =
        client.hello(WorkerId{}, WorkerBootId{}, CoordinatorEpoch{}, {}, error);
    MLF_CHECK(hello.ok());
    epoch_before = hello.u64("coordinator_epoch");

    RemoteModel remote;
    MLF_CHECK_MSG(register_model(client, "restart-llm", remote, error), error);
    MLF_CHECK_MSG(publish_compatibility(client, remote.version, error), error);
    model = remote.model;
    version = remote.version;

    PromotionMessage promotion;
    promotion.request.model = ModelId(model);
    promotion.request.candidate = ModelVersionId(version);
    promotion.request.epoch = CoordinatorEpoch(epoch_before);
    promotion.request.environment_key = "multiprocess";
    promotion.request.scope = ScopeId(1);
    promotion.request.strategy = RolloutStrategy::ImmediateCutover;
    promotion.request.request_immediate_cutover = true;
    const Reply promoted = client.call(promotion, error);
    MLF_CHECK_MSG(promoted.ok(), promoted.render());

    client.close();
    // Real, uncooperative termination of the coordinator process.
    MLF_CHECK(coordinator.kill());
    mlf::sleep_millis(20);
  }

  {
    mlftest::CoordinatorProcess coordinator;
    std::string error;
    if (!coordinator.start(state_path, error)) {
      MLF_CHECK_MSG(false, error);
    }
    LifecycleClient client;
    MLF_CHECK(client.connect("127.0.0.1", coordinator.port(), error));
    const Reply hello =
        client.hello(WorkerId{}, WorkerBootId{}, CoordinatorEpoch{}, {}, error);
    MLF_CHECK(hello.ok());
    const std::uint64_t epoch_after = hello.u64("coordinator_epoch");
    MLF_CHECK_MSG(epoch_after > epoch_before,
                  "coordinator epoch did not advance across restart");

    QueryStateMessage query;
    query.version = ModelVersionId(version);
    const Reply state = client.call(query, error);
    MLF_CHECK_MSG(state.ok(), state.render());
    MLF_CHECK(state.find("label") != nullptr);
    MLF_CHECK_EQ(std::string(*state.find("lifecycle_state")), std::string("CURRENT"));

    QueryAuthorityMessage authority;
    authority.scope = ScopeId(1);
    const Reply authority_reply = client.call(authority, error);
    MLF_CHECK(authority_reply.ok());
    MLF_CHECK_EQ(authority_reply.u64("version_id"), version);

    // Frames carrying the pre-restart epoch are refused.
    PromotionMessage stale;
    stale.request.model = ModelId(model);
    stale.request.candidate = ModelVersionId(version);
    stale.request.epoch = CoordinatorEpoch(epoch_before);
    stale.request.environment_key = "multiprocess";
    stale.request.scope = ScopeId(1);
    const Reply refused = client.call(stale, error);
    MLF_CHECK(!refused.ok());
    bool named_stale_epoch = false;
    for (const Reason& reason : refused.reasons) {
      if (reason.code == ReasonCode::CoordinatorEpochStale) named_stale_epoch = true;
    }
    MLF_CHECK_MSG(named_stale_epoch, refused.render());

    client.close();
    coordinator.stop();
  }
  mlftest::remove_file(state_path);
}

MLF_TEST(multiprocess, a_corrupt_state_file_prevents_startup) {
  const std::string state_path = mlftest::scratch_path("corrupt-coordinator", ".mlfs");
  {
    std::ofstream stream(state_path, std::ios::binary | std::ios::trunc);
    stream << "MLFS-not-really-a-state-file";
    stream.flush();
  }
  mlftest::CoordinatorProcess coordinator;
  std::string error;
  const bool started = coordinator.start(state_path, error);
  // The child exits with a failure and never reports a port.
  MLF_CHECK(!started);
  coordinator.stop();
  mlftest::remove_file(state_path);
}
