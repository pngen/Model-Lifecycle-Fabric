// Model Lifecycle Fabric — lifecycle coordinator.
//
// Owns the durable lifecycle engine and the framed TCP control plane. Holds no
// lock across socket I/O, filesystem work, or callbacks: engine calls complete
// and release the engine mutex before any frame is written.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "mlf/engine.hpp"
#include "mlf/protocol.hpp"

namespace mlf {

struct CoordinatorConfig {
  std::string bind_address{"127.0.0.1"};
  std::uint16_t port{0};
  /// Durable state file. Empty disables persistence.
  std::string state_path{};
  EngineConfig engine{};
  ProtocolLimits limits{};
  /// Persist after a poll iteration in which the engine sequence advanced.
  bool persist_on_change{true};
  /// Longest a single poll iteration may block, in milliseconds.
  int poll_timeout_millis{20};
  /// Whether a corrupt or unreadable durable state file prevents startup.
  bool refuse_start_on_corrupt_state{true};
};

struct CoordinatorStats {
  std::uint64_t connections_accepted{0};
  std::uint64_t sessions_closed{0};
  std::uint64_t frames_received{0};
  std::uint64_t frames_sent{0};
  std::uint64_t frames_rejected{0};
  std::uint64_t commands_dispatched{0};
  std::uint64_t dispatch_backpressure{0};
  std::uint64_t saves{0};
  std::uint64_t save_failures{0};
  std::uint64_t recoveries{0};
  std::uint64_t worker_fences{0};
};

class LifecycleCoordinator {
 public:
  explicit LifecycleCoordinator(CoordinatorConfig config);
  ~LifecycleCoordinator();

  LifecycleCoordinator(const LifecycleCoordinator&) = delete;
  LifecycleCoordinator& operator=(const LifecycleCoordinator&) = delete;

  /// Recover durable state, advance the coordinator epoch, invalidate volatile
  /// authority, and bind the control plane.
  bool start(std::string& error);
  void stop();
  [[nodiscard]] bool running() const noexcept { return running_; }
  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
  [[nodiscard]] const std::string& bind_address() const noexcept { return config_.bind_address; }

  [[nodiscard]] LifecycleEngine& engine() noexcept { return *engine_; }
  [[nodiscard]] const LifecycleEngine& engine() const noexcept { return *engine_; }

  /// Ask the poll loop to stop after its current iteration. Safe to call from
  /// another thread; stop() itself must run on the owning thread or after run()
  /// has returned.
  void request_stop() noexcept { stop_requested_.store(true); }

  /// Accept connections and service readable sessions. Never blocks longer than
  /// the configured poll interval.
  bool poll_once(std::string& error);
  /// Drive poll_once until stop() is called.
  bool run(std::string& error);

  /// Dispatch every unsettled attempt whose owning worker has a live session.
  std::size_t dispatch_pending(std::string& error);
  /// Persist durable state if the engine advanced since the last save.
  bool persist_if_changed(std::string& error);
  /// Force a durable save.
  bool persist(std::string& error);

  [[nodiscard]] std::size_t connection_count() const noexcept;
  [[nodiscard]] const CoordinatorStats& stats() const noexcept { return stats_; }
  [[nodiscard]] const std::string& state_path() const noexcept { return config_.state_path; }

 private:
  struct Session;

  void accept_ready();
  void service_session(Session& session);
  void close_session(Session& session, std::string_view reason);
  void handle_frame(Session& session, const Frame& frame, std::string& error);
  void handle_hello(Session& session, const Frame& frame);
  void handle_register_worker(Session& session, const Frame& frame);
  bool send_reply(Session& session, const Reply& reply);
  bool send_frame(Session& session, MessageType type, const std::vector<std::uint8_t>& payload);
  [[nodiscard]] Session* session_for_worker(WorkerId worker, WorkerBootId boot);
  void fence_session(Session& session, std::string_view reason);

  CoordinatorConfig config_{};
  std::unique_ptr<LifecycleEngine> engine_{};
  std::vector<std::unique_ptr<Session>> sessions_{};
  Socket listener_{};
  std::uint16_t port_{0};
  bool running_{false};
  std::atomic<bool> stop_requested_{false};
  /// Set while handling a frame that can mutate durable lifecycle state, so the
  /// acknowledgement is written only after the change is durable.
  bool pending_durable_change_{false};
  CoordinatorStats stats_{};
  Sequence last_saved_sequence_{};
  std::vector<AttemptId> dispatched_{};
};

}  // namespace mlf
