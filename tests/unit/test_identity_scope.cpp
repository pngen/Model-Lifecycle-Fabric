// Identity and scope model.
#include "mlf/identity.hpp"
#include "mlf/scope.hpp"

#include "mlf_test.hpp"

using namespace mlf;

MLF_TEST(identity, generations_advance_and_saturate) {
  ModelGeneration generation(kFirstGeneration);
  MLF_CHECK_EQ(generation.raw(), 1u);
  generation = next_generation(generation);
  MLF_CHECK_EQ(generation.raw(), 2u);
  ModelGeneration saturated(~std::uint64_t{0});
  MLF_CHECK_EQ(next_generation(saturated).raw(), ~std::uint64_t{0});
}

MLF_TEST(identity, ids_compare_and_hash_consistently) {
  const ModelId a(7);
  const ModelId b(7);
  const ModelId c(8);
  MLF_CHECK(a == b);
  MLF_CHECK(a != c);
  MLF_CHECK(a < c);
  MLF_CHECK(a.valid());
  MLF_CHECK(!ModelId{}.valid());
  std::hash<ModelId> hasher;
  MLF_CHECK_EQ(hasher(a), hasher(b));
}

MLF_TEST(identity, parse_rejects_malformed_input) {
  ModelId parsed;
  MLF_CHECK(parse_id<ModelIdTag>("42", parsed));
  MLF_CHECK_EQ(parsed.raw(), 42u);
  MLF_CHECK(!parse_id<ModelIdTag>("", parsed));
  MLF_CHECK(!parse_id<ModelIdTag>("4x", parsed));
  MLF_CHECK(!parse_id<ModelIdTag>("-1", parsed));
  MLF_CHECK(!parse_id<ModelIdTag>("999999999999999999999999", parsed));
}

MLF_TEST(scope, registration_is_deterministic_and_hierarchical) {
  ScopeRegistry first;
  ScopeRegistry second;
  ScopeId a1;
  ScopeId b1;
  ScopeId c1;
  ScopeId a2;
  ScopeId b2;
  ScopeId c2;
  MLF_CHECK(first.ensure_path("global", a1).allowed());
  MLF_CHECK(first.ensure_path("global/cluster:c1/tenant:t1", c1).allowed());
  MLF_CHECK(first.ensure_path("global/cluster:c1", b1).allowed());
  MLF_CHECK(second.ensure_path("global", a2).allowed());
  MLF_CHECK(second.ensure_path("global/cluster:c1/tenant:t1", c2).allowed());
  MLF_CHECK(second.ensure_path("global/cluster:c1", b2).allowed());
  MLF_CHECK_EQ(a1.raw(), a2.raw());
  MLF_CHECK_EQ(b1.raw(), b2.raw());
  MLF_CHECK_EQ(c1.raw(), c2.raw());
  MLF_CHECK_EQ(first.size(), 3u);
  MLF_CHECK_EQ(first.path_of(c1), std::string("global/cluster:c1/tenant:t1"));
}

MLF_TEST(scope, ancestors_and_chains_are_root_first) {
  ScopeRegistry registry;
  ScopeId cluster;
  ScopeId tenant;
  MLF_CHECK(registry.ensure_path("global/cluster:c1", cluster).allowed());
  MLF_CHECK(registry.ensure_path("global/cluster:c1/tenant:t1", tenant).allowed());
  MLF_CHECK(registry.is_ancestor_or_self(registry.root(), tenant));
  MLF_CHECK(registry.is_ancestor_or_self(cluster, tenant));
  MLF_CHECK(!registry.is_ancestor_or_self(tenant, cluster));
  const std::vector<ScopeId> chain = registry.chain(tenant);
  MLF_CHECK_EQ(chain.size(), 3u);
  MLF_CHECK(chain.front() == registry.root());
  MLF_CHECK(chain.back() == tenant);
}

MLF_TEST(scope, malformed_paths_are_rejected) {
  ScopeRegistry registry;
  ScopeId out;
  MLF_CHECK(!registry.ensure_path("", out).allowed());
  MLF_CHECK(!registry.ensure_path("site:alpha", out).allowed());
  MLF_CHECK(!registry.ensure_path("global//cluster:c1", out).allowed());
  MLF_CHECK(!registry.ensure_path("global/nosuchkind:c1", out).allowed());
  MLF_CHECK(!registry.ensure_path("global/cluster:", out).allowed());
  MLF_CHECK(!registry.ensure_path("global/cluster:bad name", out).allowed());
}

MLF_TEST(scope, generation_advances_on_binding_change) {
  ScopeRegistry registry;
  ScopeId cluster;
  MLF_CHECK(registry.ensure_path("global/cluster:c1", cluster).allowed());
  const ScopeGeneration before = registry.find(cluster)->generation;
  registry.touch(cluster);
  const ScopeGeneration after = registry.find(cluster)->generation;
  MLF_CHECK(after > before);
}

MLF_TEST(scope, restore_validates_structure_and_leaves_state_untouched_on_failure) {
  ScopeRegistry source;
  ScopeId cluster;
  ScopeId tenant;
  MLF_CHECK(source.ensure_path("global/cluster:c1", cluster).allowed());
  MLF_CHECK(source.ensure_path("global/cluster:c1/tenant:t1", tenant).allowed());

  ScopeRegistry target;
  MLF_CHECK(target.restore(source.records(), source.root()));
  MLF_CHECK_EQ(target.size(), source.size());

  std::vector<ScopeRecord> broken = source.records();
  broken[2].depth = 9;
  const std::size_t before = target.size();
  MLF_CHECK(!target.restore(broken, source.root()));
  MLF_CHECK_EQ(target.size(), before);

  std::vector<ScopeRecord> unknown_parent = source.records();
  unknown_parent[1].parent = ScopeId(999);
  MLF_CHECK(!target.restore(unknown_parent, source.root()));

  MLF_CHECK(!target.restore({}, source.root()));
}

MLF_TEST(scope, bounds_are_enforced) {
  ScopeRegistry registry;
  registry.set_max_scopes(3);
  ScopeId out;
  MLF_CHECK(registry.ensure_path("global", out).allowed());
  MLF_CHECK(registry.ensure_path("global/cluster:a", out).allowed());
  MLF_CHECK(registry.ensure_path("global/cluster:b", out).allowed());
  Decision overflow = registry.ensure_path("global/cluster:c", out);
  MLF_CHECK(!overflow.allowed());
  MLF_CHECK(overflow.has(ReasonCode::BoundsScopeCount));
}
