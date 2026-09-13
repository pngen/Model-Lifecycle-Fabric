#include "test_process_util.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>

#include "mlf/artifact.hpp"
#include "mlf/state_store.hpp"

#ifndef MLF_WORKER_BINARY
#define MLF_WORKER_BINARY "mlf_worker"
#endif
#ifndef MLF_COORDINATOR_BINARY
#define MLF_COORDINATOR_BINARY "mlf_coordinator"
#endif
#ifndef MLF_BINARY_DIR
#define MLF_BINARY_DIR "."
#endif

namespace mlftest {

std::string scratch_dir() {
  const std::filesystem::path directory =
      std::filesystem::path(MLF_BINARY_DIR) / "mlf_scratch";
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  return directory.string();
}

std::string scratch_path(const std::string& stem, const std::string& suffix) {
  static std::uint64_t counter = 0;
  ++counter;
  const std::filesystem::path path = std::filesystem::path(scratch_dir()) /
                                     (stem + "-" + std::to_string(counter) + suffix);
  return path.string();
}

void remove_file(const std::string& path) {
  std::error_code error;
  std::filesystem::remove(path, error);
}

bool read_text_file(const std::string& path, std::string& out) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) return false;
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  out = buffer.str();
  return true;
}

std::string worker_binary() { return MLF_WORKER_BINARY; }
std::string coordinator_binary() { return MLF_COORDINATOR_BINARY; }

CoordinatorProcess::~CoordinatorProcess() { stop(); }

bool CoordinatorProcess::start(const std::string& state_path, std::string& error) {
  port_path_ = scratch_path("coordinator-port", ".txt");
  remove_file(port_path_);
  std::vector<std::string> arguments = {"--bind", "127.0.0.1", "--port", "0", "--port-file",
                                        port_path_};
  if (!state_path.empty()) {
    arguments.push_back("--state");
    arguments.push_back(state_path);
  }
  if (!mlf::ChildProcess::spawn(coordinator_binary(), arguments, child_, error)) return false;

  for (int attempt = 0; attempt < 600; ++attempt) {
    std::string text;
    if (read_text_file(port_path_, text) && !text.empty()) {
      try {
        const auto value = static_cast<std::uint16_t>(std::stoul(text));
        if (value != 0) {
          port_ = value;
          return true;
        }
      } catch (...) {
        // The file exists but is not yet complete; keep polling.
      }
    }
    if (!child_.running()) {
      int code = 0;
      static_cast<void>(child_.poll(code));
      error = "coordinator exited before reporting its port (code " + std::to_string(code) + ")";
      return false;
    }
    mlf::sleep_millis(5);
  }
  error = "coordinator did not report a bound port";
  return false;
}

void CoordinatorProcess::stop() {
  if (!child_.valid()) return;
  if (child_.running()) static_cast<void>(child_.terminate());
  int code = 0;
  static_cast<void>(child_.wait(code));
  if (!port_path_.empty()) remove_file(port_path_);
  port_ = 0;
}

bool CoordinatorProcess::running() const { return child_.running(); }

bool CoordinatorProcess::kill() {
  if (!child_.valid()) return false;
  return child_.terminate();
}

WorkerProcess::~WorkerProcess() { stop(); }

bool WorkerProcess::start(std::uint16_t port, std::uint64_t worker_id, bool die_on_activate,
                          bool unconfirmed, std::string& error) {
  std::vector<std::string> arguments = {"--host", "127.0.0.1", "--port", std::to_string(port),
                                        "--worker-id", std::to_string(worker_id)};
  if (die_on_activate) arguments.push_back("--die-on-activate");
  if (unconfirmed) arguments.push_back("--unconfirmed-completions");
  return mlf::ChildProcess::spawn(worker_binary(), arguments, child_, error);
}

void WorkerProcess::stop() {
  if (!child_.valid()) return;
  if (child_.running()) static_cast<void>(child_.terminate());
  int code = 0;
  static_cast<void>(child_.wait(code));
}

bool WorkerProcess::running() const { return child_.running(); }

bool WorkerProcess::kill() {
  if (!child_.valid()) return false;
  return child_.terminate();
}

bool wait_for_listener(const mlf::ChildProcess& child, std::uint16_t port, std::string& error) {
  for (int attempt = 0; attempt < 600; ++attempt) {
    mlf::Socket socket;
    std::string connect_error;
    if (mlf::Socket::connect_to("127.0.0.1", port, socket, connect_error) ==
        mlf::ProtocolStatus::Ok) {
      socket.close();
      return true;
    }
    if (!child.running()) {
      error = "child process exited before the control plane became reachable";
      return false;
    }
    mlf::sleep_millis(5);
  }
  error = "control plane did not become reachable";
  return false;
}

mlf::ArtifactBinding binding_from_file(const std::string& path, std::uint64_t artifact_set,
                                       std::uint64_t generation) {
  mlf::ArtifactBinding binding;
  binding.set_id = mlf::ArtifactSetId(artifact_set);
  binding.generation = mlf::ArtifactGeneration(generation);
  std::string digest;
  if (mlf::compute_file_digest(path, digest)) {
    binding.digest = digest;
  } else {
    binding.digest = "unreadable:" + path;
  }
  return binding;
}

std::string write_artifact(const std::string& stem, std::uint64_t seed, std::size_t size) {
  const std::string path = scratch_path(stem, ".bin");
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  std::uint64_t state = seed == 0 ? 0x2545F4914F6CDD1Dull : seed;
  std::string buffer;
  buffer.reserve(size);
  for (std::size_t i = 0; i < size; ++i) {
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    buffer.push_back(static_cast<char>(state & 0xffu));
  }
  stream.write(buffer.data(), static_cast<std::streamsize>(buffer.size()));
  stream.close();
  return path;
}

}  // namespace mlftest
namespace mlftest {

InProcessCoordinator::~InProcessCoordinator() { stop(); }

bool InProcessCoordinator::start(std::string& error, const mlf::CoordinatorConfig& config) {
  mlf::CoordinatorConfig effective = config;
  effective.bind_address = "127.0.0.1";
  effective.port = 0;
  effective.poll_timeout_millis = 2;
  coordinator_ = std::make_unique<mlf::LifecycleCoordinator>(effective);
  if (!coordinator_->start(error)) {
    coordinator_.reset();
    return false;
  }
  port_ = coordinator_->port();
  running_.store(true);
  thread_ = std::thread([this]() {
    std::string loop_error;
    if (!coordinator_->run(loop_error)) {
      thread_error_ = loop_error;
    }
  });
  return true;
}

void InProcessCoordinator::stop() {
  if (!coordinator_) return;
  running_.store(false);
  // The poll loop owns every session. Signal it, wait for it to return, and only
  // then tear the control plane down from this thread.
  coordinator_->request_stop();
  if (thread_.joinable()) thread_.join();
  coordinator_->stop();
  coordinator_.reset();
  port_ = 0;
}

}  // namespace mlftest
