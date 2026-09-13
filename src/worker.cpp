#include "mlf/worker.hpp"

#include <cstdlib>

#include "mlf/process.hpp"

namespace mlf {
namespace {

/// Boot identity: unique per process incarnation. Built from the process id, a
/// monotonic clock reading and a per-process counter so that two boots of the
/// same worker never collide.
WorkerBootId make_boot_id() {
  static std::uint64_t counter = 0;
  ++counter;
  const std::uint64_t mixed = (current_process_id() * 1000003ull) ^
                              (monotonic_millis() * 31ull) ^ (counter * 7919ull);
  // Never zero: an invalid boot id would be indistinguishable from "unspecified".
  return WorkerBootId(mixed == 0 ? 1u : mixed);
}

/// Rebuild an evidence message from a synthetic replica observation.
PublishEvidenceMessage readiness_for(const SyntheticReplica& replica, EvidenceKind kind) {
  PublishEvidenceMessage message;
  message.version = replica.version;
  message.model_generation = replica.model_generation;
  message.artifact_generation = replica.artifact_generation;
  message.scope = replica.scope;
  message.kind = kind;
  message.verdict = (replica.ready && replica.healthy) ? EvidenceVerdict::Satisfied
                                                       : EvidenceVerdict::Unsatisfied;
  message.value = (replica.ready && replica.healthy) ? 1.0 : 0.0;
  message.unit = "boolean";
  message.tick = 0;
  message.detail = kind == EvidenceKind::Readiness
                       ? "synthetic replica reports ready"
                       : "synthetic replica reports resident";
  message.provenance = Provenance::Synthetic;
  return message;
}

}  // namespace

LifecycleWorker::LifecycleWorker(WorkerConfig config)
    : config_(std::move(config)), boot_(make_boot_id()) {}

LifecycleWorker::~LifecycleWorker() { stop(); }

bool LifecycleWorker::send_frame(MessageType type, const std::vector<std::uint8_t>& payload,
                                 std::string& error) {
  Frame frame = make_frame(type, payload);
  const ProtocolStatus status = channel_.send(frame, error);
  if (status != ProtocolStatus::Ok) {
    error = std::string("send failed: ") + std::string(to_string(status));
    return false;
  }
  ++stats_.frames_sent;
  return true;
}

bool LifecycleWorker::send_reply(const Reply& reply, std::string& error) {
  std::vector<std::uint8_t> payload;
  const ProtocolStatus encoded = encode(reply, config_.limits, payload);
  if (encoded != ProtocolStatus::Ok) {
    error = "reply could not be encoded";
    return false;
  }
  ++stats_.replies_sent;
  return send_frame(MessageType::REPLY, payload, error);
}

bool LifecycleWorker::start(std::string& error) {
  std::uint32_t attempt = 0;
  do {
    ++attempt;
    Socket socket;
    ProtocolStatus status = Socket::connect_to(config_.coordinator_host, config_.coordinator_port,
                                               socket, error);
    if (status == ProtocolStatus::Ok) {
      channel_ = FrameChannel(std::move(socket), config_.limits);
      break;
    }
    if (attempt < config_.connect_attempts) {
      sleep_millis(config_.connect_backoff_millis);
    }
  } while (attempt < config_.connect_attempts);
  if (!channel_.valid()) return false;

  backend_.set_owner(config_.worker_id, boot_, CoordinatorEpoch{});

  HelloMessage hello;
  hello.protocol_version = config_.limits.version;
  hello.worker = config_.worker_id;
  hello.boot = boot_;
  hello.epoch = CoordinatorEpoch{};
  hello.scopes = config_.scopes;
  hello.detail = "worker boot " + to_string(boot_);
  std::vector<std::uint8_t> payload;
  if (encode(hello, config_.limits, payload) != ProtocolStatus::Ok) {
    error = "HELLO could not be encoded";
    return false;
  }
  if (!send_frame(MessageType::HELLO, payload, error)) return false;

  Frame reply_frame;
  const ProtocolStatus received = channel_.receive(reply_frame, error);
  if (received != ProtocolStatus::Ok || reply_frame.type != MessageType::REPLY) {
    error = "no HELLO_ACK reply from the coordinator";
    return false;
  }
  Reply reply;
  if (decode(reply_frame.payload.data(), reply_frame.payload.size(), config_.limits, reply) !=
      ProtocolStatus::Ok) {
    error = "HELLO_ACK could not be decoded";
    return false;
  }
  if (!reply.ok()) {
    error = "coordinator rejected the handshake: " + reply.render();
    return false;
  }
  epoch_ = CoordinatorEpoch(reply.u64("coordinator_epoch"));
  backend_.set_owner(config_.worker_id, boot_, epoch_);

  // A worker must be registered explicitly before it may act.
  std::vector<std::uint8_t> registration_payload;
  HelloMessage registration_request = hello;
  registration_request.epoch = epoch_;
  registration_request.detail = "worker registration";
  if (encode(registration_request, config_.limits, registration_payload) != ProtocolStatus::Ok) {
    error = "REGISTER_WORKER could not be encoded";
    return false;
  }
  if (!send_frame(MessageType::REGISTER_WORKER, registration_payload, error)) return false;

  Frame ack_frame;
  if (channel_.receive(ack_frame, error) != ProtocolStatus::Ok) {
    error = "no registration acknowledgement from the coordinator";
    return false;
  }
  Reply registration;
  if (decode(ack_frame.payload.data(), ack_frame.payload.size(), config_.limits, registration) !=
      ProtocolStatus::Ok) {
    error = "registration reply could not be decoded";
    return false;
  }
  if (!registration.ok()) {
    error = "coordinator refused worker registration: " + registration.render();
    return false;
  }
  return publish_observations(error);
}

void LifecycleWorker::stop() {
  stopping_ = true;
  if (channel_.valid()) {
    channel_.close();
  }
}

bool LifecycleWorker::publish_observations(std::string& error) {
  const std::vector<SyntheticReplica> replicas = backend_.raw_replicas();
  for (const SyntheticReplica& replica : replicas) {
    if (!replica.resident) continue;
    for (EvidenceKind kind : {EvidenceKind::Readiness, EvidenceKind::ResidencyReady}) {
      PublishEvidenceMessage message = readiness_for(replica, kind);
      std::vector<std::uint8_t> payload;
      if (encode(message, config_.limits, payload) != ProtocolStatus::Ok) continue;
      const MessageType type = kind == EvidenceKind::Readiness ? MessageType::PUBLISH_READINESS
                                                               : MessageType::PUBLISH_HEALTH;
      if (!send_frame(type, payload, error)) return false;
      Frame reply_frame;
      if (channel_.receive(reply_frame, error) != ProtocolStatus::Ok) return false;
      Reply reply;
      if (decode(reply_frame.payload.data(), reply_frame.payload.size(), config_.limits, reply) ==
              ProtocolStatus::Ok &&
          reply.ok()) {
        ++stats_.evidence_published;
      }
    }
  }
  return true;
}

bool LifecycleWorker::handle_command(const CommandMessage& command, std::string& error) {
  if (command.kind == AttemptKind::Activate && config_.die_on_activate) {
    // Apply the effect, then die without acknowledging. The coordinator must
    // learn the outcome through reconciliation, not through a guess.
    const CommandResult applied = backend_.execute(command);
    if (!applied.effect_applied) {
      error = "activation effect was not applied before simulated death";
    }
    std::_Exit(97);
  }

  const CommandResult result = backend_.execute(command);
  ++stats_.commands_executed;

  CompletionMessage completion;
  completion.attempt = command.attempt;
  completion.success = result.decision.allowed() && result.effect_applied;
  completion.confirmed = !config_.unconfirmed_completions && !backend_.ambiguous_completion();
  completion.detail = result.detail;
  std::vector<std::uint8_t> payload;
  if (encode(completion, config_.limits, payload) != ProtocolStatus::Ok) {
    error = "completion could not be encoded";
    return false;
  }
  if (!send_frame(MessageType::PUBLISH_COMPLETION, payload, error)) return false;

  Frame reply_frame;
  if (channel_.receive(reply_frame, error) != ProtocolStatus::Ok) return false;
  Reply reply;
  if (decode(reply_frame.payload.data(), reply_frame.payload.size(), config_.limits, reply) !=
      ProtocolStatus::Ok) {
    error = "completion reply could not be decoded";
    return false;
  }
  if (!reply.ok()) {
    error = "coordinator refused the completion: " + reply.render();
  }
  // Whatever the outcome, the worker re-publishes the readiness it now observes.
  return publish_observations(error);
}

bool LifecycleWorker::handle_frame(const Frame& frame, std::string& error) {
  ++stats_.frames_received;
  switch (frame.type) {
    case MessageType::COMMAND:
    case MessageType::REQUEST_WARM:
    case MessageType::REQUEST_ACTIVATE:
    case MessageType::REQUEST_DRAIN:
    case MessageType::REQUEST_ROLLBACK: {
      CommandMessage command;
      if (decode(frame.payload.data(), frame.payload.size(), config_.limits, command) !=
          ProtocolStatus::Ok) {
        ++stats_.protocol_rejections;
        Reply reply;
        reply.outcome = OutcomeCode::Rejected;
        reply.add(ReasonCode::ProtocolDecodeFailure, "command frame could not be decoded");
        return send_reply(reply, error);
      }
      return handle_command(command, error);
    }
    case MessageType::FENCE: {
      FenceMessage fence;
      if (decode(frame.payload.data(), frame.payload.size(), config_.limits, fence) !=
          ProtocolStatus::Ok) {
        ++stats_.protocol_rejections;
        return true;
      }
      Reply reply;
      if (fence.worker != config_.worker_id || fence.boot != boot_) {
        reply.outcome = OutcomeCode::Rejected;
        reply.add(ReasonCode::WorkerBootStale, "fence targets a different boot");
        return send_reply(reply, error);
      }
      reply.outcome = OutcomeCode::Accepted;
      reply.set("fenced", "yes");
      if (!send_reply(reply, error)) return false;
      // A fenced boot stops voluntarily; the coordinator also closes the socket.
      stopping_ = true;
      channel_.close();
      return true;
    }
    default: {
      ++stats_.protocol_rejections;
      Reply reply;
      reply.outcome = OutcomeCode::Rejected;
      reply.add(ReasonCode::ProtocolUnknownType, std::string(to_string(frame.type)));
      return send_reply(reply, error);
    }
  }
}

bool LifecycleWorker::serve_one(std::string& error) {
  if (!channel_.valid()) {
    error = "worker is not connected";
    return false;
  }
  Frame frame;
  const ProtocolStatus status = channel_.receive(frame, error);
  if (status == ProtocolStatus::Closed) {
    error = "coordinator closed the connection";
    return false;
  }
  if (status != ProtocolStatus::Ok) {
    ++stats_.protocol_rejections;
    // A framing violation is fatal for the connection: continuing would mean
    // interpreting an untrusted byte stream.
    error = std::string("protocol violation: ") + std::string(to_string(status));
    channel_.close();
    return false;
  }
  return handle_frame(frame, error);
}

bool LifecycleWorker::run(std::string& error) {
  while (!stopping_) {
    if (!serve_one(error)) return false;
  }
  return true;
}

}  // namespace mlf
