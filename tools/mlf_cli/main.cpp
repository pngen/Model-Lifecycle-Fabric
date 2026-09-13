// mlf_cli — deterministic inspection and mutation tool.
//
// Read-only commands can inspect a durable state file offline, with no
// coordinator running. Mutation commands require a live coordinator endpoint and
// go through the same framed protocol every other client uses.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "mlf/artifact.hpp"
#include "mlf/client.hpp"
#include "mlf/engine.hpp"
#include "mlf/host_env.hpp"
#include "mlf/state_store.hpp"
#include "mlf/version.hpp"

namespace {

struct Options {
  std::string command;
  std::string endpoint;
  std::string state_path;
  std::string scope_path{"global"};
  std::string model_name;
  std::string family;
  std::string label;
  std::string runtime;
  std::string backend;
  std::string architecture;
  std::string precision;
  std::string tokenizer;
  std::string adapter;
  std::string artifact_path;
  std::string digest;
  std::string environment_key{"local"};
  std::string outcometext;
  std::uint64_t model_id{0};
  std::uint64_t version_id{0};
  std::uint64_t predecessor{0};
  std::uint64_t artifact_set{0};
  std::uint64_t artifact_generation{1};
  std::uint64_t target_version{0};
  std::uint64_t rollout_id{0};
  std::uint64_t worker_id{0};
  std::uint64_t traffic_percent{100};
  bool immediate{false};
  bool json_list{false};
};

void usage() {
  std::printf(
      "mlf_cli <command> [options]\n"
      "\n"
      "Read-only commands (accept --endpoint or --state):\n"
      "  host                 report the real host environment and provenance\n"
      "  summary              store statistics and coordinator epoch\n"
      "  models               list registered model families\n"
      "  versions             list model versions and lifecycle state\n"
      "  state                state of one version (--version-id)\n"
      "  authority            authoritative generation for --scope\n"
      "  rollout              rollout plans and stage history\n"
      "  compatibility        published compatibility facts\n"
      "  evidence             retained evidence records\n"
      "  workers              worker leases and boot identities\n"
      "  epoch                current coordinator epoch and sequence\n"
      "\n"
      "Mutation commands (require --endpoint):\n"
      "  register-model       --name --family [--provenance]\n"
      "  register-version     --model-id --label --artifact-set --artifact\n"
      "                       [--runtime --backend --architecture --precision]\n"
      "  publish-compatibility --version-id --outcome [--environment-key]\n"
      "  promote              --model-id --version-id --scope [--immediate]\n"
      "  plan                 create a rollout plan for a promoted candidate\n"
      "  begin                enter the first stage of a rollout\n"
      "  advance              advance the current stage\n"
      "  fail                 fail the current stage\n"
      "  rollback             --model-id --version-id --target-version --scope\n"
      "  retire               --model-id --version-id\n"
      "  revalidate           --model-id --version-id\n"
      "  fence                --worker-id\n"
      "\n"
      "Global options: --endpoint <host:port> | --state <path> --scope <path>\n");
}

bool parse_u64(const std::string& text, std::uint64_t& out) {
  if (text.empty()) return false;
  std::uint64_t value = 0;
  for (char c : text) {
    if (c < '0' || c > '9') return false;
    value = value * 10u + static_cast<std::uint64_t>(c - '0');
  }
  out = value;
  return true;
}

bool parse_options(int argc, char** argv, Options& options) {
  if (argc < 2) {
    usage();
    return false;
  }
  options.command = argv[1];
  for (int i = 2; i < argc; ++i) {
    const std::string argument = argv[i];
    const auto value = [&](std::string& target) {
      if (i + 1 >= argc) return false;
      target = argv[++i];
      return true;
    };
    if (argument == "--endpoint") {
      if (!value(options.endpoint)) return false;
    } else if (argument == "--state") {
      if (!value(options.state_path)) return false;
    } else if (argument == "--scope") {
      if (!value(options.scope_path)) return false;
    } else if (argument == "--name") {
      if (!value(options.model_name)) return false;
    } else if (argument == "--family") {
      if (!value(options.family)) return false;
    } else if (argument == "--label") {
      if (!value(options.label)) return false;
    } else if (argument == "--runtime") {
      if (!value(options.runtime)) return false;
    } else if (argument == "--backend") {
      if (!value(options.backend)) return false;
    } else if (argument == "--architecture") {
      if (!value(options.architecture)) return false;
    } else if (argument == "--precision") {
      if (!value(options.precision)) return false;
    } else if (argument == "--tokenizer") {
      if (!value(options.tokenizer)) return false;
    } else if (argument == "--adapter") {
      if (!value(options.adapter)) return false;
    } else if (argument == "--artifact") {
      if (!value(options.artifact_path)) return false;
    } else if (argument == "--digest") {
      if (!value(options.digest)) return false;
    } else if (argument == "--environment-key") {
      if (!value(options.environment_key)) return false;
    } else if (argument == "--outcome") {
      if (!value(options.outcometext)) return false;
    } else if (argument == "--model-id") {
      std::string text;
      if (!value(text) || !parse_u64(text, options.model_id)) return false;
    } else if (argument == "--version-id") {
      std::string text;
      if (!value(text) || !parse_u64(text, options.version_id)) return false;
    } else if (argument == "--predecessor") {
      std::string text;
      if (!value(text) || !parse_u64(text, options.predecessor)) return false;
    } else if (argument == "--artifact-set") {
      std::string text;
      if (!value(text) || !parse_u64(text, options.artifact_set)) return false;
    } else if (argument == "--artifact-generation") {
      std::string text;
      if (!value(text) || !parse_u64(text, options.artifact_generation)) return false;
    } else if (argument == "--target-version") {
      std::string text;
      if (!value(text) || !parse_u64(text, options.target_version)) return false;
    } else if (argument == "--rollout-id") {
      std::string text;
      if (!value(text) || !parse_u64(text, options.rollout_id)) return false;
    } else if (argument == "--worker-id") {
      std::string text;
      if (!value(text) || !parse_u64(text, options.worker_id)) return false;
    } else if (argument == "--traffic-percent") {
      std::string text;
      if (!value(text) || !parse_u64(text, options.traffic_percent)) return false;
    } else if (argument == "--immediate") {
      options.immediate = true;
    } else if (argument == "--help" || argument == "-h") {
      usage();
      return false;
    } else {
      std::fprintf(stderr, "mlf_cli: unknown option %s\n", argument.c_str());
      return false;
    }
  }
  return true;
}

/// Split "host:port" into its parts.
bool split_endpoint(const std::string& endpoint, std::string& host, std::uint16_t& port) {
  const std::size_t colon = endpoint.rfind(':');
  if (colon == std::string::npos) return false;
  host = endpoint.substr(0, colon);
  std::uint64_t value = 0;
  if (!parse_u64(endpoint.substr(colon + 1), value) || value == 0 || value > 65535) return false;
  port = static_cast<std::uint16_t>(value);
  return true;
}

/// Resolve a scope path against an engine. The record is returned by value: the
/// registry hands out a snapshot, so retaining a pointer into it would dangle.
std::optional<mlf::ScopeRecord> find_scope(const mlf::LifecycleEngine& engine,
                                           const std::string& path) {
  for (const mlf::ScopeRecord& record : engine.scopes()) {
    if (record.path == path) return record;
  }
  return std::nullopt;
}

/// Resolve a scope path against a live coordinator, creating it when needed.
bool resolve_scope(mlf::LifecycleClient& client, const std::string& path, mlf::ScopeId& out,
                   std::string& error) {
  mlf::RegisterScopeMessage message;
  message.path = path;
  const mlf::Reply reply = client.call(message, error);
  if (!reply.ok()) {
    std::fprintf(stderr, "mlf_cli: cannot resolve scope '%s': %s\n", path.c_str(),
                 reply.render().c_str());
    return false;
  }
  out = mlf::ScopeId(reply.u64("scope_id"));
  return out.valid();
}

std::string version_label(const mlf::LifecycleEngine& engine, mlf::ModelVersionId id) {
  for (const mlf::ModelVersionRecord& record : engine.all_versions()) {
    if (record.version == id) return record.label;
  }
  return "?";
}

void print_version(const mlf::ModelVersionRecord& record) {
  std::printf("version %-6llu label=%-20s state=%-22s gen=%llu artifact=%llu/%llu compat=%llu "
              "promotion=%llu rollout=%llu/%llu\n",
              static_cast<unsigned long long>(record.version.raw()), record.label.c_str(),
              std::string(mlf::to_string(record.state)).c_str(),
              static_cast<unsigned long long>(record.generation.raw()),
              static_cast<unsigned long long>(record.artifact.set_id.raw()),
              static_cast<unsigned long long>(record.artifact.generation.raw()),
              static_cast<unsigned long long>(record.compatibility_generation.raw()),
              static_cast<unsigned long long>(record.last_promotion.raw()),
              static_cast<unsigned long long>(record.rollout.raw()),
              static_cast<unsigned long long>(record.rollout_generation.raw()));
}

/// Load durable state into an offline engine for read-only inspection.
bool load_offline(const Options& options, mlf::LifecycleEngine& engine) {
  mlf::DurableState state;
  const mlf::DecodeLimits limits = mlf::DecodeLimits::from_bounds(engine.bounds());
  const mlf::StateStoreResult result = mlf::load_durable_state(options.state_path, limits, state);
  if (!result.ok()) {
    std::fprintf(stderr, "mlf_cli: cannot load state: %s (%s)\n",
                 std::string(mlf::to_string(result.status)).c_str(), result.detail.c_str());
    return false;
  }
  mlf::Decision decision =
      engine.import_durable_state(state, mlf::next_generation(state.epoch));
  if (!decision.allowed()) {
    std::fprintf(stderr, "mlf_cli: state rejected: %s\n", decision.render().c_str());
    return false;
  }
  return true;
}

int read_only_command(const Options& options) {
  if (options.command == "host") {
    std::printf("%s", mlf::render_host_environment(mlf::discover_host_environment()).c_str());
    return 0;
  }

  mlf::LifecycleEngine offline;
  mlf::LifecycleEngine* engine = &offline;
  std::optional<mlf::LifecycleClient> client;
  std::string error;
  if (!options.endpoint.empty()) {
    std::string host;
    std::uint16_t port = 0;
    if (!split_endpoint(options.endpoint, host, port)) {
      std::fprintf(stderr, "mlf_cli: --endpoint must be host:port\n");
      return 2;
    }
    client.emplace();
    if (!client->connect(host, port, error)) {
      std::fprintf(stderr, "mlf_cli: %s\n", error.c_str());
      return 1;
    }
    if (!client->hello(mlf::WorkerId{}, mlf::WorkerBootId{}, mlf::CoordinatorEpoch{}, {}, error)
             .ok()) {
      std::fprintf(stderr, "mlf_cli: handshake refused\n");
      return 1;
    }
  } else if (!options.state_path.empty()) {
    if (!load_offline(options, offline)) return 1;
  } else {
    std::fprintf(stderr, "mlf_cli: provide --endpoint or --state\n");
    return 2;
  }

  if (client.has_value()) {
    // Remote inspection uses the protocol only; no direct engine access. The
    // wire protocol exposes a bounded summary and single-entity lookups, so
    // whole-registry listings are served from a durable state file instead of
    // being fabricated here.
    const bool remote_supported = options.command == "summary" || options.command == "epoch" ||
                                  options.command == "state" || options.command == "authority" ||
                                  options.command == "workers";
    if (!remote_supported) {
      std::fprintf(stderr,
                   "mlf_cli: '%s' lists the whole registry and requires --state <path>; the "
                   "control plane exposes bounded single-entity queries only\n",
                   options.command.c_str());
      return 2;
    }
    mlf::QueryStateMessage query;
    query.model = mlf::ModelId(options.model_id);
    query.version = mlf::ModelVersionId(options.version_id);
    mlf::Reply reply = client->call(query, error);
    std::printf("outcome=%s\n", std::string(mlf::to_string(reply.outcome)).c_str());
    for (const auto& entry : reply.values) {
      std::printf("  %-28s = %s\n", entry.first.c_str(), entry.second.c_str());
    }
    for (const mlf::Reason& reason : reply.reasons) {
      std::printf("  reason: %s %s\n", std::string(mlf::to_string(reason.code)).c_str(),
                  reason.detail.c_str());
    }
    if (options.command == "authority") {
      if (!options.scope_path.empty()) {
        const mlf::ScopeId scope = [&]() {
          for (const mlf::ScopeRecord& record : offline.scopes()) {
            if (record.path == options.scope_path) return record.id;
          }
          return mlf::ScopeId{};
        }();
        mlf::QueryAuthorityMessage authority_query;
        authority_query.scope = scope;
        mlf::Reply authority = client->call(authority_query, error);
        std::printf("authority outcome=%s\n",
                    std::string(mlf::to_string(authority.outcome)).c_str());
        for (const auto& entry : authority.values) {
          std::printf("  %-28s = %s\n", entry.first.c_str(), entry.second.c_str());
        }
      }
    }
    return reply.ok() ? 0 : 1;
  }

  engine = &offline;
  if (options.command == "summary" || options.command == "epoch") {
    const mlf::StoreStats stats = engine->stats();
    std::printf("coordinator_epoch   = %llu\n",
                static_cast<unsigned long long>(engine->epoch().raw()));
    std::printf("sequence            = %llu\n",
                static_cast<unsigned long long>(engine->sequence().raw()));
    std::printf("policy_generation   = %llu\n",
                static_cast<unsigned long long>(engine->policy_generation().raw()));
    std::printf("snapshot_generation = %llu\n",
                static_cast<unsigned long long>(engine->snapshot_generation().raw()));
    std::printf("models              = %zu\n", stats.models);
    std::printf("versions            = %zu\n", stats.versions);
    std::printf("scopes              = %zu\n", stats.scopes);
    std::printf("rollouts            = %zu\n", stats.rollouts);
    std::printf("authority_bindings  = %zu\n", stats.authority_bindings);
    std::printf("compatibility_facts = %zu\n", stats.compatibility_facts);
    std::printf("evidence_records    = %zu (dropped %llu)\n", stats.evidence,
                static_cast<unsigned long long>(stats.evidence_dropped));
    std::printf("attempts            = %zu\n", stats.attempts);
    std::printf("workers             = %zu\n", stats.workers);
    return 0;
  }
  if (options.command == "models") {
    for (const mlf::ModelRecord& record : engine->models()) {
      std::printf("model %-6llu name=%-28s family=%-20s provenance=%s\n",
                  static_cast<unsigned long long>(record.id.raw()), record.name.c_str(),
                  record.family.c_str(), std::string(mlf::to_string(record.provenance)).c_str());
    }
    return 0;
  }
  if (options.command == "versions") {
    for (const mlf::ModelVersionRecord& record : engine->all_versions()) {
      std::printf("model %-6llu ", static_cast<unsigned long long>(record.model.raw()));
      print_version(record);
    }
    return 0;
  }
  if (options.command == "state") {
    if (options.version_id == 0) {
      std::fprintf(stderr, "mlf_cli: --version-id is required\n");
      return 2;
    }
    for (const mlf::ModelVersionRecord& record : engine->all_versions()) {
      if (record.version.raw() != options.version_id) continue;
      print_version(record);
      std::printf("  artifact_digest      = %s\n", record.artifact.digest.c_str());
      std::printf("  predecessor          = %llu\n",
                  static_cast<unsigned long long>(record.predecessor.raw()));
      std::printf("  lifecycle_generation = %llu\n",
                  static_cast<unsigned long long>(record.lifecycle_generation.raw()));
      std::printf("  last_rollback        = %llu gen %llu\n",
                  static_cast<unsigned long long>(record.last_rollback.raw()),
                  static_cast<unsigned long long>(record.rollback_generation.raw()));
      for (const mlf::AuthorityBinding& binding :
           engine->authority_bindings(record.model, record.version)) {
        if (binding.superseded) continue;
        std::printf("  authority scope=%llu kind=%s gen=%llu seq=%llu\n",
                    static_cast<unsigned long long>(binding.scope.raw()),
                    std::string(mlf::to_string(binding.kind)).c_str(),
                    static_cast<unsigned long long>(binding.model_generation.raw()),
                    static_cast<unsigned long long>(binding.granted_sequence.raw()));
      }
      return 0;
    }
    std::fprintf(stderr, "mlf_cli: version %llu not found\n",
                 static_cast<unsigned long long>(options.version_id));
    return 1;
  }
  if (options.command == "authority") {
    const std::optional<mlf::ScopeRecord> scope = find_scope(*engine, options.scope_path);
    if (!scope.has_value()) {
      std::fprintf(stderr, "mlf_cli: scope %s is not registered\n", options.scope_path.c_str());
      return 1;
    }
    const mlf::AuthorityQueryResult result = engine->query_authority(scope->id);
    std::printf("explanation:\n%s", result.explanation.render().c_str());
    if (result.binding != nullptr) {
      std::printf("authoritative version label = %s\n",
                  version_label(*engine, result.binding->version).c_str());
    }
    return result.outcome == mlf::OutcomeCode::Ok ? 0 : 1;
  }
  if (options.command == "rollout") {
    for (const mlf::RolloutPlan& plan : engine->rollouts()) {
      std::printf("rollout %llu gen=%llu candidate=%llu strategy=%s stage=%zu/%zu live=%s "
                  "completed=%s failed=%s superseded=%s\n",
                  static_cast<unsigned long long>(plan.id.raw()),
                  static_cast<unsigned long long>(plan.generation.raw()),
                  static_cast<unsigned long long>(plan.candidate.raw()),
                  std::string(mlf::to_string(plan.strategy)).c_str(), plan.current_stage_index,
                  plan.stages.size(), plan.live() ? "yes" : "no", plan.completed ? "yes" : "no",
                  plan.failed ? "yes" : "no", plan.superseded ? "yes" : "no");
      for (const mlf::StageRecord& record : plan.history) {
        std::printf("  stage %llu gen=%llu decision=%s entered=%llu decided=%llu\n",
                    static_cast<unsigned long long>(record.stage.raw()),
                    static_cast<unsigned long long>(record.generation.raw()),
                    std::string(mlf::to_string(record.decision)).c_str(),
                    static_cast<unsigned long long>(record.entered_sequence.raw()),
                    static_cast<unsigned long long>(record.decided_sequence.raw()));
      }
    }
    return 0;
  }
  if (options.command == "compatibility") {
    for (const mlf::ModelVersionRecord& record : engine->all_versions()) {
      for (const mlf::CompatibilityFact& fact : engine->compatibility_facts(record.version)) {
        std::printf("version %-6llu env=%-16s outcome=%-28s model_gen=%llu artifact_gen=%llu "
                    "provenance=%s\n",
                    static_cast<unsigned long long>(fact.version.raw()),
                    fact.environment_key.c_str(),
                    std::string(mlf::to_string(fact.outcome)).c_str(),
                    static_cast<unsigned long long>(fact.model_generation.raw()),
                    static_cast<unsigned long long>(fact.artifact_generation.raw()),
                    std::string(mlf::to_string(fact.provenance)).c_str());
      }
    }
    return 0;
  }
  if (options.command == "evidence") {
    for (const mlf::EvidenceRecord& record : engine->all_evidence()) {
      std::printf("evidence %-6llu gen=%-6llu kind=%-22s verdict=%-11s version=%llu "
                  "scope=%llu boot=%llu provenance=%s\n",
                  static_cast<unsigned long long>(record.id.raw()),
                  static_cast<unsigned long long>(record.generation.raw()),
                  std::string(mlf::to_string(record.kind)).c_str(),
                  std::string(mlf::to_string(record.verdict)).c_str(),
                  static_cast<unsigned long long>(record.subject.version.raw()),
                  static_cast<unsigned long long>(record.subject.scope.raw()),
                  static_cast<unsigned long long>(record.boot.raw()),
                  std::string(mlf::to_string(record.provenance)).c_str());
    }
    return 0;
  }
  if (options.command == "workers") {
    for (const mlf::WorkerLease& lease : engine->workers()) {
      std::printf("worker %-6llu boot=%-6llu epoch=%-6llu fenced=%s scopes=%zu\n",
                  static_cast<unsigned long long>(lease.id.raw()),
                  static_cast<unsigned long long>(lease.boot.raw()),
                  static_cast<unsigned long long>(lease.epoch.raw()),
                  lease.fenced ? "yes" : "no", lease.scopes.size());
    }
    return 0;
  }
  std::fprintf(stderr, "mlf_cli: unknown read-only command %s\n", options.command.c_str());
  usage();
  return 2;
}

int mutation_command(const Options& options) {
  if (options.endpoint.empty()) {
    std::fprintf(stderr, "mlf_cli: %s requires --endpoint\n", options.command.c_str());
    return 2;
  }
  std::string host;
  std::uint16_t port = 0;
  if (!split_endpoint(options.endpoint, host, port)) {
    std::fprintf(stderr, "mlf_cli: --endpoint must be host:port\n");
    return 2;
  }
  mlf::LifecycleClient client;
  std::string error;
  if (!client.connect(host, port, error)) {
    std::fprintf(stderr, "mlf_cli: %s\n", error.c_str());
    return 1;
  }
  mlf::Reply hello = client.hello(mlf::WorkerId{}, mlf::WorkerBootId{}, mlf::CoordinatorEpoch{},
                                  {}, error);
  if (!hello.ok()) {
    std::fprintf(stderr, "mlf_cli: handshake refused: %s\n", hello.render().c_str());
    return 1;
  }
  const mlf::CoordinatorEpoch epoch(hello.u64("coordinator_epoch"));

  // Mutation commands address scopes by canonical path; resolve them to identity
  // through the control plane so the CLI never invents an identifier.
  mlf::ScopeId scope_id;
  const bool needs_scope = options.command == "promote" || options.command == "rollback" ||
                           options.command == "plan" || options.command == "begin" ||
                           options.command == "advance" || options.command == "fail";
  if (needs_scope && !resolve_scope(client, options.scope_path, scope_id, error)) {
    return 2;
  }

  mlf::Reply reply;
  if (options.command == "register-model") {
    if (options.model_name.empty()) {
      std::fprintf(stderr, "mlf_cli: --name is required\n");
      return 2;
    }
    mlf::RegisterModelMessage message;
    message.name = options.model_name;
    message.family = options.family;
    message.provenance = mlf::Provenance::Real;
    reply = client.call(message, error);
    if (reply.ok()) std::printf("model_id=%llu\n", static_cast<unsigned long long>(reply.u64("model_id")));
  } else if (options.command == "register-version") {
    mlf::RegisterVersionMessage message;
    message.model = mlf::ModelId(options.model_id);
    message.label = options.label;
    message.predecessor = mlf::ModelVersionId(options.predecessor);
    message.provenance = mlf::Provenance::Real;
    message.artifact.set_id = mlf::ArtifactSetId(options.artifact_set);
    message.artifact.generation = mlf::ArtifactGeneration(options.artifact_generation);
    if (!options.digest.empty()) {
      message.artifact.digest = options.digest;
    } else if (!options.artifact_path.empty()) {
      if (!mlf::compute_file_digest(options.artifact_path, message.artifact.digest)) {
        std::fprintf(stderr, "mlf_cli: cannot read artifact %s\n", options.artifact_path.c_str());
        return 1;
      }
    } else {
      std::fprintf(stderr, "mlf_cli: provide --artifact <path> or --digest <token>\n");
      return 2;
    }
    message.requirements.runtime = options.runtime;
    message.requirements.backend = options.backend;
    message.requirements.architecture = options.architecture;
    message.requirements.precision = options.precision;
    message.requirements.tokenizer_generation = options.tokenizer;
    message.requirements.adapter_set = options.adapter;
    reply = client.call(message, error);
    if (reply.ok()) {
      std::printf("version_id=%llu\n", static_cast<unsigned long long>(reply.u64("version_id")));
    }
  } else if (options.command == "publish-compatibility") {
    mlf::PublishCompatibilityMessage message;
    message.version = mlf::ModelVersionId(options.version_id);
    message.environment.key = options.environment_key;
    message.detail = "published by mlf_cli";
    message.provenance = mlf::Provenance::Real;
    if (!mlf::parse_compatibility_outcome(options.outcometext, message.outcome)) {
      std::fprintf(stderr, "mlf_cli: --outcome %s is not a known compatibility outcome\n",
                   options.outcometext.c_str());
      return 2;
    }
    reply = client.call(message, error);
  } else if (options.command == "promote") {
    mlf::PromotionMessage message;
    message.request.model = mlf::ModelId(options.model_id);
    message.request.candidate = mlf::ModelVersionId(options.version_id);
    message.request.epoch = epoch;
    message.request.scope = scope_id;
    message.request.rollback_target = mlf::ModelVersionId(options.target_version);
    message.request.environment_key = options.environment_key;
    message.request.request_immediate_cutover = options.immediate;
    message.request.administrative_approval = true;
    message.request.strategy = options.immediate ? mlf::RolloutStrategy::ImmediateCutover
                                                 : mlf::RolloutStrategy::Canary;
    reply = client.call(message, error);
    if (reply.ok()) {
      std::printf("promotion_id=%llu promotion_generation=%llu\n",
                  static_cast<unsigned long long>(reply.u64("promotion_id")),
                  static_cast<unsigned long long>(reply.u64("promotion_generation")));
    }
  } else if (options.command == "plan" || options.command == "begin" ||
             options.command == "advance" || options.command == "fail") {
    mlf::ProgressMessage message;
    message.request.rollout = mlf::RolloutId(options.rollout_id);
    message.request.epoch = epoch;
    message.request.administrative_approval = true;
    if (options.command == "plan") {
      std::fprintf(stderr, "mlf_cli: plan requires the full plan on the wire; use the API or an "
                           "example\n");
      return 2;
    }
    const mlf::MessageType type = options.command == "begin"    ? mlf::MessageType::BEGIN_ROLLOUT
                                  : options.command == "advance" ? mlf::MessageType::ADVANCE_STAGE
                                                                 : mlf::MessageType::FAIL_STAGE;
    reply = client.call_as(type, message, error);
  } else if (options.command == "rollback") {
    mlf::RollbackMessage message;
    message.request.model = mlf::ModelId(options.model_id);
    message.request.candidate = mlf::ModelVersionId(options.version_id);
    message.request.target = mlf::ModelVersionId(options.target_version);
    message.request.rollout = mlf::RolloutId(options.rollout_id);
    message.request.scope = scope_id;
    message.request.epoch = epoch;
    reply = client.call(message, error);
  } else if (options.command == "retire" || options.command == "revalidate") {
    if (options.command == "retire") {
      mlf::RetireMessage message;
      message.request.model = mlf::ModelId(options.model_id);
      message.request.version = mlf::ModelVersionId(options.version_id);
      message.request.epoch = epoch;
      reply = client.call(message, error);
    } else {
      mlf::RevalidateMessage message;
      message.request.model = mlf::ModelId(options.model_id);
      message.request.version = mlf::ModelVersionId(options.version_id);
      message.request.cause = mlf::ReasonCode::CompatibilityStale;
      message.request.detail = "requested by mlf_cli";
      reply = client.call(message, error);
    }
  } else if (options.command == "fence") {
    mlf::FenceMessage message;
    message.worker = mlf::WorkerId(options.worker_id);
    message.reason = "fenced by mlf_cli";
    reply = client.call(message, error);
  } else {
    std::fprintf(stderr, "mlf_cli: unknown command %s\n", options.command.c_str());
    usage();
    return 2;
  }

  std::printf("outcome=%s\n", std::string(mlf::to_string(reply.outcome)).c_str());
  for (const auto& entry : reply.values) {
    std::printf("  %-28s = %s\n", entry.first.c_str(), entry.second.c_str());
  }
  for (const mlf::Reason& reason : reply.reasons) {
    std::printf("  reason: %-44s %s\n", std::string(mlf::to_string(reason.code)).c_str(),
                reason.detail.c_str());
  }
  if (!error.empty()) std::fprintf(stderr, "mlf_cli: %s\n", error.c_str());
  return reply.ok() ? 0 : 1;
}

bool is_mutation(const std::string& command) {
  static const char* kCommands[] = {"register-model", "register-version", "publish-compatibility",
                                    "promote",        "plan",             "begin",
                                    "advance",        "fail",             "rollback",
                                    "retire",         "revalidate",       "fence"};
  for (const char* candidate : kCommands) {
    if (command == candidate) return true;
  }
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!parse_options(argc, argv, options)) return 2;
  std::printf("mlf_cli %s (protocol %u, state format %u)\n", std::string(mlf::kVersionString).c_str(),
              static_cast<unsigned>(mlf::kProtocolVersion),
              static_cast<unsigned>(mlf::kStateFormatVersion));
  if (is_mutation(options.command)) return mutation_command(options);
  return read_only_command(options);
}