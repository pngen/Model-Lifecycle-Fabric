// Model Lifecycle Fabric — version constants.
#pragma once

#include <cstdint>
#include <string_view>

#ifndef MLF_VERSION_MAJOR
#define MLF_VERSION_MAJOR 1
#endif
#ifndef MLF_VERSION_MINOR
#define MLF_VERSION_MINOR 0
#endif
#ifndef MLF_VERSION_PATCH
#define MLF_VERSION_PATCH 0
#endif

namespace mlf {

inline constexpr std::uint32_t kVersionMajor = MLF_VERSION_MAJOR;
inline constexpr std::uint32_t kVersionMinor = MLF_VERSION_MINOR;
inline constexpr std::uint32_t kVersionPatch = MLF_VERSION_PATCH;

/// Version string of the runtime. Version strings are identity metadata, never
/// lifecycle authority.
inline constexpr std::string_view kVersionString = "1.0.0";

/// Wire protocol version. Independent of the library version: it changes only
/// when frame layout or message semantics change incompatibly.
inline constexpr std::uint16_t kProtocolVersion = 1;

/// Durable state format version.
inline constexpr std::uint16_t kStateFormatVersion = 1;

}  // namespace mlf
