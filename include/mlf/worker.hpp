// Model Lifecycle Fabric — lifecycle worker.
//
// A worker is a real OS process holding a real TCP connection to the
// coordinator. Its authority is bound to (WorkerId, WorkerBootId,
// CoordinatorEpoch); a dead boot permanently loses authority and a replacement
// requires a fresh boot identity and fresh evidence.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "mlf/identity.hpp"
#include "mlf/protocol.hpp"
#include "mlf/scope.hpp"
#include "mlf/synthetic_backend.hpp"

namespace mlf {

struct WorkerConfig {
  std::string coordinator_host{"127.0.0.1"};
  std::uint16_t coordinator_port{0};
  WorkerId worker_id{};
  std::vector<ScopeId> scopes{};
  ProtocolLimits limits{};
  /// When true the worker applies an activation effect and then terminates its
  /// own process before acknowledging. This exists to prove ambiguous
  /// completion with a genuine OS process death.
  bool die_on_activate{false};
  /// When true the worker never confirms completions it sends.
  bool unconfirmed_completions{false};
  /// Bounded connect retry count. Never a timeout on a lifecycle decision.
  std::uint32_t connect_attempts{1};
  /// Milliseconds between connect attempts.
  std::uint64_t connect_backoff_millis{10};
};

struct WorkerStats {
  std::uint64_t commands_executed{0};
  std::uint64_t frames_sent{0};
  std::uint64_t frames_received{0};
  std::uint64_t protocol_rejections{0};
  std::uint64_t backpressure_rejections{0};
  std::uint64_t evidence_published{0};
  std::uint64_t replies_sent{0};
};

class LifecycleWorker {
 public:
  explicit LifecycleWorker(WorkerConfig config);
  ~LifecycleWorker();

  LifecycleWorker(const LifecycleWorker&) = delete;
  LifecycleWorker& operator=(const LifecycleWorker&) = delete;

  /// Connect, negotiate HELLO, and register the worker lease.
  bool start(std::string& error);
  void stop();
  [[nodiscard]] bool connected() const noexcept { return channel_.valid(); }

  /// Receive and handle exactly one frame. Blocks until one arrives or the peer
  /// closes. Returns false on close or error.
  bool serve_one(std::string& error);
  /// Service frames until the peer closes or stop() is called.
  bool run(std::string& error);

  [[nodiscard]] WorkerId id() const noexcept { return config_.worker_id; }
  [[nodiscard]] WorkerBootId boot() const noexcept { return boot_; }
  [[nodiscard]] CoordinatorEpoch epoch() const noexcept { return epoch_; }
  [[nodiscard]] const WorkerConfig& config() const noexcept { return config_; }
  [[nodiscard]] SyntheticLifecycleBackend& backend() noexcept { return backend_; }
  [[nodiscard]] const SyntheticLifecycleBackend& backend() const noexcept { return backend_; }
  [[nodiscard]] const WorkerStats& stats() const noexcept { return stats_; }

  /// Re-publish readiness and residency evidence for everything the backend
  /// currently holds as resident and ready.
  bool publish_observations(std::string& error);

 private:
  bool send_frame(MessageType type, const std::vector<std::uint8_t>& payload, std::string& error);
  bool send_reply(const Reply& reply, std::string& error);
  bool handle_command(const CommandMessage& command, std::string& error);
  bool handle_frame(const Frame& frame, std::string& error);

  WorkerConfig config_{};
  WorkerBootId boot_{};
  CoordinatorEpoch epoch_{};
  FrameChannel channel_{};
  SyntheticLifecycleBackend backend_{};
  WorkerStats stats_{};
  bool stopping_{false};
};

}  // namespace mlf
