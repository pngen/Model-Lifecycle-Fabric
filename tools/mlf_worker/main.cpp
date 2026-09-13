// mlf_worker — lifecycle worker host.
//
// A real OS process: it connects to the coordinator over TCP, holds a worker
// lease bound to its boot identity, executes lifecycle commands against the
// synthetic backend, and publishes the evidence it observes.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "mlf/process.hpp"
#include "mlf/worker.hpp"

namespace {

void usage() {
  std::printf(
      "mlf_worker --host <addr> --port <n> [--worker-id <id>] [--scopes <n,n,...>]\n"
      "           [--die-on-activate] [--unconfirmed-completions]\n"
      "\n"
      "  --die-on-activate            apply an activation effect, then terminate this\n"
      "                               process before acknowledging it. Used to prove\n"
      "                               ambiguous completion with real process death.\n"
      "  --unconfirmed-completions    never confirm a completion this worker sends.\n");
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
  mlf::WorkerConfig config;
  for (int i = 1; i < argc; ++i) {
    const char* argument = argv[i];
    if (std::strcmp(argument, "--help") == 0 || std::strcmp(argument, "-h") == 0) {
      usage();
      return 0;
    }
    if (std::strcmp(argument, "--die-on-activate") == 0) {
      config.die_on_activate = true;
      continue;
    }
    if (std::strcmp(argument, "--unconfirmed-completions") == 0) {
      config.unconfirmed_completions = true;
      continue;
    }
    if (i + 1 >= argc) {
      std::fprintf(stderr, "mlf_worker: option %s requires a value\n", argument);
      return 2;
    }
    const char* value = argv[++i];
    std::uint64_t parsed = 0;
    if (std::strcmp(argument, "--host") == 0) {
      config.coordinator_host = value;
    } else if (std::strcmp(argument, "--port") == 0) {
      if (!parse_u64(value, parsed) || parsed == 0 || parsed > 65535) {
        std::fprintf(stderr, "mlf_worker: --port must be 1..65535\n");
        return 2;
      }
      config.coordinator_port = static_cast<std::uint16_t>(parsed);
    } else if (std::strcmp(argument, "--worker-id") == 0) {
      if (!parse_u64(value, parsed) || parsed == 0) {
        std::fprintf(stderr, "mlf_worker: --worker-id must be a positive integer\n");
        return 2;
      }
      config.worker_id = mlf::WorkerId(parsed);
    } else if (std::strcmp(argument, "--scopes") == 0) {
      std::string text(value);
      std::size_t start = 0;
      while (start <= text.size()) {
        const std::size_t comma = text.find(',', start);
        const std::size_t end = comma == std::string::npos ? text.size() : comma;
        if (end > start) {
          std::uint64_t scope = 0;
          if (parse_u64(text.substr(start, end - start).c_str(), scope) && scope != 0) {
            config.scopes.push_back(mlf::ScopeId(scope));
          }
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
      }
    } else {
      std::fprintf(stderr, "mlf_worker: unknown option %s\n", argument);
      usage();
      return 2;
    }
  }
  if (config.coordinator_port == 0) {
    std::fprintf(stderr, "mlf_worker: --port is required\n");
    return 2;
  }
  if (!config.worker_id.valid()) {
    config.worker_id = mlf::WorkerId(mlf::current_process_id());
  }

  mlf::LifecycleWorker worker(config);
  std::string error;
  if (!worker.start(error)) {
    std::fprintf(stderr, "mlf_worker: %s\n", error.c_str());
    return 1;
  }
  while (true) {
    std::string serve_error;
    if (!worker.serve_one(serve_error)) {
      if (!serve_error.empty()) {
        std::fprintf(stderr, "mlf_worker: %s\n", serve_error.c_str());
        return 1;
      }
      break;
    }
  }
  worker.stop();
  return 0;
}
