#include "mlf/host_env.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sstream>

#include "mlf/process.hpp"
#include "mlf/version.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <intrin.h>
#else
#include <dlfcn.h>
#include <sys/sysinfo.h>
#include <unistd.h>
#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#endif
#endif

namespace mlf {
namespace {

// ---------------------------------------------------------------------------
// CUDA driver API, loaded dynamically.
//
// The declarations below use only the ABI-stable subset that has not changed
// since CUDA 3.2. Loading the driver at run time keeps the library free of any
// build-time CUDA dependency: on a host without the driver the probe reports
// UNSUPPORTED instead of failing to build or to start.
// ---------------------------------------------------------------------------
#ifdef _WIN32
#define MLF_CUDA_CALL __stdcall
#else
#define MLF_CUDA_CALL
#endif

using CuResult = int;
using CuDevice = int;

using CuInitFn = CuResult(MLF_CUDA_CALL*)(unsigned int);
using CuDeviceGetCountFn = CuResult(MLF_CUDA_CALL*)(int*);
using CuDeviceGetFn = CuResult(MLF_CUDA_CALL*)(CuDevice*, int);
using CuDeviceGetNameFn = CuResult(MLF_CUDA_CALL*)(char*, int, CuDevice);
using CuDeviceGetAttributeFn = CuResult(MLF_CUDA_CALL*)(int*, int, CuDevice);
using CuDeviceTotalMemFn = CuResult(MLF_CUDA_CALL*)(std::size_t*, CuDevice);
using CuDriverGetVersionFn = CuResult(MLF_CUDA_CALL*)(int*);

constexpr int kAttrName = 13;
constexpr int kAttrComputeCapabilityMajor = 75;
constexpr int kAttrComputeCapabilityMinor = 76;

struct CudaLibrary {
  void* handle{nullptr};
  CuInitFn init{nullptr};
  CuDeviceGetCountFn device_count{nullptr};
  CuDeviceGetFn device_get{nullptr};
  CuDeviceGetNameFn device_name{nullptr};
  CuDeviceGetAttributeFn device_attribute{nullptr};
  CuDeviceTotalMemFn device_total_mem{nullptr};
  CuDriverGetVersionFn driver_version{nullptr};

  [[nodiscard]] bool loaded() const noexcept { return handle != nullptr; }
};

void* load_library(const char* name) {
#ifdef _WIN32
  return reinterpret_cast<void*>(::LoadLibraryA(name));
#else
  return ::dlopen(name, RTLD_NOW | RTLD_LOCAL);
#endif
}

void* load_symbol(void* handle, const char* name) {
#ifdef _WIN32
  return reinterpret_cast<void*>(::GetProcAddress(reinterpret_cast<HMODULE>(handle), name));
#else
  return ::dlsym(handle, name);
#endif
}

CudaLibrary load_cuda() {
  CudaLibrary library;
#ifdef _WIN32
  const char* candidates[] = {"nvcuda.dll"};
#else
  const char* candidates[] = {"libcuda.so.1", "libcuda.so"};
#endif
  for (const char* candidate : candidates) {
    library.handle = load_library(candidate);
    if (library.handle != nullptr) break;
  }
  if (library.handle == nullptr) return library;
  library.init = reinterpret_cast<CuInitFn>(load_symbol(library.handle, "cuInit"));
  library.device_count =
      reinterpret_cast<CuDeviceGetCountFn>(load_symbol(library.handle, "cuDeviceGetCount"));
  library.device_get = reinterpret_cast<CuDeviceGetFn>(load_symbol(library.handle, "cuDeviceGet"));
  library.device_name =
      reinterpret_cast<CuDeviceGetNameFn>(load_symbol(library.handle, "cuDeviceGetName"));
  library.device_attribute =
      reinterpret_cast<CuDeviceGetAttributeFn>(load_symbol(library.handle, "cuDeviceGetAttribute"));
  library.device_total_mem =
      reinterpret_cast<CuDeviceTotalMemFn>(load_symbol(library.handle, "cuDeviceTotalMem_v2"));
  library.driver_version =
      reinterpret_cast<CuDriverGetVersionFn>(load_symbol(library.handle, "cuDriverGetVersion"));
  if (library.device_total_mem == nullptr) {
    library.device_total_mem =
        reinterpret_cast<CuDeviceTotalMemFn>(load_symbol(library.handle, "cuDeviceTotalMem"));
  }
  const bool complete = library.init != nullptr && library.device_count != nullptr &&
                        library.device_get != nullptr && library.device_name != nullptr &&
                        library.device_attribute != nullptr;
  if (!complete) {
    library.handle = nullptr;
  }
  return library;
}

// ---------------------------------------------------------------------------
// CPU identification
// ---------------------------------------------------------------------------
struct CpuidResult {
  std::uint32_t eax{0};
  std::uint32_t ebx{0};
  std::uint32_t ecx{0};
  std::uint32_t edx{0};
};

CpuidResult cpuid_leaf(std::uint32_t leaf, std::uint32_t subleaf) {
  CpuidResult result;
#if defined(_MSC_VER)
  int registers[4] = {0, 0, 0, 0};
  __cpuidex(registers, static_cast<int>(leaf), static_cast<int>(subleaf));
  result.eax = static_cast<std::uint32_t>(registers[0]);
  result.ebx = static_cast<std::uint32_t>(registers[1]);
  result.ecx = static_cast<std::uint32_t>(registers[2]);
  result.edx = static_cast<std::uint32_t>(registers[3]);
#elif defined(__x86_64__) || defined(__i386__)
  unsigned int a = 0;
  unsigned int b = 0;
  unsigned int c = 0;
  unsigned int d = 0;
  if (__get_cpuid_count(leaf, subleaf, &a, &b, &c, &d) != 0) {
    result.eax = a;
    result.ebx = b;
    result.ecx = c;
    result.edx = d;
  }
#else
  static_cast<void>(leaf);
  static_cast<void>(subleaf);
#endif
  return result;
}

std::string trim(std::string value) {
  const auto not_space = [](unsigned char c) { return c != ' ' && c != '\t' && c != '\0'; };
  while (!value.empty() && !not_space(static_cast<unsigned char>(value.front()))) {
    value.erase(value.begin());
  }
  while (!value.empty() && !not_space(static_cast<unsigned char>(value.back()))) {
    value.pop_back();
  }
  return value;
}

CpuInfo discover_cpu() {
  CpuInfo info;
  const CpuidResult vendor_leaf = cpuid_leaf(0, 0);
  char vendor[13] = {};
  std::memcpy(vendor + 0, &vendor_leaf.ebx, 4);
  std::memcpy(vendor + 4, &vendor_leaf.edx, 4);
  std::memcpy(vendor + 8, &vendor_leaf.ecx, 4);
  info.vendor = trim(std::string(vendor));

  const std::uint32_t max_leaf = vendor_leaf.eax;
  const std::uint32_t max_extended_leaf = cpuid_leaf(0x80000000u, 0).eax;
  if (max_extended_leaf >= 0x80000004u) {
    char brand[49] = {};
    for (std::uint32_t i = 0; i < 3; ++i) {
      const CpuidResult leaf = cpuid_leaf(0x80000002u + i, 0);
      std::memcpy(brand + i * 16 + 0, &leaf.eax, 4);
      std::memcpy(brand + i * 16 + 4, &leaf.ebx, 4);
      std::memcpy(brand + i * 16 + 8, &leaf.ecx, 4);
      std::memcpy(brand + i * 16 + 12, &leaf.edx, 4);
    }
    info.brand = trim(std::string(brand));
  }

  if (max_leaf >= 7u) {
    const CpuidResult leaf7 = cpuid_leaf(7, 0);
    info.avx2 = (leaf7.ebx & (1u << 5)) != 0;
    info.avx512 = (leaf7.ebx & (1u << 16)) != 0;
  }

#ifdef _WIN32
  SYSTEM_INFO system_info{};
  ::GetSystemInfo(&system_info);
  info.logical_cores = system_info.dwNumberOfProcessors;

  DWORD length = 0;
  ::GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
  if (length > 0) {
    std::vector<unsigned char> buffer(length);
    if (::GetLogicalProcessorInformationEx(
            RelationProcessorCore, reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(
                                       buffer.data()),
            &length) != 0) {
      std::uint32_t physical = 0;
      std::size_t offset = 0;
      while (offset < length) {
        const auto* entry = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(
            buffer.data() + offset);
        if (entry->Relationship == RelationProcessorCore) ++physical;
        offset += entry->Size;
        if (entry->Size == 0) break;
      }
      info.physical_cores = physical;
    }
  }

  MEMORYSTATUSEX memory{};
  memory.dwLength = sizeof(memory);
  if (::GlobalMemoryStatusEx(&memory) != 0) {
    info.total_memory_bytes = memory.ullTotalPhys;
  }
#else
  info.logical_cores = static_cast<std::uint32_t>(::sysconf(_SC_NPROCESSORS_ONLN));
  info.physical_cores = info.logical_cores;
  struct sysinfo system_info {};
  if (::sysinfo(&system_info) == 0) {
    info.total_memory_bytes = static_cast<std::uint64_t>(system_info.totalram) * system_info.mem_unit;
  }
#endif
  if (info.physical_cores == 0) info.physical_cores = info.logical_cores;
  return info;
}

CudaRuntimeInfo discover_cuda() {
  CudaRuntimeInfo info;
  CudaLibrary library = load_cuda();
  if (!library.loaded()) {
    info.provenance = Provenance::Unsupported;
    info.detail = "no CUDA driver library present on this host";
    return info;
  }
  if (library.init(0) != 0) {
    info.provenance = Provenance::Unsupported;
    info.detail = "CUDA driver library present but cuInit failed (no usable device)";
    return info;
  }
  info.driver_present = true;
  info.runtime_library = "nvcuda";
  if (library.driver_version != nullptr) {
    int version = 0;
    if (library.driver_version(&version) == 0) {
      info.driver_version = static_cast<std::uint32_t>(version);
      info.driver_version_string =
          std::to_string(version / 1000) + "." + std::to_string((version % 1000) / 10);
    }
  }
  int count = 0;
  if (library.device_count(&count) != 0 || count <= 0) {
    info.provenance = Provenance::Unsupported;
    info.detail = "CUDA driver initialized but reported no devices";
    return info;
  }
  const int bounded = std::min(count, 16);
  for (int index = 0; index < bounded; ++index) {
    CuDevice device = 0;
    if (library.device_get(&device, index) != 0) continue;
    GpuDevice gpu;
    gpu.index = static_cast<std::uint32_t>(index);
    char name[128] = {};
    if (library.device_name(name, static_cast<int>(sizeof(name)), device) == 0) {
      gpu.name = trim(std::string(name));
    }
    int major = 0;
    int minor = 0;
    if (library.device_attribute(&major, kAttrComputeCapabilityMajor, device) == 0 &&
        library.device_attribute(&minor, kAttrComputeCapabilityMinor, device) == 0) {
      gpu.compute_major = static_cast<std::uint32_t>(major);
      gpu.compute_minor = static_cast<std::uint32_t>(minor);
      gpu.architecture_token = cuda_architecture_token(gpu.compute_major, gpu.compute_minor);
    }
    static_cast<void>(kAttrName);
    if (library.device_total_mem != nullptr) {
      std::size_t bytes = 0;
      if (library.device_total_mem(&bytes, device) == 0) {
        gpu.total_memory_bytes = static_cast<std::uint64_t>(bytes);
      }
    }
    info.devices.push_back(std::move(gpu));
  }
  if (info.devices.empty()) {
    info.provenance = Provenance::Unsupported;
    info.detail = "CUDA driver initialized but no device attributes could be read";
    return info;
  }
  info.provenance = Provenance::Real;
  info.detail = "device attributes read through the CUDA driver API";
  return info;
}

std::vector<InferenceRuntimeInfo> discover_inference_runtimes() {
  std::vector<InferenceRuntimeInfo> runtimes;
  struct Candidate {
    const char* name;
    const char* windows_path;
    const char* other_path;
  };
  const Candidate candidates[] = {
      {"onnxruntime", "onnxruntime.dll", "libonnxruntime.so"},
      {"tensorrt", "nvinfer.dll", "libnvinfer.so"},
      {"vllm", nullptr, nullptr},
      {"llama.cpp", "llama.dll", "libllama.so"},
  };
  for (const Candidate& candidate : candidates) {
    InferenceRuntimeInfo info;
    info.name = candidate.name;
#ifdef _WIN32
    const char* path = candidate.windows_path;
#else
    const char* path = candidate.other_path;
#endif
    if (path == nullptr) {
      info.present = false;
      info.detail = "no local import path is probed for this runtime";
      runtimes.push_back(std::move(info));
      continue;
    }
    void* handle = load_library(path);
    info.present = handle != nullptr;
    info.path = path;
    info.detail = info.present ? "loadable on this host" : "not present on this host";
    runtimes.push_back(std::move(info));
  }
  return runtimes;
}

}  // namespace

HostEnvironment discover_host_environment() {
  HostEnvironment environment;
  environment.cpu = discover_cpu();
  environment.host = host_name();
  environment.process_id = current_process_id();
  environment.cuda = discover_cuda();
  environment.inference_runtimes = discover_inference_runtimes();
  environment.compiler =
#if defined(_MSC_VER)
      "msvc-" + std::to_string(_MSC_VER);
#elif defined(__clang__)
      std::string("clang-") + __clang_version__;
#elif defined(__GNUC__)
      std::string("gcc-") + __VERSION__;
#else
      "unknown";
#endif
#ifdef NDEBUG
  environment.build_type = "Release";
#else
  environment.build_type = "Debug";
#endif

#ifdef _WIN32
  environment.os_name = "Windows";
  {
    // GetVersionEx reports a compatibility-shimmed version unless the binary
    // carries a manifest, so the unshimmed RtlGetVersion is used instead.
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    RTL_OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    bool reported = false;
    if (HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll"); ntdll != nullptr) {
      const auto rtl_get_version = reinterpret_cast<RtlGetVersionFn>(
          ::GetProcAddress(ntdll, "RtlGetVersion"));
      if (rtl_get_version != nullptr && rtl_get_version(&version) == 0) {
        environment.os_version = std::to_string(version.dwMajorVersion) + "." +
                                 std::to_string(version.dwMinorVersion) + " (build " +
                                 std::to_string(version.dwBuildNumber) + ")";
        reported = true;
      }
    }
    if (!reported) environment.os_version = "unknown";
  }
  SYSTEM_INFO system_info{};
  ::GetNativeSystemInfo(&system_info);
  switch (system_info.wProcessorArchitecture) {
    case PROCESSOR_ARCHITECTURE_AMD64: environment.machine = "x86_64"; break;
    case PROCESSOR_ARCHITECTURE_ARM64: environment.machine = "aarch64"; break;
    case PROCESSOR_ARCHITECTURE_INTEL: environment.machine = "x86"; break;
    default: environment.machine = "unknown"; break;
  }
#else
  environment.os_name = "POSIX";
  environment.machine =
#if defined(__x86_64__)
      "x86_64";
#elif defined(__aarch64__)
      "aarch64";
#else
      "unknown";
#endif
#endif
  if (environment.os_version.empty()) environment.os_version = "unknown";
  return environment;
}

EnvironmentProfile to_environment_profile(const HostEnvironment& environment, std::string key) {
  EnvironmentProfile profile;
  profile.key = std::move(key);
  profile.architecture = environment.machine;
  profile.precisions.insert("fp32");
  if (environment.cpu.avx2) profile.precisions.insert("bf16");
  if (environment.cpu.avx512) profile.precisions.insert("fp16");

  for (const GpuDevice& device : environment.cuda.devices) {
    if (device.compute_major == 0 && device.compute_minor == 0) continue;
    // The first observed device defines the environment capability. Reporting a
    // heterogeneous fleet as one profile would be a fabrication.
    profile.architecture = device.architecture_token;
    profile.compute_capability = device.compute_major * 100u + device.compute_minor;
    profile.precisions.insert("fp16");
    profile.precisions.insert("bf16");
    profile.precisions.insert("fp8");
    profile.precisions.insert("int8");
    if (device.compute_major >= 8) profile.precisions.insert("int4");
    break;
  }
  if (environment.cuda.driver_present) {
    profile.backend = "cuda";
    profile.backend_version = environment.cuda.driver_version_string;
    profile.runtime = "cuda-driver";
    profile.runtime_version = environment.cuda.driver_version_string;
  } else {
    profile.backend = "cpu";
    profile.runtime = "cpu";
  }
  profile.provenance = environment.cuda.driver_present ? Provenance::Real : Provenance::Synthetic;
  return profile;
}

std::string render_host_environment(const HostEnvironment& environment) {
  std::ostringstream out;
  out << "host              = " << environment.host << "\n";
  out << "process_id        = " << environment.process_id << "\n";
  out << "os                = " << environment.os_name << " " << environment.os_version << "\n";
  out << "machine           = " << environment.machine << "\n";
  out << "compiler          = " << environment.compiler << " (" << environment.build_type << ")\n";
  out << "cpu_vendor        = " << environment.cpu.vendor << "\n";
  out << "cpu_brand         = " << environment.cpu.brand << "\n";
  out << "cpu_cores         = " << environment.cpu.physical_cores << " physical, "
      << environment.cpu.logical_cores << " logical\n";
  out << "cpu_avx2          = " << (environment.cpu.avx2 ? "yes" : "no") << "\n";
  out << "cpu_avx512        = " << (environment.cpu.avx512 ? "yes" : "no") << "\n";
  out << "memory_bytes      = " << environment.cpu.total_memory_bytes << "\n";
  out << "cuda_provenance   = " << to_string(environment.cuda.provenance) << "\n";
  out << "cuda_driver       = " << (environment.cuda.driver_present ? "present" : "absent")
      << " version=" << environment.cuda.driver_version_string << "\n";
  out << "cuda_detail       = " << environment.cuda.detail << "\n";
  for (const GpuDevice& device : environment.cuda.devices) {
    out << "gpu[" << device.index << "]            = " << device.name << " "
        << device.architecture_token << " (" << device.compute_major << "."
        << device.compute_minor << ") vram=" << device.total_memory_bytes << "\n";
  }
  for (const InferenceRuntimeInfo& runtime : environment.inference_runtimes) {
    out << "runtime[" << runtime.name << "]"
        << std::string(runtime.name.size() < 8 ? 8 - runtime.name.size() : 0, ' ')
        << "= " << (runtime.present ? "present" : "absent") << " (" << runtime.detail << ")\n";
  }
  return out.str();
}

bool has_compute_capability_at_least(const HostEnvironment& environment, std::uint32_t major,
                                     std::uint32_t minor) {
  for (const GpuDevice& device : environment.cuda.devices) {
    if (device.compute_major > major) return true;
    if (device.compute_major == major && device.compute_minor >= minor) return true;
  }
  return false;
}

}  // namespace mlf
