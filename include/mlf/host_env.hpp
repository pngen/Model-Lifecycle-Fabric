// Model Lifecycle Fabric — real host environment discovery.
//
// Everything reported here is observed from the live host. Anything that cannot
// be observed is reported as UNSUPPORTED rather than assumed.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "mlf/compatibility.hpp"
#include "mlf/provenance.hpp"

namespace mlf {

struct CpuInfo {
  std::string vendor{};
  std::string brand{};
  std::uint32_t logical_cores{0};
  std::uint32_t physical_cores{0};
  std::uint64_t total_memory_bytes{0};
  bool avx2{false};
  bool avx512{false};
};

/// One device observed through the CUDA driver API.
struct GpuDevice {
  std::uint32_t index{0};
  std::string name{};
  std::uint32_t compute_major{0};
  std::uint32_t compute_minor{0};
  std::uint64_t total_memory_bytes{0};
  /// Canonical token such as "sm_120".
  std::string architecture_token{};
};

struct CudaRuntimeInfo {
  /// True when the CUDA driver library was loaded and initialized on this host.
  bool driver_present{false};
  std::uint32_t driver_version{0};
  std::string driver_version_string{};
  std::string runtime_library{};
  std::vector<GpuDevice> devices{};
  Provenance provenance{Provenance::Unsupported};
  std::string detail{};
};

struct InferenceRuntimeInfo {
  std::string name{};
  bool present{false};
  std::string path{};
  std::string detail{};
};

/// Everything discovered about the host. Every field is either observed or
/// explicitly absent.
struct HostEnvironment {
  CpuInfo cpu{};
  std::string os_name{};
  std::string os_version{};
  std::string machine{};
  std::string compiler{};
  std::string build_type{};
  std::string host{};
  std::uint64_t process_id{0};
  CudaRuntimeInfo cuda{};
  std::vector<InferenceRuntimeInfo> inference_runtimes{};
};

/// Discover the host. Performs no network access.
[[nodiscard]] HostEnvironment discover_host_environment();

/// Derive an environment profile usable as a lifecycle compatibility subject.
/// The profile records only what was actually observed.
[[nodiscard]] EnvironmentProfile to_environment_profile(const HostEnvironment& environment,
                                                        std::string key);

/// Deterministic multi-line rendering for CLI output and evidence records.
[[nodiscard]] std::string render_host_environment(const HostEnvironment& environment);

/// Convenience: true when a CUDA device with at least the given compute
/// capability was observed.
[[nodiscard]] bool has_compute_capability_at_least(const HostEnvironment& environment,
                                                   std::uint32_t major, std::uint32_t minor);

}  // namespace mlf
