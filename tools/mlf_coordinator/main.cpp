// mlf_coordinator — lifecycle coordinator host.
//
// Owns durable lifecycle state, the framed TCP control plane, and the
// coordinator epoch. Restarting this process advances the epoch and invalidates
// every volatile authority it previously held.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

#include "mlf/coordinator.hpp"
#include "mlf/host_env.hpp"
#include "mlf/process.hpp"

namespace {

void usage() {
  std::printf(
      "mlf_coordinator [--bind <addr>] [--port <n>] [--state <path>] [--port-file <path>]\n"
      "                [--poll-ms <n>] [--no-persist] [--allow-corrupt-state]\n"
      "                [--host-report]\n");
}

bool parse_u64(const char* text, std::uint64_t& out) {
  if (text == nullptr || *text == '\0') return false;
  std::uint64_t value = 0;
  for (const char* cursor = text; *cursor != '\0'; ++cursor) {
    if (*cursor < '0' || *cursor > '9') return false;
    value = value * 10u + static_cast<std::uint64_t>(*cursor - '0');
  }
  out = value;
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  mlf::CoordinatorConfig config;
  std::string port_file;
  bool host_report = false;
  for (int i = 1; i < argc; ++i) {
    const char* argument = argv[i];
    if (std::strcmp(argument, "--help") == 0 || std::strcmp(argument, "-h") == 0) {
      usage();
      return 0;
    }
    if (std::strcmp(argument, "--no-persist") == 0) {
      config.persist_on_change = false;
      continue;
    }
    if (std::strcmp(argument, "--allow-corrupt-state") == 0) {
      config.refuse_start_on_corrupt_state = false;
      continue;
    }
    if (std::strcmp(argument, "--host-report") == 0) {
      host_report = true;
      continue;
    }
    if (i + 1 >= argc) {
      std::fprintf(stderr, "mlf_coordinator: option %s requires a value\n", argument);
      return 2;
    }
    const char* value = argv[++i];
    std::uint64_t parsed = 0;
    if (std::strcmp(argument, "--bind") == 0) {
      config.bind_address = value;
    } else if (std::strcmp(argument, "--port") == 0) {
      if (!parse_u64(value, parsed) || parsed > 65535) {
        std::fprintf(stderr, "mlf_coordinator: --port must be 0..65535\n");
        return 2;
      }
      config.port = static_cast<std::uint16_t>(parsed);
    } else if (std::strcmp(argument, "--state") == 0) {
      config.state_path = value;
    } else if (std::strcmp(argument, "--port-file") == 0) {
      port_file = value;
    } else if (std::strcmp(argument, "--poll-ms") == 0) {
      if (!parse_u64(value, parsed)) return 2;
      config.poll_timeout_millis = static_cast<int>(parsed);
    } else {
      std::fprintf(stderr, "mlf_coordinator: unknown option %s\n", argument);
      usage();
      return 2;
    }
  }

  if (host_report) {
    std::printf("%s", mlf::render_host_environment(mlf::discover_host_environment()).c_str());
  }

  mlf::LifecycleCoordinator coordinator(config);
  std::string error;
  if (!coordinator.start(error)) {
    std::fprintf(stderr, "mlf_coordinator: %s\n", error.c_str());
    return 1;
  }
  if (!port_file.empty()) {
    std::ofstream stream(port_file, std::ios::trunc);
    stream << coordinator.port();
    stream.flush();
  }
  std::printf("mlf_coordinator listening on %s:%u epoch=%llu\n", config.bind_address.c_str(),
              static_cast<unsigned>(coordinator.port()),
              static_cast<unsigned long long>(coordinator.engine().epoch().raw()));
  std::fflush(stdout);

  std::string loop_error;
  if (!coordinator.run(loop_error)) {
    if (!loop_error.empty()) {
      std::fprintf(stderr, "mlf_coordinator: %s\n", loop_error.c_str());
      coordinator.stop();
      return 1;
    }
  }
  std::string persist_error;
  static_cast<void>(coordinator.persist(persist_error));
  coordinator.stop();
  return 0;
}
