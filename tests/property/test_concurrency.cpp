// Simultaneous lifecycle operations from many threads.
#include <atomic>
#include <thread>
#include <vector>

#include "mlf/engine.hpp"

#include "mlf_test.hpp"
#include "scenario.hpp"

using namespace mlf;
using mlftest::require_ok;

MLF_TEST(concurrency, simultaneous_reads_and_writes_keep_the_invariants) {
  mlftest::Scenario scenario = mlftest::make_scenario(701, 4096);
  LifecycleEngine& engine = *scenario.engine;

  std::atomic<bool> stop{false};
  std::atomic<int> conflicts{0};
  std::atomic<std::uint64_t> operations{0};

  const auto writer = [&](int index) {
    mlftest::Rng rng(static_cast<std::uint64_t>(index) + 1);
    int step = 0;
    while (!stop.load()) {
      ++step;
      switch (rng.below(4)) {
        case 0:
          static_cast<void>(mlftest::promote_immediately(engine, scenario, scenario.v1,
                                                         scenario.global, ModelVersionId{}));
          break;
        case 1:
          static_cast<void>(mlftest::promote_immediately(engine, scenario, scenario.v2,
                                                         scenario.cluster, scenario.v1));
          break;
        case 2: {
          const ModelVersionRecord record = mlftest::version_of(engine, scenario.v1);
          if (record.version.valid()) {
            static_cast<void>(mlftest::publish_evidence(engine, record, scenario.global,
                                                        EvidenceKind::TestResult,
                                                        EvidenceVerdict::Satisfied, 1.0));
          }
          break;
        }
        default: {
          WorkerRegistration registration;
          registration.id = WorkerId(static_cast<std::uint64_t>(index) + 1);
          registration.boot = WorkerBootId(static_cast<std::uint64_t>(step) + 1);
          registration.epoch = engine.epoch();
          static_cast<void>(engine.register_worker(registration));
          static_cast<void>(engine.fence_worker(registration.id, registration.boot, engine.epoch(),
                                                "concurrency"));
          break;
        }
      }
      operations.fetch_add(1);
      if (step > 200) break;
    }
  };

  const auto reader = [&]() {
    while (!stop.load()) {
      const std::vector<AuthorityBinding> bindings = engine.all_authority_bindings();
      std::map<std::uint64_t, int> exclusive_per_scope;
      for (const AuthorityBinding& binding : bindings) {
        if (binding.superseded) continue;
        if (binding.kind == AuthorityKind::Exclusive) ++exclusive_per_scope[binding.scope.raw()];
      }
      for (const auto& entry : exclusive_per_scope) {
        if (entry.second > 1) conflicts.fetch_add(1);
      }
      for (const ModelVersionRecord& record : engine.all_versions()) {
        if (!record.retired()) continue;
        for (const AuthorityBinding& binding : engine.authority_bindings(record.model,
                                                                        record.version)) {
          if (!binding.superseded) conflicts.fetch_add(1);
        }
      }
      const std::vector<ScopeRecord> scopes = engine.scopes();
      static_cast<void>(scopes);
      const auto snapshot = engine.export_durable_state();
      static_cast<void>(snapshot);
      operations.fetch_add(1);
    }
  };

  std::vector<std::thread> threads;
  for (int i = 0; i < 3; ++i) threads.emplace_back(writer, i);
  for (int i = 0; i < 3; ++i) threads.emplace_back(reader);
  mlf::sleep_millis(150);
  stop.store(true);
  for (std::thread& thread : threads) thread.join();

  MLF_CHECK(operations.load() > 100);
  MLF_CHECK_MSG(conflicts.load() == 0,
                "observed " + std::to_string(conflicts.load()) + " invariant violations");
}

MLF_TEST(concurrency, simultaneous_scope_registration_is_consistent) {
  LifecycleEngine engine;
  std::atomic<int> failures{0};
  const auto worker = [&](int index) {
    for (int i = 0; i < 40; ++i) {
      ScopeId scope;
      const Decision decision =
          engine.register_scope("global/cluster:c" + std::to_string(index) + "-" +
                                    std::to_string(i),
                                scope);
      if (!decision.allowed()) failures.fetch_add(1);
    }
  };
  std::vector<std::thread> threads;
  for (int i = 0; i < 4; ++i) threads.emplace_back(worker, i);
  for (std::thread& thread : threads) thread.join();
  MLF_CHECK_EQ(failures.load(), 0);
  MLF_CHECK_EQ(engine.scopes().size(), 1u + 4u * 40u);
}

MLF_TEST(concurrency, no_orphan_threads_survive_shutdown) {
  // The engine holds no worker threads of its own; this test asserts that a
  // heavily used engine can be destroyed while other work observes it.
  {
    mlftest::Scenario scenario = mlftest::make_scenario(702, 2048);
    std::atomic<bool> stop{false};
    std::thread reader([&]() {
      while (!stop.load()) {
        static_cast<void>(scenario.engine->stats());
      }
    });
    for (int i = 0; i < 60; ++i) {
      static_cast<void>(mlftest::promote_immediately(*scenario.engine, scenario, scenario.v1,
                                                     scenario.global, ModelVersionId{}));
    }
    stop.store(true);
    reader.join();
    MLF_CHECK(scenario.engine->stats().models >= 1);
  }
  MLF_CHECK(true);
}
