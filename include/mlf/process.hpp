// Model Lifecycle Fabric — real OS process control.
//
// The multiprocess proofs require genuine independent processes: spawning,
// observing and terminating them through the operating system, not through an
// in-process thread abstraction.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mlf {

/// State of a spawned child process.
enum class ChildState : std::uint8_t {
  Running = 0,
  Exited,
  Signalled,
  Unknown,
};

[[nodiscard]] std::string_view to_string(ChildState state) noexcept;

/// A real child process.
class ChildProcess {
 public:
  ChildProcess() = default;
  ~ChildProcess();
  ChildProcess(ChildProcess&& other) noexcept;
  ChildProcess& operator=(ChildProcess&& other) noexcept;
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;

  /// Launch an executable with arguments. No shell is involved: the executable
  /// path and each argument are passed verbatim. stdout/stderr are inherited.
  [[nodiscard]] static bool spawn(const std::string& executable,
                                  const std::vector<std::string>& arguments, ChildProcess& out,
                                  std::string& error);

  /// Process identifier as reported by the operating system.
  [[nodiscard]] std::uint64_t pid() const noexcept { return pid_; }
  [[nodiscard]] bool running() const;

  /// Poll for exit without blocking. Returns Unknown while still running.
  ChildState poll(int& exit_code);

  /// Block until exit.
  ChildState wait(int& exit_code);

  /// Terminate forcefully. This is a real OS termination, not a cooperative
  /// shutdown request, and is used only where process death is the thing under
  /// test.
  bool terminate();

  [[nodiscard]] bool valid() const noexcept { return pid_ != 0; }

 private:
  void close_handles();

  std::uint64_t pid_{0};
  std::uintptr_t handle_{0};
  std::uintptr_t thread_handle_{0};
};

/// This process's identifier.
[[nodiscard]] std::uint64_t current_process_id() noexcept;
/// Monotonic milliseconds since an unspecified epoch. Used for liveness logging
/// only, never for lifecycle decisions.
[[nodiscard]] std::uint64_t monotonic_millis() noexcept;
/// Sleep for a bounded number of milliseconds. Used only for pacing between
/// independent processes, never to decide a lifecycle outcome.
void sleep_millis(std::uint64_t millis);
/// Host name of this machine.
[[nodiscard]] std::string host_name();

}  // namespace mlf
