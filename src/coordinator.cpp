#include "mlf/coordinator.hpp"

#include <algorithm>
#include <cstring>

#include "mlf/state_store.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#else
#include <sys/select.h>
#include <sys/socket.h>
#endif

namespace mlf {
namespace {

std::string scope_path(const LifecycleEngine& engine, ScopeId scope) {
  for (const ScopeRecord& record : engine.scopes()) {
    if (record.id == scope) return record.path;
  }
  return {};
}

Reply rejected(OutcomeCode outcome, ReasonCode code, std::string_view detail) {
  Reply reply;
  reply.outcome = outcome;
  reply.add(code, std::string(detail));
  return reply;
}

}  // namespace

struct LifecycleCoordinator::Session {
  std::uint64_t id{0};
  Socket socket{};
  FrameDecoder decoder{};
  WorkerId worker{};
  WorkerBootId boot{};
  CoordinatorEpoch epoch{};
  bool hello_seen{false};
  bool registered{false};
  std::vector<std::uint8_t> scratch{};
};

LifecycleCoordinator::LifecycleCoordinator(CoordinatorConfig config) : config_(std::move(config)) {
  engine_ = std::make_unique<LifecycleEngine>(config_.engine);
}

LifecycleCoordinator::~LifecycleCoordinator() { stop(); }

bool LifecycleCoordinator::start(std::string& error) {
  if (running_) return true;

  // --- recovery -----------------------------------------------------------
  if (!config_.state_path.empty()) {
    DurableState loaded;
    const DecodeLimits limits = DecodeLimits::from_bounds(config_.engine.bounds);
    const StateStoreResult result = load_durable_state(config_.state_path, limits, loaded);
    if (result.ok()) {
      const CoordinatorEpoch next_epoch = next_generation(loaded.epoch);
      Decision restored = engine_->import_durable_state(loaded, next_epoch);
      if (!restored.allowed()) {
        error = "durable state was rejected: " + restored.render();
        return false;
      }
      engine_->invalidate_volatile_state("coordinator restart");
      ++stats_.recoveries;
    } else if (result.status == StateStoreStatus::IoError) {
      // No state file yet: this is a first start.
    } else if (config_.refuse_start_on_corrupt_state) {
      error = "durable state could not be loaded (" +
              std::string(to_string(result.status)) + "): " + result.detail;
      return false;
    }
  }

  // --- control plane ------------------------------------------------------
  const ProtocolStatus bound =
      Socket::listen_on(config_.bind_address, config_.port, 64, listener_, error);
  if (bound != ProtocolStatus::Ok) {
    error = std::string("coordinator could not bind: ") + error;
    return false;
  }
  port_ = listener_.local_port();
  // Accepting must never block: the accept loop drains every pending connection
  // and then returns, so a poll iteration cannot wedge the control plane.
  std::string blocking_error;
  const ProtocolStatus nonblocking = listener_.set_blocking(false, blocking_error);
  if (nonblocking != ProtocolStatus::Ok) {
    error = "listener could not be made non-blocking: " + blocking_error;
    return false;
  }
  running_ = true;
  last_saved_sequence_ = engine_->sequence();
  return true;
}

void LifecycleCoordinator::stop() {
  running_ = false;
  for (auto& session : sessions_) {
    if (session != nullptr) session->socket.close();
  }
  sessions_.clear();
  if (listener_.valid()) listener_.close();
}

std::size_t LifecycleCoordinator::connection_count() const noexcept {
  std::size_t count = 0;
  for (const auto& session : sessions_) {
    if (session != nullptr && session->socket.valid()) ++count;
  }
  return count;
}

LifecycleCoordinator::Session* LifecycleCoordinator::session_for_worker(WorkerId worker,
                                                                       WorkerBootId boot) {
  if (!worker.valid()) return nullptr;
  for (auto& session : sessions_) {
    if (session == nullptr) continue;
    if (!session->socket.valid()) continue;
    if (session->worker != worker) continue;
    if (boot.valid() && session->boot != boot) continue;
    return session.get();
  }
  return nullptr;
}

bool LifecycleCoordinator::send_frame(Session& session, MessageType type,
                                      const std::vector<std::uint8_t>& payload) {
  std::string error;
  const ProtocolStatus status = session.socket.valid()
                                    ? [&]() {
                                        std::vector<std::uint8_t> encoded;
                                        const ProtocolStatus encoded_status =
                                            encode_frame(make_frame(type, payload),
                                                         config_.limits, encoded);
                                        if (encoded_status != ProtocolStatus::Ok) {
                                          return encoded_status;
                                        }
                                        return session.socket.send_all(encoded.data(),
                                                                       encoded.size(), error);
                                      }()
                                    : ProtocolStatus::Closed;
  if (status != ProtocolStatus::Ok) {
    ++stats_.frames_rejected;
    return false;
  }
  ++stats_.frames_sent;
  return true;
}

bool LifecycleCoordinator::send_reply(Session& session, const Reply& reply) {
  // A mutation is acknowledged only after it is durable. A client that observes
  // success and immediately loses the coordinator must still find the change.
  if (pending_durable_change_) {
    pending_durable_change_ = false;
    std::string persist_error;
    if (!persist_if_changed(persist_error)) {
      ++stats_.frames_rejected;
      Reply failure;
      failure.outcome = OutcomeCode::Rejected;
      failure.add(ReasonCode::TransactionRolledBack,
                  "the change was applied in memory but could not be made durable");
      std::vector<std::uint8_t> failure_payload;
      if (encode(failure, config_.limits, failure_payload) == ProtocolStatus::Ok) {
        static_cast<void>(send_frame(session, MessageType::REPLY, failure_payload));
      }
      return false;
    }
  }
  std::vector<std::uint8_t> payload;
  const ProtocolStatus encoded = encode(reply, config_.limits, payload);
  if (encoded != ProtocolStatus::Ok) {
    ++stats_.frames_rejected;
    return false;
  }
  return send_frame(session, MessageType::REPLY, payload);
}

void LifecycleCoordinator::accept_ready() {
  for (;;) {
    Socket accepted;
    std::string error;
    const ProtocolStatus status = listener_.accept(accepted, error);
    if (status != ProtocolStatus::Ok) return;
    // Sessions are serviced synchronously, so they must block.
    std::string blocking_error;
    if (accepted.set_blocking(true, blocking_error) != ProtocolStatus::Ok) {
      accepted.close();
      ++stats_.frames_rejected;
      continue;
    }
    if (sessions_.size() >= config_.limits.max_connections) {
      // Bounded connections: excess peers are closed immediately rather than
      // queued without limit.
      accepted.close();
      ++stats_.frames_rejected;
      continue;
    }
    auto session = std::make_unique<Session>();
    session->id = sessions_.size() + 1;
    session->socket = std::move(accepted);
    session->decoder = FrameDecoder(config_.limits);
    sessions_.push_back(std::move(session));
    ++stats_.connections_accepted;
  }
}

void LifecycleCoordinator::fence_session(Session& session, std::string_view reason) {
  if (session.worker.valid() && session.boot.valid()) {
    std::string detail_text(reason);
    Decision fenced = engine_->fence_worker(session.worker, session.boot, session.epoch, detail_text);
    if (fenced.allowed()) ++stats_.worker_fences;
  }
  session.socket.close();
  session.registered = false;
  ++stats_.sessions_closed;
}

void LifecycleCoordinator::close_session(Session& session, std::string_view reason) {
  fence_session(session, reason);
}

void LifecycleCoordinator::service_session(Session& session) {
  if (!session.socket.valid()) return;
  session.scratch.resize(64 * 1024);
  std::size_t received = 0;
  std::string error;
  const ProtocolStatus read =
      session.socket.recv_some(session.scratch.data(), session.scratch.size(), received, error);
  if (read == ProtocolStatus::Closed) {
    close_session(session, "peer closed the connection");
    return;
  }
  if (read != ProtocolStatus::Ok) {
    close_session(session, "read error");
    return;
  }
  const ProtocolStatus fed = session.decoder.feed(session.scratch.data(), received);
  if (fed != ProtocolStatus::Ok) {
    ++stats_.frames_rejected;
    Reply reply = rejected(outcome_for(fed), reason_for(fed), to_string(fed));
    static_cast<void>(send_reply(session, reply));
    close_session(session, "framing violation");
    return;
  }
  for (;;) {
    Frame frame;
    const ProtocolStatus next = session.decoder.next(frame);
    if (next == ProtocolStatus::NeedMoreData) break;
    if (next != ProtocolStatus::Ok) {
      ++stats_.frames_rejected;
      Reply reply = rejected(outcome_for(next), reason_for(next), to_string(next));
      static_cast<void>(send_reply(session, reply));
      close_session(session, "framing violation");
      return;
    }
    ++stats_.frames_received;
    handle_frame(session, frame, error);
    if (!session.socket.valid()) return;
  }
}

bool LifecycleCoordinator::poll_once(std::string& error) {
  if (!running_) {
    error = "coordinator is not running";
    return false;
  }
  fd_set read_set;
  FD_ZERO(&read_set);
#ifdef _WIN32
  FD_SET(static_cast<SOCKET>(listener_.native_handle()), &read_set);
  SOCKET max_handle = static_cast<SOCKET>(listener_.native_handle());
  for (auto& session : sessions_) {
    if (session == nullptr || !session->socket.valid()) continue;
    const auto handle = static_cast<SOCKET>(session->socket.native_handle());
    FD_SET(handle, &read_set);
    if (handle > max_handle) max_handle = handle;
  }
#else
  int max_handle = static_cast<int>(listener_.native_handle());
  FD_SET(static_cast<int>(listener_.native_handle()), &read_set);
  for (auto& session : sessions_) {
    if (session == nullptr || !session->socket.valid()) continue;
    const int handle = static_cast<int>(session->socket.native_handle());
    FD_SET(handle, &read_set);
    if (handle > max_handle) max_handle = handle;
  }
#endif
  timeval wait{};
  wait.tv_sec = config_.poll_timeout_millis / 1000;
  wait.tv_usec = (config_.poll_timeout_millis % 1000) * 1000;
  const int ready = ::select(static_cast<int>(max_handle) + 1, &read_set, nullptr, nullptr, &wait);
  if (ready < 0) {
    error = "select() failed";
    return false;
  }
  if (ready > 0) {
#ifdef _WIN32
    if (FD_ISSET(static_cast<SOCKET>(listener_.native_handle()), &read_set) != 0) accept_ready();
#else
    if (FD_ISSET(static_cast<int>(listener_.native_handle()), &read_set) != 0) accept_ready();
#endif
    for (auto& session : sessions_) {
      if (session == nullptr || !session->socket.valid()) continue;
#ifdef _WIN32
      if (FD_ISSET(static_cast<SOCKET>(session->socket.native_handle()), &read_set) == 0) continue;
#else
      if (FD_ISSET(static_cast<int>(session->socket.native_handle()), &read_set) == 0) continue;
#endif
      service_session(*session);
    }
  }

  sessions_.erase(std::remove_if(sessions_.begin(), sessions_.end(),
                                 [](const std::unique_ptr<Session>& session) {
                                   return session == nullptr || !session->socket.valid();
                                 }),
                  sessions_.end());

  // dispatch_pending reports how many commands were sent, which may legitimately
  // be zero; it is not a success flag.
  static_cast<void>(dispatch_pending(error));
  return persist_if_changed(error);
}

bool LifecycleCoordinator::run(std::string& error) {
  while (running_ && !stop_requested_.load(std::memory_order_relaxed)) {
    if (!poll_once(error)) return false;
  }
  return true;
}

std::size_t LifecycleCoordinator::dispatch_pending(std::string& error) {
  static_cast<void>(error);
  std::size_t dispatched = 0;
  for (const Attempt& attempt : engine_->attempts()) {
    if (attempt.state != AttemptState::Dispatched) continue;
    if (std::find(dispatched_.begin(), dispatched_.end(), attempt.id) != dispatched_.end()) continue;
    Session* session = session_for_worker(attempt.worker, attempt.boot);
    if (session == nullptr) continue;
    CommandMessage command;
    command.kind = attempt.kind;
    command.attempt = attempt.id;
    command.model = attempt.model;
    command.version = attempt.version;
    command.model_generation = attempt.model_generation;
    command.artifact_generation = attempt.artifact_generation;
    command.scope = attempt.scope;
    command.rollout = attempt.rollout;
    command.rollout_generation = attempt.rollout_generation;
    command.detail = attempt.detail;
    std::vector<std::uint8_t> payload;
    if (encode(command, config_.limits, payload) != ProtocolStatus::Ok) {
      ++stats_.dispatch_backpressure;
      continue;
    }
    if (!send_frame(*session, MessageType::COMMAND, payload)) {
      // The outbound path is bounded: a failure here is accounted and the
      // attempt stays dispatched rather than being silently dropped.
      ++stats_.dispatch_backpressure;
      continue;
    }
    dispatched_.push_back(attempt.id);
    ++stats_.commands_dispatched;
    ++dispatched;
  }
  return dispatched;
}

bool LifecycleCoordinator::persist(std::string& error) {
  if (config_.state_path.empty()) return true;
  const std::shared_ptr<const DurableState> state = engine_->export_durable_state();
  const DecodeLimits limits = DecodeLimits::from_bounds(config_.engine.bounds);
  const StateStoreResult result = save_durable_state(*state, limits, config_.state_path);
  if (!result.ok()) {
    ++stats_.save_failures;
    error = std::string("state save failed (") + std::string(to_string(result.status)) +
            "): " + result.detail;
    return false;
  }
  ++stats_.saves;
  last_saved_sequence_ = engine_->sequence();
  return true;
}

bool LifecycleCoordinator::persist_if_changed(std::string& error) {
  if (config_.state_path.empty() || !config_.persist_on_change) return true;
  if (engine_->sequence() == last_saved_sequence_) return true;
  return persist(error);
}

// ---------------------------------------------------------------------------
// Frame handling
// ---------------------------------------------------------------------------

void LifecycleCoordinator::handle_hello(Session& session, const Frame& frame) {
  HelloMessage hello;
  if (decode(frame.payload.data(), frame.payload.size(), config_.limits, hello) !=
      ProtocolStatus::Ok) {
    ++stats_.frames_rejected;
    static_cast<void>(send_reply(session, rejected(OutcomeCode::Rejected,
                                                   ReasonCode::ProtocolDecodeFailure,
                                                   "HELLO could not be decoded")));
    return;
  }
  if (session.hello_seen) {
    ++stats_.frames_rejected;
    static_cast<void>(send_reply(session, rejected(OutcomeCode::CONFLICT,
                                                   ReasonCode::ProtocolDuplicateHello,
                                                   "HELLO was already received on this connection")));
    return;
  }
  if (hello.protocol_version != config_.limits.version) {
    ++stats_.frames_rejected;
    static_cast<void>(send_reply(session, rejected(OutcomeCode::Rejected,
                                                   ReasonCode::ProtocolUnsupportedVersion,
                                                   "protocol version is not supported")));
    close_session(session, "unsupported protocol version");
    return;
  }
  session.hello_seen = true;
  session.worker = hello.worker;
  session.boot = hello.boot;
  session.epoch = engine_->epoch();

  Reply reply;
  reply.outcome = OutcomeCode::Accepted;
  reply.set("coordinator_epoch", engine_->epoch().raw());
  reply.set("protocol_version", static_cast<std::uint64_t>(config_.limits.version));
  reply.set("role", hello.worker.valid() ? "worker" : "client");
  static_cast<void>(send_reply(session, reply));
}

void LifecycleCoordinator::handle_register_worker(Session& session, const Frame& frame) {
  HelloMessage registration;
  if (decode(frame.payload.data(), frame.payload.size(), config_.limits, registration) !=
      ProtocolStatus::Ok) {
    ++stats_.frames_rejected;
    static_cast<void>(send_reply(session, rejected(OutcomeCode::Rejected,
                                                   ReasonCode::ProtocolDecodeFailure,
                                                   "registration could not be decoded")));
    return;
  }
  if (!session.hello_seen) {
    static_cast<void>(send_reply(session, rejected(OutcomeCode::Rejected,
                                                   ReasonCode::ProtocolInvalidTransition,
                                                   "REGISTER_WORKER before HELLO")));
    return;
  }
  WorkerRegistration request;
  request.id = registration.worker;
  request.boot = registration.boot;
  request.epoch = engine_->epoch();
  request.scopes = registration.scopes;
  request.provenance = Provenance::Synthetic;
  Decision decision = engine_->register_worker(request);
  Reply reply = reply_from_decision(decision);
  if (decision.allowed()) {
    session.worker = registration.worker;
    session.boot = registration.boot;
    session.registered = true;
    reply.set("worker_id", registration.worker.raw());
    reply.set("worker_boot_id", registration.boot.raw());
    reply.set("coordinator_epoch", engine_->epoch().raw());
  }
  static_cast<void>(send_reply(session, reply));
}

void LifecycleCoordinator::handle_frame(Session& session, const Frame& frame, std::string& error) {
  static_cast<void>(error);
  // Frames that can change durable lifecycle state ask for a durable write before
  // their acknowledgement leaves the process.
  switch (frame.type) {
    case MessageType::REGISTER_WORKER:
    case MessageType::REGISTER_SCOPE:
    case MessageType::REGISTER_MODEL:
    case MessageType::REGISTER_VERSION:
    case MessageType::PUBLISH_COMPATIBILITY:
    case MessageType::PUBLISH_READINESS:
    case MessageType::PUBLISH_HEALTH:
    case MessageType::REQUEST_WARM:
    case MessageType::REQUEST_ACTIVATE:
    case MessageType::REQUEST_DRAIN:
    case MessageType::REQUEST_ROLLBACK:
    case MessageType::ACK_ATTEMPT:
    case MessageType::PUBLISH_COMPLETION:
    case MessageType::FENCE:
    case MessageType::PROMOTE:
    case MessageType::REGISTER_ROLLOUT_PLAN:
    case MessageType::BEGIN_ROLLOUT:
    case MessageType::ADVANCE_STAGE:
    case MessageType::FAIL_STAGE:
    case MessageType::RETIRE:
    case MessageType::REVALIDATE:
    case MessageType::ROLLBACK:
    case MessageType::SUPERSEDE_ROLLOUT:
      pending_durable_change_ = true;
      break;
    default:
      pending_durable_change_ = false;
      break;
  }

  switch (frame.type) {
    case MessageType::HELLO:
      handle_hello(session, frame);
      return;
    case MessageType::REGISTER_WORKER:
      handle_register_worker(session, frame);
      return;
    case MessageType::REGISTER_SCOPE: {
      RegisterScopeMessage message;
      if (decode(frame.payload.data(), frame.payload.size(), config_.limits, message) !=
          ProtocolStatus::Ok) {
        static_cast<void>(send_reply(session, rejected(OutcomeCode::Rejected,
                                                       ReasonCode::ProtocolDecodeFailure,
                                                       "REGISTER_SCOPE could not be decoded")));
        return;
      }
      ScopeId scope;
      Decision decision = engine_->register_scope(message.path, scope);
      Reply reply = reply_from_decision(decision);
      if (decision.allowed()) {
        reply.set("scope_id", scope.raw());
        reply.set("scope_path", message.path);
        const std::vector<ScopeRecord> scopes = engine_->scopes();
        for (const ScopeRecord& record : scopes) {
          if (record.id != scope) continue;
          reply.set("scope_generation", record.generation.raw());
          reply.set("scope_depth", static_cast<std::uint64_t>(record.depth));
          break;
        }
      }
      static_cast<void>(send_reply(session, reply));
      return;
    }
    case MessageType::REGISTER_MODEL: {
      RegisterModelMessage message;
      if (decode(frame.payload.data(), frame.payload.size(), config_.limits, message) !=
          ProtocolStatus::Ok) {
        static_cast<void>(send_reply(session, rejected(OutcomeCode::Rejected,
                                                       ReasonCode::ProtocolDecodeFailure,
                                                       "REGISTER_MODEL could not be decoded")));
        return;
      }
      ModelId model;
      Decision decision =
          engine_->register_model(message.name, message.family, message.provenance, model);
      Reply reply = reply_from_decision(decision);
      if (decision.allowed()) reply.set("model_id", model.raw());
      static_cast<void>(send_reply(session, reply));
      return;
    }
    case MessageType::REGISTER_VERSION: {
      RegisterVersionMessage message;
      if (decode(frame.payload.data(), frame.payload.size(), config_.limits, message) !=
          ProtocolStatus::Ok) {
        static_cast<void>(send_reply(session, rejected(OutcomeCode::Rejected,
                                                       ReasonCode::ProtocolDecodeFailure,
                                                       "REGISTER_VERSION could not be decoded")));
        return;
      }
      RegisterVersionRequest request;
      request.model = message.model;
      request.label = message.label;
      request.artifact = message.artifact;
      request.requirements = message.requirements;
      request.predecessor = message.predecessor;
      request.provenance = message.provenance;
      ModelVersionId version;
      Decision decision = engine_->register_version(request, version);
      Reply reply = reply_from_decision(decision);
      if (decision.allowed()) reply.set("version_id", version.raw());
      static_cast<void>(send_reply(session, reply));
      return;
    }
    case MessageType::PUBLISH_COMPATIBILITY: {
      PublishCompatibilityMessage message;
      if (decode(frame.payload.data(), frame.payload.size(), config_.limits, message) !=
          ProtocolStatus::Ok) {
        static_cast<void>(send_reply(session, rejected(OutcomeCode::Rejected,
                                                       ReasonCode::ProtocolDecodeFailure,
                                                       "PUBLISH_COMPATIBILITY is malformed")));
        return;
      }
      Decision decision = engine_->publish_compatibility(message.version, message.environment,
                                                         message.outcome, message.detail,
                                                         message.provenance);
      static_cast<void>(send_reply(session, reply_from_decision(decision)));
      return;
    }
    case MessageType::PUBLISH_READINESS:
    case MessageType::PUBLISH_HEALTH: {
      PublishEvidenceMessage message;
      if (decode(frame.payload.data(), frame.payload.size(), config_.limits, message) !=
          ProtocolStatus::Ok) {
        static_cast<void>(send_reply(session, rejected(OutcomeCode::Rejected,
                                                       ReasonCode::ProtocolDecodeFailure,
                                                       "evidence frame is malformed")));
        return;
      }
      if (frame.type == MessageType::PUBLISH_HEALTH) {
        message.kind = EvidenceKind::ResidencyReady;
      }
      EvidenceRecord record;
      record.kind = message.kind;
      record.verdict = message.verdict;
      record.subject.version = message.version;
      record.subject.model_generation = message.model_generation;
      record.subject.artifact_generation = message.artifact_generation;
      record.subject.scope = message.scope;
      record.subject.rollout = message.rollout;
      record.subject.stage = message.stage;
      record.provenance = message.provenance;
      record.value = message.value;
      record.unit = message.unit;
      record.tick = message.tick;
      record.detail = message.detail;
      record.worker = session.worker;
      record.boot = session.boot;
      record.epoch = session.epoch;
      Decision decision = engine_->publish_evidence(record);
      Reply reply = reply_from_decision(decision);
      if (decision.allowed()) {
        reply.set("evidence_id", record.id.raw());
        reply.set("evidence_generation", record.generation.raw());
      }
      static_cast<void>(send_reply(session, reply));
      return;
    }
    case MessageType::REQUEST_WARM:
    case MessageType::REQUEST_ACTIVATE:
    case MessageType::REQUEST_DRAIN:
    case MessageType::REQUEST_ROLLBACK: {
      AttemptRequestMessage message;
      if (decode(frame.payload.data(), frame.payload.size(), config_.limits, message) !=
          ProtocolStatus::Ok) {
        static_cast<void>(send_reply(session, rejected(OutcomeCode::Rejected,
                                                       ReasonCode::ProtocolDecodeFailure,
                                                       "request is malformed")));
        return;
      }
      switch (frame.type) {
        case MessageType::REQUEST_WARM:     message.kind = AttemptKind::Warm; break;
        case MessageType::REQUEST_ACTIVATE: message.kind = AttemptKind::Activate; break;
        case MessageType::REQUEST_DRAIN:    message.kind = AttemptKind::Drain; break;
        default:                            message.kind = AttemptKind::Rollback; break;
      }
      AttemptRequest request;
      request.kind = message.kind;
      // An explicit performer wins; otherwise the request is routed to the
      // requesting session's own worker lease.
      request.worker = message.worker.valid() ? message.worker : session.worker;
      request.boot = message.worker.valid() && !message.boot.valid()
                         ? WorkerBootId{}
                         : (message.worker.valid() ? message.boot : session.boot);
      if (message.worker.valid() && !request.boot.valid()) {
        if (Session* owner = session_for_worker(message.worker, WorkerBootId{})) {
          request.boot = owner->boot;
        }
      }
      request.epoch = session.epoch.valid() ? session.epoch : engine_->epoch();
      request.model = message.model;
      request.version = message.version;
      request.model_generation = message.model_generation;
      request.scope = message.scope;
      request.rollout = message.rollout;
      request.rollout_generation = message.rollout_generation;
      request.stage = message.stage;
      request.stage_generation = message.stage_generation;
      request.idempotent = message.idempotent;
      request.detail = message.detail;
      AttemptOutcome outcome = engine_->begin_attempt(request);
      Reply reply = reply_from_decision(outcome.decision);
      if (outcome.decision.allowed()) reply.set("attempt_id", outcome.attempt.raw());
      static_cast<void>(send_reply(session, reply));
      return;
    }
    case MessageType::PUBLISH_COMPLETION:
    case MessageType::ACK_ATTEMPT: {
      CompletionMessage message;
      if (decode(frame.payload.data(), frame.payload.size(), config_.limits, message) !=
          ProtocolStatus::Ok) {
        static_cast<void>(send_reply(session, rejected(OutcomeCode::Rejected,
                                                       ReasonCode::ProtocolDecodeFailure,
                                                       "completion is malformed")));
        return;
      }
      CompletionRecord completion;
      completion.attempt = message.attempt;
      completion.worker = session.worker;
      completion.boot = session.boot;
      completion.epoch = session.epoch;
      completion.success = message.success;
      completion.confirmed = message.confirmed;
      completion.detail = message.detail;
      Decision decision = engine_->complete_attempt(completion);
      static_cast<void>(send_reply(session, reply_from_decision(decision)));
      return;
    }
    case MessageType::QUERY_STATE: {
      QueryStateMessage message;
      if (decode(frame.payload.data(), frame.payload.size(), config_.limits, message) !=
          ProtocolStatus::Ok) {
        static_cast<void>(send_reply(session, rejected(OutcomeCode::Rejected,
                                                       ReasonCode::ProtocolDecodeFailure,
                                                       "QUERY_STATE is malformed")));
        return;
      }
      Reply reply;
      reply.outcome = OutcomeCode::Ok;
      reply.set("coordinator_epoch", engine_->epoch().raw());
      reply.set("sequence", engine_->sequence().raw());
      reply.set("snapshot_generation", engine_->snapshot_generation().raw());
      if (message.version.valid()) {
        for (const ModelVersionRecord& record : engine_->all_versions()) {
          if (record.version != message.version) continue;
          reply.set("model_id", record.model.raw());
          reply.set("version_id", record.version.raw());
          reply.set("label", record.label);
          reply.set("model_generation", record.generation.raw());
          reply.set("lifecycle_state", std::string(to_string(record.state)));
          reply.set("lifecycle_generation", record.lifecycle_generation.raw());
          reply.set("artifact_generation", record.artifact.generation.raw());
          reply.set("artifact_digest", record.artifact.digest);
          reply.set("compatibility_generation", record.compatibility_generation.raw());
          reply.set("promotion_id", record.last_promotion.raw());
          reply.set("rollout_id", record.rollout.raw());
          reply.set("rollout_generation", record.rollout_generation.raw());
          reply.set("current_stage_id", record.current_stage.raw());
          reply.set("current_stage_generation", record.current_stage_generation.raw());
          break;
        }
      }
      if (message.scope.valid()) {
        const AuthorityQueryResult authority = engine_->query_authority(message.scope);
        reply.set("scope", scope_path(*engine_, message.scope));
        reply.set("authority_outcome", std::string(to_string(authority.outcome)));
        if (authority.binding != nullptr) {
          reply.set("authoritative_version_id", authority.binding->version.raw());
          reply.set("authoritative_model_generation", authority.binding->model_generation.raw());
        }
      }
      const StoreStats stats = engine_->stats();
      reply.set("models", static_cast<std::uint64_t>(stats.models));
      reply.set("versions", static_cast<std::uint64_t>(stats.versions));
      reply.set("scopes", static_cast<std::uint64_t>(stats.scopes));
      reply.set("rollouts", static_cast<std::uint64_t>(stats.rollouts));
      reply.set("evidence_records", static_cast<std::uint64_t>(stats.evidence));
      reply.set("evidence_dropped", stats.evidence_dropped);
      reply.set("authority_bindings", static_cast<std::uint64_t>(stats.authority_bindings));
      reply.set("compatibility_facts", static_cast<std::uint64_t>(stats.compatibility_facts));
      reply.set("workers", static_cast<std::uint64_t>(stats.workers));
      reply.set("attempts", static_cast<std::uint64_t>(stats.attempts));
      reply.set("committed_promotions",
                static_cast<std::uint64_t>(stats.committed_promotions));
      reply.set("committed_rollbacks", static_cast<std::uint64_t>(stats.committed_rollbacks));
      reply.set("policy_generation", engine_->policy_generation().raw());
      static_cast<void>(send_reply(session, reply));
      return;
    }
    case MessageType::QUERY_AUTHORITY: {
      QueryAuthorityMessage message;
      if (decode(frame.payload.data(), frame.payload.size(), config_.limits, message) !=
          ProtocolStatus::Ok) {
        static_cast<void>(send_reply(session, rejected(OutcomeCode::Rejected,
                                                       ReasonCode::ProtocolDecodeFailure,
                                                       "QUERY_AUTHORITY is malformed")));
        return;
      }
      const AuthorityQueryResult authority = engine_->query_authority(message.scope);
      Reply reply;
      reply.outcome = authority.outcome;
      reply.set("scope", scope_path(*engine_, message.scope));
      reply.set("scope_generation", authority.scope_generation.raw());
      reply.set("inherited", authority.inherited ? "yes" : "no");
      if (authority.binding != nullptr) {
        reply.set("model_id", authority.binding->model.raw());
        reply.set("version_id", authority.binding->version.raw());
        reply.set("model_generation", authority.binding->model_generation.raw());
        reply.set("artifact_set_id", authority.binding->artifact_set.raw());
        reply.set("artifact_generation", authority.binding->artifact_generation.raw());
        reply.set("authority_kind", std::string(to_string(authority.binding->kind)));
        reply.set("rollout_id", authority.binding->rollout.raw());
        reply.set("rollout_generation", authority.binding->rollout_generation.raw());
        reply.set("granted_sequence", authority.binding->granted_sequence.raw());
      }
      reply.set("canary_overlays", static_cast<std::uint64_t>(authority.overlays.size()));
      reply.set("rollback_retained", static_cast<std::uint64_t>(authority.retained.size()));
      for (const Reason& reason : authority.explanation.reasons()) {
        reply.add(reason.code, reason.detail);
      }
      static_cast<void>(send_reply(session, reply));
      return;
    }
    case MessageType::FENCE: {
      FenceMessage message;
      if (decode(frame.payload.data(), frame.payload.size(), config_.limits, message) !=
          ProtocolStatus::Ok) {
        static_cast<void>(send_reply(session, rejected(OutcomeCode::Rejected,
                                                       ReasonCode::ProtocolDecodeFailure,
                                                       "FENCE is malformed")));
        return;
      }
      Decision decision = engine_->fence_worker(message.worker, message.boot, engine_->epoch(),
                                                message.reason);
      static_cast<void>(send_reply(session, reply_from_decision(decision)));
      if (decision.allowed()) {
        for (auto& candidate : sessions_) {
          if (candidate == nullptr || !candidate->socket.valid()) continue;
          if (candidate->worker != message.worker) continue;
          candidate->socket.close();
          ++stats_.sessions_closed;
        }
      }
      return;
    }
    case MessageType::PROMOTE: {
      PromotionMessage message;
      if (decode(frame.payload.data(), frame.payload.size(), config_.limits, message) !=
          ProtocolStatus::Ok) {
        static_cast<void>(send_reply(session, rejected(OutcomeCode::Rejected,
                                                       ReasonCode::ProtocolDecodeFailure,
                                                       "PROMOTE is malformed")));
        return;
      }
      if (!message.request.epoch.valid()) message.request.epoch = engine_->epoch();
      if (message.request.worker.valid() && !message.request.boot.valid()) {
        if (Session* owner = session_for_worker(message.request.worker, WorkerBootId{})) {
          message.request.boot = owner->boot;
        }
      }
      PromotionOutcome outcome = engine_->promote(message.request);
      Reply reply = reply_from_decision(outcome.decision);
      if (outcome.decision.allowed()) {
        reply.set("promotion_id", outcome.promotion.raw());
        reply.set("promotion_generation", outcome.promotion_generation.raw());
      }
      static_cast<void>(send_reply(session, reply));
      return;
    }
    case MessageType::REGISTER_ROLLOUT_PLAN: {
      RolloutPlanMessage message;
      if (decode(frame.payload.data(), frame.payload.size(), config_.limits, message) !=
          ProtocolStatus::Ok) {
        static_cast<void>(send_reply(session, rejected(OutcomeCode::Rejected,
                                                       ReasonCode::ProtocolDecodeFailure,
                                                       "plan is malformed")));
        return;
      }
      if (!message.request.epoch.valid()) message.request.epoch = engine_->epoch();
      RolloutId rollout;
      RolloutGeneration generation;
      Decision decision = engine_->create_rollout(message.request, rollout, generation);
      Reply reply = reply_from_decision(decision);
      if (decision.allowed()) {
        reply.set("rollout_id", rollout.raw());
        reply.set("rollout_generation", generation.raw());
      }
      static_cast<void>(send_reply(session, reply));
      return;
    }
    case MessageType::BEGIN_ROLLOUT:
    case MessageType::ADVANCE_STAGE:
    case MessageType::FAIL_STAGE: {
      ProgressMessage message;
      if (decode(frame.payload.data(), frame.payload.size(), config_.limits, message) !=
          ProtocolStatus::Ok) {
        static_cast<void>(send_reply(session, rejected(OutcomeCode::Rejected,
                                                       ReasonCode::ProtocolDecodeFailure,
                                                       "progress request is malformed")));
        return;
      }
      if (!message.request.epoch.valid()) message.request.epoch = engine_->epoch();
      if (message.request.worker.valid() && !message.request.boot.valid()) {
        if (Session* owner = session_for_worker(message.request.worker, WorkerBootId{})) {
          message.request.boot = owner->boot;
        }
      }
      Decision decision = frame.type == MessageType::BEGIN_ROLLOUT
                              ? engine_->begin_rollout(message.request)
                              : (frame.type == MessageType::ADVANCE_STAGE
                                     ? engine_->advance_stage(message.request)
                                     : engine_->fail_stage(message.request));
      static_cast<void>(send_reply(session, reply_from_decision(decision)));
      return;
    }
    case MessageType::RETIRE: {
      RetireMessage message;
      if (decode(frame.payload.data(), frame.payload.size(), config_.limits, message) !=
          ProtocolStatus::Ok) {
        static_cast<void>(send_reply(session, rejected(OutcomeCode::Rejected,
                                                       ReasonCode::ProtocolDecodeFailure,
                                                       "RETIRE is malformed")));
        return;
      }
      if (!message.request.epoch.valid()) message.request.epoch = engine_->epoch();
      Decision decision = engine_->retire(message.request);
      static_cast<void>(send_reply(session, reply_from_decision(decision)));
      return;
    }
    case MessageType::REVALIDATE: {
      RevalidateMessage message;
      if (decode(frame.payload.data(), frame.payload.size(), config_.limits, message) !=
          ProtocolStatus::Ok) {
        static_cast<void>(send_reply(session, rejected(OutcomeCode::Rejected,
                                                       ReasonCode::ProtocolDecodeFailure,
                                                       "REVALIDATE is malformed")));
        return;
      }
      Decision decision = engine_->revalidate(message.request);
      static_cast<void>(send_reply(session, reply_from_decision(decision)));
      return;
    }
    case MessageType::ROLLBACK: {
      RollbackMessage message;
      if (decode(frame.payload.data(), frame.payload.size(), config_.limits, message) !=
          ProtocolStatus::Ok) {
        static_cast<void>(send_reply(session, rejected(OutcomeCode::Rejected,
                                                       ReasonCode::ProtocolDecodeFailure,
                                                       "ROLLBACK is malformed")));
        return;
      }
      if (!message.request.epoch.valid()) message.request.epoch = engine_->epoch();
      RollbackOutcome outcome = engine_->rollback(message.request);
      Reply reply = reply_from_decision(outcome.decision);
      if (outcome.decision.allowed()) {
        reply.set("rollback_id", outcome.rollback.raw());
        reply.set("rollback_generation", outcome.rollback_generation.raw());
      }
      static_cast<void>(send_reply(session, reply));
      return;
    }
    case MessageType::SUPERSEDE_ROLLOUT: {
      SupersedeMessage message;
      if (decode(frame.payload.data(), frame.payload.size(), config_.limits, message) !=
          ProtocolStatus::Ok) {
        static_cast<void>(send_reply(session, rejected(OutcomeCode::Rejected,
                                                       ReasonCode::ProtocolDecodeFailure,
                                                       "SUPERSEDE_ROLLOUT is malformed")));
        return;
      }
      Decision decision =
          engine_->supersede_rollout(message.rollout, message.generation, engine_->epoch(),
                                     message.detail);
      static_cast<void>(send_reply(session, reply_from_decision(decision)));
      return;
    }
    case MessageType::RECONCILE: {
      // The coordinator reconciles durable lifecycle truth against the evidence
      // workers have actually published. Absent evidence is reported as absent.
      ReconciliationInput input;
      for (const EvidenceRecord& record : engine_->all_evidence()) {
        if (record.kind == EvidenceKind::Readiness || record.kind == EvidenceKind::ResidencyReady) {
          ResidencyObservation observation;
          observation.model = record.subject.model;
          observation.version = record.subject.version;
          observation.model_generation = record.subject.model_generation;
          observation.artifact_generation = record.subject.artifact_generation;
          observation.scope = record.subject.scope;
          observation.generation = ResidencyGeneration(record.generation.raw());
          observation.resident = record.verdict == EvidenceVerdict::Satisfied;
          observation.ready = record.verdict == EvidenceVerdict::Satisfied;
          observation.boot = record.boot;
          observation.provenance = record.provenance;
          input.residency.push_back(observation);
        }
      }
      for (const auto& entry : sessions_) {
        if (entry == nullptr || !entry->registered) continue;
        input.live_workers.push_back(entry->worker);
      }
      input.worker_liveness_known = !sessions_.empty();
      const std::vector<Finding> findings = engine_->reconcile(input);
      Reply reply;
      reply.outcome = findings.empty() ? OutcomeCode::Ok : OutcomeCode::RECONCILIATION_REQUIRED;
      reply.set("findings", static_cast<std::uint64_t>(findings.size()));
      for (std::size_t i = 0; i < findings.size() && i < 16; ++i) {
        reply.set("finding_" + std::to_string(i), std::string(to_string(findings[i].kind)));
      }
      for (const Finding& finding : findings) {
        reply.add(finding.reason, finding.detail);
      }
      static_cast<void>(send_reply(session, reply));
      return;
    }
    default: {
      ++stats_.frames_rejected;
      static_cast<void>(send_reply(session, rejected(OutcomeCode::Rejected,
                                                     ReasonCode::ProtocolUnknownType,
                                                     std::string(to_string(frame.type)))));
      return;
    }
  }
}

}  // namespace mlf
