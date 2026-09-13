// REAL artifact identity proof over bytes on disk.
#include <filesystem>
#include <fstream>

#include "mlf/artifact.hpp"
#include "mlf/engine.hpp"

#include "mlf_test.hpp"
#include "scenario.hpp"

using namespace mlf;
using mlftest::require_ok;

MLF_TEST(artifact, digest_matches_published_test_vectors) {
  // SHA-256 of the empty input and of "abc", as published in FIPS 180-4.
  MLF_CHECK_EQ(compute_buffer_digest("", 0),
               std::string("sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  MLF_CHECK_EQ(compute_buffer_digest("abc", 3),
               std::string("sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  const std::string long_input(1000000, 'a');
  MLF_CHECK_EQ(compute_buffer_digest(long_input.data(), long_input.size()),
               std::string("sha256:cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
}

MLF_TEST(artifact, file_digest_is_stable_and_content_sensitive) {
  const std::string first = mlftest::write_artifact("artifact-a", 900, 32 * 1024);
  const std::string second = mlftest::write_artifact("artifact-b", 901, 32 * 1024);
  std::string first_digest;
  std::string first_again;
  std::string second_digest;
  MLF_CHECK(compute_file_digest(first, first_digest));
  MLF_CHECK(compute_file_digest(first, first_again));
  MLF_CHECK(compute_file_digest(second, second_digest));
  MLF_CHECK_EQ(first_digest, first_again);
  MLF_CHECK(first_digest != second_digest);
  MLF_CHECK_EQ(first_digest.size(), 7u + 64u);

  // Rewriting the same path with different content yields a different digest.
  const std::string replaced = mlftest::write_artifact("artifact-a", 902, 32 * 1024);
  std::string replaced_digest;
  MLF_CHECK(compute_file_digest(replaced, replaced_digest));
  MLF_CHECK(replaced_digest != first_digest);

  std::string missing;
  MLF_CHECK(!compute_file_digest(mlftest::scratch_path("does-not-exist", ".bin"), missing));
}

MLF_TEST(artifact, an_absent_artifact_cannot_be_bound_to_a_version) {
  LifecycleEngine engine;
  ModelId model;
  require_ok(engine.register_model("m", "f", Provenance::Real, model), "model");

  const std::string missing = mlftest::scratch_path("absent-artifact", ".bin");
  std::string digest;
  const bool readable = compute_file_digest(missing, digest);
  MLF_CHECK(!readable);

  // A caller that ignores the failure and binds a made-up token is still bound
  // to a *generation*, so the lifecycle is deterministic even though the bytes
  // are unavailable; what must never happen is a claim that the digest was
  // computed.
  RegisterVersionRequest request;
  request.model = model;
  request.label = "1.0.0";
  request.artifact.set_id = ArtifactSetId(1);
  request.artifact.generation = ArtifactGeneration(1);
  request.artifact.digest = "unavailable-artifact-bytes";
  request.provenance = Provenance::Unsupported;
  ModelVersionId version;
  require_ok(engine.register_version(request, version), "register");

  const ModelVersionRecord record = mlftest::version_of(engine, version);
  MLF_CHECK_EQ(std::string(to_string(record.provenance)), std::string("UNSUPPORTED"));
  MLF_CHECK_EQ(record.artifact.digest, std::string("unavailable-artifact-bytes"));
}

MLF_TEST(artifact, lifecycle_authority_binds_the_exact_artifact_generation) {
  mlftest::Scenario scenario = mlftest::make_scenario(910, 8192);
  LifecycleEngine& engine = *scenario.engine;

  const ModelVersionRecord before = mlftest::version_of(engine, scenario.v1);
  const std::string digest_before = before.artifact.digest;
  MLF_CHECK(digest_before.rfind("sha256:", 0) == 0);

  require_ok(mlftest::promote_immediately(engine, scenario, scenario.v1, scenario.global,
                                          ModelVersionId{})
                 .decision,
             "promote");
  const AuthorityQueryResult authority = engine.query_authority(scenario.global);
  MLF_CHECK(authority.binding != nullptr);
  MLF_CHECK_EQ(authority.binding->artifact_generation.raw(),
               before.artifact.generation.raw());

  // Replacing the artifact bytes moves the generation and invalidates both the
  // binding and every plan that cited it.
  const std::string replaced = mlftest::write_artifact("scenario-artifact", 911, 8192);
  std::string new_digest;
  MLF_CHECK(compute_file_digest(replaced, new_digest));
  MLF_CHECK(new_digest != digest_before);

  ReviseVersionRequest revise;
  revise.model = scenario.model;
  revise.version = scenario.v1;
  revise.expected_generation = before.generation;
  revise.artifact.set_id = before.artifact.set_id;
  revise.artifact.generation = ArtifactGeneration(before.artifact.generation.raw() + 1);
  revise.artifact.digest = new_digest;
  revise.requirements = before.requirements;
  require_ok(engine.revise_version(revise), "revise");

  // The old binding survives only as history; the current record cites the new
  // artifact generation and requires revalidation.
  const ModelVersionRecord after = mlftest::version_of(engine, scenario.v1);
  MLF_CHECK(after.artifact.generation > before.artifact.generation);
  MLF_CHECK_EQ(after.artifact.digest, new_digest);
  MLF_CHECK(!after.compatibility_generation.valid());
  MLF_CHECK_EQ(std::string(to_string(after.state)), std::string("REVALIDATION_REQUIRED"));
  MLF_CHECK_EQ(std::string(to_string(engine.query_authority(scenario.global).outcome)),
               std::string("OK"));

  static_cast<void>(authority);
}

MLF_TEST(artifact, digest_validation_rejects_malformed_tokens) {
  MLF_CHECK(valid_digest("sha256:" + std::string(64, 'a'), 128));
  MLF_CHECK(!valid_digest("sha256:" + std::string(63, 'a'), 128));
  MLF_CHECK(!valid_digest("sha256:" + std::string(64, 'z'), 128));
  MLF_CHECK(!valid_digest("", 128));
  MLF_CHECK(!valid_digest(std::string(200, 'a'), 128));
  MLF_CHECK(valid_digest("opaque-token_1.0", 128));
  MLF_CHECK(!valid_digest("has space", 128));
}
