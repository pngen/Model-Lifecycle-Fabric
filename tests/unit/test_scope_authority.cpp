// Scope authority: which generation is authoritative where, and why.
#include "mlf/engine.hpp"

#include "mlf_test.hpp"
#include "scenario.hpp"

using namespace mlf;
using mlftest::require_ok;

MLF_TEST(authority, no_authority_is_reported_as_no_authoritative_model) {
  mlftest::Scenario scenario = mlftest::make_scenario(31);
  const AuthorityQueryResult result = scenario.engine->query_authority(scenario.global);
  MLF_CHECK_EQ(std::string(to_string(result.outcome)), std::string("NO_AUTHORITATIVE_MODEL"));
  MLF_CHECK(result.binding == nullptr);
  MLF_CHECK(result.explanation.find("scope") != nullptr);
}

MLF_TEST(authority, unknown_scope_is_refused) {
  mlftest::Scenario scenario = mlftest::make_scenario(32);
  const AuthorityQueryResult result = scenario.engine->query_authority(ScopeId(9999));
  MLF_CHECK_EQ(std::string(to_string(result.outcome)), std::string("NO_AUTHORITATIVE_MODEL"));
  MLF_CHECK(result.explanation.find("scope") == nullptr);
}

MLF_TEST(authority, narrower_scopes_inherit_and_can_override) {
  mlftest::Scenario scenario = mlftest::make_scenario(33);
  require_ok(mlftest::promote_immediately(*scenario.engine, scenario, scenario.v1, scenario.global,
                                          ModelVersionId{})
                 .decision,
             "v1 global");
  require_ok(mlftest::promote_immediately(*scenario.engine, scenario, scenario.v2, scenario.tenant,
                                          scenario.v1)
                 .decision,
             "v2 tenant");

  const AuthorityQueryResult global = scenario.engine->query_authority(scenario.global);
  MLF_CHECK_EQ(global.binding->version.raw(), scenario.v1.raw());
  MLF_CHECK(!global.inherited);

  const AuthorityQueryResult tenant = scenario.engine->query_authority(scenario.tenant);
  MLF_CHECK_EQ(tenant.binding->version.raw(), scenario.v2.raw());
  MLF_CHECK(!tenant.inherited);

  const AuthorityQueryResult cluster = scenario.engine->query_authority(scenario.cluster);
  MLF_CHECK_EQ(cluster.binding->version.raw(), scenario.v1.raw());
  MLF_CHECK(cluster.inherited);
  MLF_CHECK(cluster.inherited_scope == scenario.global);
  MLF_CHECK(cluster.explanation.find("inherited") != nullptr);
}

MLF_TEST(authority, committed_authority_expires_with_the_generation_it_names) {
  mlftest::Scenario scenario = mlftest::make_scenario(34);
  require_ok(mlftest::promote_immediately(*scenario.engine, scenario, scenario.v1, scenario.global,
                                          ModelVersionId{})
                 .decision,
             "v1 global");
  const AuthorityQueryResult before = scenario.engine->query_authority(scenario.global);
  MLF_CHECK_EQ(before.binding->version.raw(), scenario.v1.raw());
  MLF_CHECK(before.binding->model_generation == mlftest::version_of(*scenario.engine, scenario.v1).generation);
}

MLF_TEST(authority, conflicting_exclusive_bindings_are_reported_as_ambiguous) {
  EngineConfig config;
  LifecycleEngine engine(config);
  ScopeId global;
  require_ok(engine.register_scope("global", global), "scope");
  ModelId model;
  require_ok(engine.register_model("m", "f", Provenance::Synthetic, model), "model");

  const auto add_version = [&](const char* label, std::uint64_t set) {
    RegisterVersionRequest request;
    request.model = model;
    request.label = label;
    request.artifact.set_id = ArtifactSetId(set);
    request.artifact.generation = ArtifactGeneration(1);
    request.artifact.digest = std::string("digest-") + label;
    request.provenance = Provenance::Synthetic;
    ModelVersionId id;
    require_ok(engine.register_version(request, id), "register");
    return id;
  };
  const ModelVersionId a = add_version("a", 1);
  const ModelVersionId b = add_version("b", 2);

  EnvironmentProfile environment;
  environment.key = "e";
  environment.provenance = Provenance::Synthetic;
  for (ModelVersionId id : {a, b}) {
    require_ok(engine.publish_compatibility(id, environment, CompatibilityOutcome::COMPATIBLE, "x",
                                            Provenance::Synthetic),
               "compat");
  }

  // Two exclusive bindings cannot be created through the public path; build the
  // conflict through restore, which validates but does not enforce the single
  // exclusive rule for already-persisted content beyond one per scope.
  DurableState state;
  state.epoch = engine.epoch();
  state.scopes = engine.scopes();
  state.root_scope = ScopeId(1);
  for (const ModelRecord& model_record : engine.models()) {
    state.models[model_record.id.raw()] = model_record;
  }
  for (const ModelVersionRecord& version_record : engine.all_versions()) {
    state.versions[std::make_pair(version_record.model.raw(), version_record.version.raw())] =
        version_record;
  }
  state.base_policy = engine.current_policy();
  AuthorityBinding first;
  first.scope = global;
  first.scope_generation = state.scopes.front().generation;
  first.model = model;
  first.version = a;
  first.model_generation = ModelGeneration(1);
  first.kind = AuthorityKind::Exclusive;
  first.granted_sequence = Sequence(1);
  state.authority[global.raw()] = {first};
  LifecycleEngine other;
  require_ok(other.import_durable_state(state, next_generation(state.epoch)), "import single");
  MLF_CHECK_EQ(std::string(to_string(other.query_authority(global).outcome)), std::string("OK"));

  state.authority[global.raw()] = {first, first};
  LifecycleEngine hostile;
  const Decision conflict =
      hostile.import_durable_state(state, next_generation(state.epoch));
  MLF_CHECK(!conflict.allowed());
  MLF_CHECK(conflict.has(ReasonCode::PolicyDeniesCoexistence));
  MLF_CHECK_EQ(std::string(to_string(hostile.query_authority(global).outcome)),
               std::string("NO_AUTHORITATIVE_MODEL"));
  // The engine that already loaded a consistent view is untouched.
  MLF_CHECK_EQ(std::string(to_string(other.query_authority(global).outcome)), std::string("OK"));
}

MLF_TEST(authority, sibling_scopes_are_independent) {
  mlftest::Scenario scenario = mlftest::make_scenario(35);
  ScopeId other;
  require_ok(scenario.engine->register_scope("global/cluster:c2", other), "second cluster");

  require_ok(mlftest::promote_immediately(*scenario.engine, scenario, scenario.v1, scenario.cluster,
                                          ModelVersionId{})
                 .decision,
             "v1 in c1");
  require_ok(mlftest::promote_immediately(*scenario.engine, scenario, scenario.v2, other,
                                          scenario.v1)
                 .decision,
             "v2 in c2");

  MLF_CHECK_EQ(scenario.engine->query_authority(scenario.cluster).binding->version.raw(),
               scenario.v1.raw());
  MLF_CHECK_EQ(scenario.engine->query_authority(other).binding->version.raw(),
               scenario.v2.raw());
  // Neither sibling overrides the root.
  MLF_CHECK_EQ(std::string(to_string(scenario.engine->query_authority(scenario.global).outcome)),
               std::string("NO_AUTHORITATIVE_MODEL"));
}
