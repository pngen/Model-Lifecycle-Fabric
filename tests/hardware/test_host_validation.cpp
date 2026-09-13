// REAL host and CUDA capability validation.
#include "mlf/host_env.hpp"

#include "mlf_test.hpp"

using namespace mlf;

MLF_TEST(hardware, host_environment_is_discovered_from_the_live_machine) {
  const HostEnvironment environment = discover_host_environment();
  MLF_CHECK(!environment.host.empty());
  MLF_CHECK(environment.process_id != 0);
  MLF_CHECK(!environment.os_name.empty());
  MLF_CHECK(!environment.machine.empty());
  MLF_CHECK(environment.cpu.logical_cores > 0);
  MLF_CHECK(environment.cpu.physical_cores > 0);
  MLF_CHECK(environment.cpu.total_memory_bytes > 0);
  MLF_CHECK(!environment.compiler.empty());
  MLF_CHECK(environment.build_type == "Release" || environment.build_type == "Debug");
  std::printf("host report (REAL discovery):\n%s",
              render_host_environment(environment).c_str());
}

MLF_TEST(hardware, cuda_capability_is_either_observed_or_honestly_unsupported) {
  const HostEnvironment environment = discover_host_environment();
  const Provenance provenance = environment.cuda.provenance;
  MLF_CHECK(provenance == Provenance::Real || provenance == Provenance::Unsupported);
  if (provenance == Provenance::Real) {
    MLF_CHECK(environment.cuda.driver_present);
    MLF_CHECK(!environment.cuda.devices.empty());
    for (const GpuDevice& device : environment.cuda.devices) {
      MLF_CHECK(!device.name.empty());
      MLF_CHECK(!device.architecture_token.empty());
      MLF_CHECK_EQ(device.architecture_token,
                   cuda_architecture_token(device.compute_major, device.compute_minor));
      std::printf("gpu[%u] %s %s vram=%llu\n", device.index, device.name.c_str(),
                  device.architecture_token.c_str(),
                  static_cast<unsigned long long>(device.total_memory_bytes));
    }
  } else {
    MLF_CHECK(!environment.cuda.devices.empty() == false);
    std::printf("cuda capability: UNSUPPORTED (%s)\n", environment.cuda.detail.c_str());
  }
}

namespace {

ModelRequirements requirement_for(const std::string& architecture, const std::string& backend) {
  ModelRequirements requirements;
  requirements.runtime = "cuda-driver";
  requirements.backend = backend;
  requirements.architecture = architecture;
  requirements.precision = "fp8";
  return requirements;
}

}  // namespace

MLF_TEST(hardware, a_model_requirement_is_bound_to_real_device_capability) {
  const HostEnvironment environment = discover_host_environment();
  if (environment.cuda.provenance != Provenance::Real) {
    // The device capability proof cannot be made on this host; the gap is
    // recorded rather than filled with a synthetic claim.
    std::printf("device capability proof: UNSUPPORTED on this host\n");
    return;
  }
  const EnvironmentProfile profile = to_environment_profile(environment, "host");
  MLF_CHECK_EQ(profile.provenance, Provenance::Real);
  MLF_CHECK(profile.compute_capability != 0);

  const GpuDevice& device = environment.cuda.devices.front();
  const ModelRequirements compatible = requirement_for(device.architecture_token, "cuda");
  const CompatibilityResult accepted = evaluate_requirements(compatible, profile);
  MLF_CHECK_EQ(std::string(to_string(accepted.outcome)), std::string("COMPATIBLE"));
  MLF_CHECK(accepted.decision.allowed());

  // A capability this host does not have must be refused.
  const ModelRequirements future = requirement_for("sm_999", "cuda");
  const CompatibilityResult refused = evaluate_requirements(future, profile);
  MLF_CHECK_EQ(std::string(to_string(refused.outcome)),
               std::string("INCOMPATIBLE_ARCHITECTURE"));
  MLF_CHECK(!refused.decision.allowed());

  // A backend this host does not run must be refused.
  const ModelRequirements wrong_backend = requirement_for(device.architecture_token, "rocm");
  const CompatibilityResult backend = evaluate_requirements(wrong_backend, profile);
  MLF_CHECK_EQ(std::string(to_string(backend.outcome)), std::string("INCOMPATIBLE_BACKEND"));

  // The environment profile only advertises precisions the device really has.
  const ModelRequirements exotic = requirement_for(device.architecture_token, "cuda");
  ModelRequirements tf32 = exotic;
  tf32.precision = "tf32";
  const CompatibilityResult precision = evaluate_requirements(tf32, profile);
  MLF_CHECK_EQ(std::string(to_string(precision.outcome)), std::string("INCOMPATIBLE_PRECISION"));

  std::printf("device capability proof: REAL (%s, capability %u.%u)\n", device.name.c_str(),
              device.compute_major, device.compute_minor);
}

MLF_TEST(hardware, compute_capability_predicate_agrees_with_observed_devices) {
  const HostEnvironment environment = discover_host_environment();
  if (environment.cuda.devices.empty()) {
    MLF_CHECK(!has_compute_capability_at_least(environment, 1, 0));
    return;
  }
  const GpuDevice& device = environment.cuda.devices.front();
  MLF_CHECK(has_compute_capability_at_least(environment, device.compute_major, device.compute_minor));
  MLF_CHECK(!has_compute_capability_at_least(environment, device.compute_major + 1, 0));
}

MLF_TEST(hardware, inference_runtime_presence_is_reported_honestly) {
  const HostEnvironment environment = discover_host_environment();
  MLF_CHECK_EQ(environment.inference_runtimes.size(), 4u);
  for (const InferenceRuntimeInfo& runtime : environment.inference_runtimes) {
    MLF_CHECK(!runtime.name.empty());
    MLF_CHECK(!runtime.detail.empty());
    std::printf("runtime %-14s present=%s\n", runtime.name.c_str(),
                runtime.present ? "yes" : "no");
  }
}
