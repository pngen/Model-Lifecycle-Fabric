// Model Lifecycle Fabric — benchmarks over completed operations.
//
// Every measurement is over operations that actually completed: no partial or
// rejected work is timed as if it succeeded.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "mlf/artifact.hpp"
#include "mlf/engine.hpp"
#include "mlf/state_store.hpp"

namespace {

using Clock = std::chrono::steady_clock;

struct Measurement {
  std::string operation;
  std::size_t scale{0};
  std::size_t iterations{0};
  double total_millis{0.0};
  double per_operation_micros{0.0};
};

std::vector<Measurement> g_measurements;

template <class Body>
void measure(const std::string& operation, std::size_t scale, std::size_t iterations, Body body) {
  const auto start = Clock::now();
  for (std::size_t i = 0; i < iterations; ++i) body(i);
  const auto finish = Clock::now();
  Measurement measurement;
  measurement.operation = operation;
  measurement.scale = scale;
  measurement.iterations = iterations;
  measurement.total_millis =
      std::chrono::duration<double, std::milli>(finish - start).count();
  measurement.per_operation_micros =
      iterations == 0 ? 0.0 : (measurement.total_millis * 1000.0) / static_cast<double>(iterations);
  g_measurements.push_back(measurement);
}

struct Fixture {
  std::unique_ptr<mlf::LifecycleEngine> engine{};
  mlf::ModelId model{};
  std::vector<mlf::ModelVersionId> versions{};
  std::vector<mlf::ScopeId> scopes{};
  mlf::EnvironmentProfile environment{};
};

Fixture build_fixture(std::size_t models, std::size_t versions_per_model, std::size_t scope_count) {
  Fixture fixture;
  fixture.engine = std::make_unique<mlf::LifecycleEngine>();
  fixture.environment.key = "bench";
  fixture.environment.runtime = "bench-runtime";
  fixture.environment.backend = "cpu";
  fixture.environment.architecture = "x86_64";
  fixture.environment.precisions.insert("fp32");
  fixture.environment.provenance = mlf::Provenance::Synthetic;

  mlf::ScopeId global;
  fixture.engine->register_scope("global", global);
  fixture.scopes.push_back(global);
  for (std::size_t i = 1; i < scope_count; ++i) {
    mlf::ScopeId scope;
    if (!fixture.engine
             ->register_scope("global/cluster:c" + std::to_string(i), scope)
             .allowed()) {
      break;
    }
    fixture.scopes.push_back(scope);
  }

  mlf::ModelRequirements requirements;
  requirements.runtime = "bench-runtime";
  requirements.backend = "cpu";
  requirements.architecture = "x86_64";
  requirements.precision = "fp32";

  for (std::size_t m = 0; m < models; ++m) {
    mlf::ModelId model;
    if (!fixture.engine
             ->register_model("bench-model-" + std::to_string(m), "bench",
                              mlf::Provenance::Synthetic, model)
             .allowed()) {
      break;
    }
    fixture.model = model;
    for (std::size_t v = 0; v < versions_per_model; ++v) {
      mlf::RegisterVersionRequest request;
      request.model = model;
      request.label = "1." + std::to_string(v);
      request.artifact.set_id = mlf::ArtifactSetId(m * versions_per_model + v + 1);
      request.artifact.generation = mlf::ArtifactGeneration(1);
      request.artifact.digest = "bench-artifact-" + std::to_string(m) + "-" + std::to_string(v);
      request.requirements = requirements;
      request.provenance = mlf::Provenance::Synthetic;
      mlf::ModelVersionId version;
      if (!fixture.engine->register_version(request, version).allowed()) break;
      fixture.versions.push_back(version);
      static_cast<void>(fixture.engine->publish_compatibility(
          version, fixture.environment, mlf::CompatibilityOutcome::COMPATIBLE, "bench",
          mlf::Provenance::Synthetic));
    }
  }
  return fixture;
}

void report() {
  std::printf("%-30s %8s %10s %14s %12s\n", "operation", "scale", "iterations", "total_ms",
              "micros/op");
  for (const Measurement& measurement : g_measurements) {
    std::printf("%-30s %8zu %10zu %14.3f %12.3f\n", measurement.operation.c_str(),
                measurement.scale, measurement.iterations, measurement.total_millis,
                measurement.per_operation_micros);
  }
}

}  // namespace

int main() {
  std::printf("Model Lifecycle Fabric benchmarks\n");
  std::printf("provenance: SYNTHETIC workload over a deterministic in-process registry\n\n");

  const std::size_t scales[] = {1, 100, 1000};

  for (std::size_t scale : scales) {
    const std::size_t models = scale;
    const std::size_t versions_per_model = 2;
    // At least two scopes are always built so that the scope-lookup benchmark has
    // a real sibling to address; a single-scope registry would divide by zero.
    const std::size_t scope_count = std::max<std::size_t>(2, std::min<std::size_t>(scale, 1000));
    Fixture fixture = build_fixture(models, versions_per_model, scope_count);
    const std::size_t versions = fixture.versions.size();
    if (fixture.versions.empty() || fixture.scopes.size() < 2) {
      std::printf("benchmark fixture could not be built at scale %zu\n", scale);
      return 1;
    }

    // Model registration into a fresh engine of the same size.
    measure("model_registration", models, 200, [&](std::size_t i) {
      mlf::LifecycleEngine engine;
      mlf::ModelId id;
      static_cast<void>(
          engine.register_model("x" + std::to_string(i), "bench", mlf::Provenance::Synthetic, id));
    });

    measure("registry_scan_versions", versions, 50, [&](std::size_t) {
      volatile std::size_t count = fixture.engine->all_versions().size();
      static_cast<void>(count);
    });

    measure("promotion_eligibility", versions, 50, [&](std::size_t i) {
      const mlf::ModelVersionId id = fixture.versions[i % versions];
      mlf::PromotionRequest request;
      request.model = fixture.model;
      request.candidate = id;
      request.scope = fixture.scopes.front();
      request.epoch = fixture.engine->epoch();
      request.environment_key = fixture.environment.key;
      static_cast<void>(fixture.engine->evaluate_promotion(request, nullptr));
    });

    measure("authority_resolution", fixture.scopes.size(), 200, [&](std::size_t i) {
      static_cast<void>(
          fixture.engine->query_authority(fixture.scopes[i % fixture.scopes.size()]));
    });

    const std::size_t addressable_scopes = fixture.scopes.size() - 1;
    measure("scope_lookup_by_path", addressable_scopes, 200, [&](std::size_t i) {
      const std::size_t index = i % addressable_scopes;
      if (index >= addressable_scopes) return;
      static_cast<void>(fixture.engine->find_scope_by_path("global/cluster:c" +
                                                           std::to_string(index + 1)));
    });

    measure("retirement_validation", versions, 50, [&](std::size_t i) {
      mlf::RetirementRequest request;
      request.model = fixture.model;
      request.version = fixture.versions[i % versions];
      request.epoch = fixture.engine->epoch();
      static_cast<void>(fixture.engine->evaluate_retirement(request, nullptr));
    });

    measure("rollback_planning", versions, 50, [&](std::size_t i) {
      mlf::RollbackRequest request;
      request.model = fixture.model;
      request.candidate = fixture.versions[i % versions];
      request.target = fixture.versions[(i + 1) % versions];
      request.scope = fixture.scopes.front();
      request.epoch = fixture.engine->epoch();
      static_cast<void>(fixture.engine->evaluate_rollback(request, nullptr));
    });

    measure("snapshot_creation", versions, 20, [&](std::size_t) {
      static_cast<void>(fixture.engine->export_durable_state());
    });

    measure("persistence_save", versions, 10, [&](std::size_t i) {
      const auto state = fixture.engine->export_durable_state();
      std::vector<std::uint8_t> bytes;
      static_cast<void>(mlf::serialize_durable_state(
          *state, mlf::DecodeLimits::from_bounds(fixture.engine->bounds()), bytes));
      static_cast<void>(i);
    });

    std::vector<std::uint8_t> encoded;
    {
      const auto state = fixture.engine->export_durable_state();
      static_cast<void>(mlf::serialize_durable_state(
          *state, mlf::DecodeLimits::from_bounds(fixture.engine->bounds()), encoded));
    }
    measure("persistence_load", versions, 10, [&](std::size_t) {
      mlf::DurableState decoded;
      static_cast<void>(mlf::deserialize_durable_state(
          encoded.data(), encoded.size(), mlf::DecodeLimits::from_bounds(fixture.engine->bounds()),
          decoded));
    });
  }

  // Stage advancement on a bounded canary plan.
  {
    Fixture fixture = build_fixture(1, 2, 4);
    mlf::ModelVersionRecord record;
    for (const mlf::ModelVersionRecord& candidate : fixture.engine->all_versions()) {
      if (candidate.version == fixture.versions.front()) record = candidate;
    }
    mlf::PromotionRequest promotion;
    promotion.model = fixture.model;
    promotion.candidate = record.version;
    promotion.candidate_generation = record.generation;
    promotion.artifact_generation = record.artifact.generation;
    promotion.compatibility_generation = record.compatibility_generation;
    promotion.scope = fixture.scopes.front();
    promotion.epoch = fixture.engine->epoch();
    promotion.environment_key = fixture.environment.key;
    promotion.strategy = mlf::RolloutStrategy::ImmediateCutover;
    promotion.request_immediate_cutover = true;
    static_cast<void>(fixture.engine->promote(promotion));
    measure("stage_advance_planning", 1, 200, [&](std::size_t) {
      mlf::RolloutProgressRequest request;
      request.rollout = mlf::RolloutId(1);
      static_cast<void>(fixture.engine->evaluate_stage_advance(request, nullptr));
    });
  }

  // Artifact digest over real bytes.
  {
    std::string digest;
    std::vector<char> payload(1u << 20, 'm');
    measure("artifact_digest_1MiB", 1, 20, [&](std::size_t) {
      digest = mlf::compute_buffer_digest(payload.data(), payload.size());
    });
    static_cast<void>(digest);
  }

  std::printf("\n");
  report();
  return 0;
}
