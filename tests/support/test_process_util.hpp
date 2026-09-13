// Model Lifecycle Fabric — shared helpers for the multiprocess proofs.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <atomic>
#include <memory>
#include <thread>

#include "mlf/client.hpp"
#include "mlf/coordinator.hpp"
#include "mlf/process.hpp"

namespace mlftest {

/// An in-process coordinator whose poll loop runs on its own thread. Real
/// sockets, real framing, real concurrent service; the transport is identical to
/// the out-of-process coordinator.
class InProcessCoordinator {
 public:
  InProcessCoordinator() = default;
  ~InProcessCoordinator();

  InProcessCoordinator(const InProcessCoordinator&) = delete;
  InProcessCoordinator& operator=(const InProcessCoordinator&) = delete;

  bool start(std::string& error, const mlf::CoordinatorConfig& config = {});
  void stop();

  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
  [[nodiscard]] mlf::LifecycleCoordinator& coordinator() noexcept { return *coordinator_; }

 private:
  std::unique_ptr<mlf::LifecycleCoordinator> coordinator_{};
  std::thread thread_{};
  std::atomic<bool> running_{false};
  std::uint16_t port_{0};
  std::string thread_error_{};
};

/// Directory for scratch state files, created on demand under the test binary
/// directory so no absolute build path is baked into the sources.
[[nodiscard]] std::string scratch_dir();

/// A unique scratch path with the given suffix.
[[nodiscard]] std::string scratch_path(const std::string& stem, const std::string& suffix);

/// Remove a file if it exists.
void remove_file(const std::string& path);

/// Read a small text file into a string. Returns false when unreadable.
[[nodiscard]] bool read_text_file(const std::string& path, std::string& out);

/// Path of the mlf_worker executable as configured by the build.
[[nodiscard]] std::string worker_binary();
/// Path of the mlf_coordinator executable as configured by the build.
[[nodiscard]] std::string coordinator_binary();

/// A real child process running a coordinator, with the reported port resolved.
class CoordinatorProcess {
 public:
  CoordinatorProcess() = default;
  ~CoordinatorProcess();
  CoordinatorProcess(CoordinatorProcess&&) noexcept = default;
  CoordinatorProcess& operator=(CoordinatorProcess&&) noexcept = default;

  /// Spawn a coordinator with the given durable state file. Blocks until the
  /// port file appears, which is the coordinator's own report of its bound port.
  bool start(const std::string& state_path, std::string& error);
  void stop();

  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
  [[nodiscard]] bool running() const;
  /// Terminate without a cooperative shutdown: used to prove recovery.
  bool kill();
  [[nodiscard]] std::uint64_t pid() const noexcept { return child_.pid(); }

 private:
  mlf::ChildProcess child_{};
  std::string port_path_{};
  std::uint16_t port_{0};
};

/// A real child process running a lifecycle worker.
class WorkerProcess {
 public:
  WorkerProcess() = default;
  ~WorkerProcess();
  WorkerProcess(WorkerProcess&&) noexcept = default;
  WorkerProcess& operator=(WorkerProcess&&) noexcept = default;

  bool start(std::uint16_t port, std::uint64_t worker_id, bool die_on_activate, bool unconfirmed,
             std::string& error);
  void stop();
  [[nodiscard]] bool running() const;
  bool kill();
  [[nodiscard]] std::uint64_t pid() const noexcept { return child_.pid(); }

 private:
  mlf::ChildProcess child_{};
};

/// Wait until a TCP connection to the port succeeds. Returns false when the
/// child process exits first, which is reported rather than waited out.
[[nodiscard]] bool wait_for_listener(const mlf::ChildProcess& child, std::uint16_t port,
                                     std::string& error);

/// Poll the coordinator until the predicate over a connected client holds. The
/// poll count is bounded so a defect surfaces as a failure instead of a hang;
/// it is never a timeout on a lifecycle decision.
template <class Predicate>
bool poll_until(mlf::LifecycleClient& client, Predicate predicate, int attempts = 400) {
  for (int i = 0; i < attempts; ++i) {
    if (predicate(client)) return true;
    mlf::sleep_millis(5);
  }
  return false;
}

/// Build a well-formed artifact binding over a real file on disk.
[[nodiscard]] mlf::ArtifactBinding binding_from_file(const std::string& path,
                                                     std::uint64_t artifact_set,
                                                     std::uint64_t generation);

/// Write a synthetic model-like artifact of the requested size and return its
/// path. The bytes are a deterministic function of the seed.
[[nodiscard]] std::string write_artifact(const std::string& stem, std::uint64_t seed,
                                         std::size_t size);

}  // namespace mlftest
