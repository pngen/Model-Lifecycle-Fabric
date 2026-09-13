#include "mlf/process.hpp"

#include <chrono>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <csignal>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace mlf {

std::string_view to_string(ChildState state) noexcept {
  switch (state) {
    case ChildState::Running:   return "RUNNING";
    case ChildState::Exited:    return "EXITED";
    case ChildState::Signalled: return "SIGNALLED";
    case ChildState::Unknown:   break;
  }
  return "UNKNOWN";
}

ChildProcess::~ChildProcess() { close_handles(); }

ChildProcess::ChildProcess(ChildProcess&& other) noexcept
    : pid_(other.pid_), handle_(other.handle_), thread_handle_(other.thread_handle_) {
  other.pid_ = 0;
  other.handle_ = 0;
  other.thread_handle_ = 0;
}

ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept {
  if (this != &other) {
    close_handles();
    pid_ = other.pid_;
    handle_ = other.handle_;
    thread_handle_ = other.thread_handle_;
    other.pid_ = 0;
    other.handle_ = 0;
    other.thread_handle_ = 0;
  }
  return *this;
}

void ChildProcess::close_handles() {
#ifdef _WIN32
  if (handle_ != 0) {
    ::CloseHandle(reinterpret_cast<HANDLE>(handle_));
    handle_ = 0;
  }
  if (thread_handle_ != 0) {
    ::CloseHandle(reinterpret_cast<HANDLE>(thread_handle_));
    thread_handle_ = 0;
  }
#endif
  pid_ = 0;
}

#ifdef _WIN32

bool ChildProcess::spawn(const std::string& executable, const std::vector<std::string>& arguments,
                         ChildProcess& out, std::string& error) {
  std::string command_line = "\"" + executable + "\"";
  for (const std::string& argument : arguments) {
    command_line.push_back(' ');
    // Quote each argument and escape embedded quotes so no shell metacharacter
    // or space can change the meaning of an argument.
    command_line.push_back('"');
    for (char c : argument) {
      if (c == '"') command_line.push_back('\\');
      command_line.push_back(c);
    }
    command_line.push_back('"');
  }

  std::string mutable_command = command_line;
  STARTUPINFOA startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION information{};
  const BOOL created =
      ::CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr,
                       &startup, &information);
  if (created == 0) {
    error = "CreateProcess failed with code " + std::to_string(::GetLastError());
    return false;
  }
  out.close_handles();
  out.pid_ = static_cast<std::uint64_t>(information.dwProcessId);
  out.handle_ = reinterpret_cast<std::uintptr_t>(information.hProcess);
  out.thread_handle_ = reinterpret_cast<std::uintptr_t>(information.hThread);
  return true;
}

bool ChildProcess::running() const {
  if (handle_ == 0) return false;
  const DWORD status = ::WaitForSingleObject(reinterpret_cast<HANDLE>(handle_), 0);
  return status == WAIT_TIMEOUT;
}

ChildState ChildProcess::poll(int& exit_code) {
  exit_code = 0;
  if (handle_ == 0) return ChildState::Unknown;
  const DWORD status = ::WaitForSingleObject(reinterpret_cast<HANDLE>(handle_), 0);
  if (status == WAIT_TIMEOUT) return ChildState::Running;
  if (status != WAIT_OBJECT_0) return ChildState::Unknown;
  DWORD code = 0;
  if (::GetExitCodeProcess(reinterpret_cast<HANDLE>(handle_), &code) == 0) return ChildState::Unknown;
  exit_code = static_cast<int>(code);
  if (code == static_cast<DWORD>(0xC0000000u) || code >= 0x80000000u) return ChildState::Signalled;
  return ChildState::Exited;
}

ChildState ChildProcess::wait(int& exit_code) {
  exit_code = 0;
  if (handle_ == 0) return ChildState::Unknown;
  const DWORD status = ::WaitForSingleObject(reinterpret_cast<HANDLE>(handle_), INFINITE);
  if (status != WAIT_OBJECT_0) return ChildState::Unknown;
  return poll(exit_code);
}

bool ChildProcess::terminate() {
  if (handle_ == 0) return false;
  if (!running()) return true;
  if (::TerminateProcess(reinterpret_cast<HANDLE>(handle_), 1) == 0) return false;
  ::WaitForSingleObject(reinterpret_cast<HANDLE>(handle_), INFINITE);
  return true;
}

std::uint64_t current_process_id() noexcept {
  return static_cast<std::uint64_t>(::GetCurrentProcessId());
}

std::string host_name() {
  char buffer[256] = {};
  DWORD size = static_cast<DWORD>(sizeof(buffer));
  if (::GetComputerNameA(buffer, &size) == 0) return "unknown-host";
  return std::string(buffer, size);
}

#else

bool ChildProcess::spawn(const std::string& executable, const std::vector<std::string>& arguments,
                         ChildProcess& out, std::string& error) {
  std::vector<char*> argv;
  std::string program = executable;
  argv.push_back(program.data());
  std::vector<std::string> storage = arguments;
  for (std::string& argument : storage) argv.push_back(argument.data());
  argv.push_back(nullptr);

  const pid_t child = ::fork();
  if (child < 0) {
    error = "fork failed";
    return false;
  }
  if (child == 0) {
    ::execv(program.c_str(), argv.data());
    ::_exit(127);
  }
  out.close_handles();
  out.pid_ = static_cast<std::uint64_t>(child);
  return true;
}

bool ChildProcess::running() const {
  if (pid_ == 0) return false;
  int status = 0;
  const pid_t result = ::waitpid(static_cast<pid_t>(pid_), &status, WNOHANG);
  return result == 0;
}

ChildState ChildProcess::poll(int& exit_code) {
  exit_code = 0;
  if (pid_ == 0) return ChildState::Unknown;
  int status = 0;
  const pid_t result = ::waitpid(static_cast<pid_t>(pid_), &status, WNOHANG);
  if (result == 0) return ChildState::Running;
  if (result < 0) return ChildState::Unknown;
  if (WIFEXITED(status)) {
    exit_code = WEXITSTATUS(status);
    return ChildState::Exited;
  }
  if (WIFSIGNALED(status)) {
    exit_code = 128 + WTERMSIG(status);
    return ChildState::Signalled;
  }
  return ChildState::Unknown;
}

ChildState ChildProcess::wait(int& exit_code) {
  exit_code = 0;
  if (pid_ == 0) return ChildState::Unknown;
  int status = 0;
  if (::waitpid(static_cast<pid_t>(pid_), &status, 0) < 0) return ChildState::Unknown;
  if (WIFEXITED(status)) {
    exit_code = WEXITSTATUS(status);
    return ChildState::Exited;
  }
  if (WIFSIGNALED(status)) {
    exit_code = 128 + WTERMSIG(status);
    return ChildState::Signalled;
  }
  return ChildState::Unknown;
}

bool ChildProcess::terminate() {
  if (pid_ == 0) return false;
  if (!running()) return true;
  if (::kill(static_cast<pid_t>(pid_), SIGKILL) != 0) return false;
  int status = 0;
  ::waitpid(static_cast<pid_t>(pid_), &status, 0);
  return true;
}

std::uint64_t current_process_id() noexcept { return static_cast<std::uint64_t>(::getpid()); }

std::string host_name() {
  char buffer[256] = {};
  if (::gethostname(buffer, sizeof(buffer)) != 0) return "unknown-host";
  return std::string(buffer);
}

#endif

std::uint64_t monotonic_millis() noexcept {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

void sleep_millis(std::uint64_t millis) {
  std::this_thread::sleep_for(std::chrono::milliseconds(millis));
}

}  // namespace mlf
