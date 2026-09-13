#include "mlf/artifact.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

namespace mlf {
namespace {

// ---------------------------------------------------------------------------
// SHA-256 (FIPS 180-4). Self-contained: the runtime takes no third-party
// dependency, and artifact identity must be a real content digest rather than a
// weak checksum.
// ---------------------------------------------------------------------------
constexpr std::uint32_t kSha256K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

constexpr std::uint32_t rotr(std::uint32_t value, std::uint32_t amount) noexcept {
  return (value >> amount) | (value << ((32u - amount) & 31u));
}

class Sha256 {
 public:
  Sha256() noexcept
      : state_{0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au, 0x510e527fu, 0x9b05688cu,
               0x1f83d9abu, 0x5be0cd19u} {}

  void update(const std::uint8_t* data, std::size_t length) noexcept {
    total_bytes_ += static_cast<std::uint64_t>(length);
    for (std::size_t i = 0; i < length; ++i) {
      buffer_[buffer_length_++] = data[i];
      if (buffer_length_ == 64) {
        compress(buffer_);
        buffer_length_ = 0;
      }
    }
  }

  [[nodiscard]] std::array<std::uint8_t, 32> finish() noexcept {
    const std::uint64_t bit_length = total_bytes_ * 8u;
    const std::uint8_t pad = 0x80u;
    update(&pad, 1);
    const std::uint8_t zero = 0x00u;
    while (buffer_length_ != 56) update(&zero, 1);
    std::uint8_t length_bytes[8];
    for (int i = 7; i >= 0; --i) {
      length_bytes[7 - i] = static_cast<std::uint8_t>((bit_length >> (8u * static_cast<unsigned>(i))) & 0xffu);
    }
    update(length_bytes, 8);

    std::array<std::uint8_t, 32> out{};
    for (std::size_t i = 0; i < 8; ++i) {
      out[i * 4 + 0] = static_cast<std::uint8_t>((state_[i] >> 24) & 0xffu);
      out[i * 4 + 1] = static_cast<std::uint8_t>((state_[i] >> 16) & 0xffu);
      out[i * 4 + 2] = static_cast<std::uint8_t>((state_[i] >> 8) & 0xffu);
      out[i * 4 + 3] = static_cast<std::uint8_t>(state_[i] & 0xffu);
    }
    return out;
  }

 private:
  void compress(const std::uint8_t* block) noexcept {
    std::uint32_t w[64];
    for (std::size_t i = 0; i < 16; ++i) {
      w[i] = (static_cast<std::uint32_t>(block[i * 4 + 0]) << 24) |
             (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
             (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
             (static_cast<std::uint32_t>(block[i * 4 + 3]));
    }
    for (std::size_t i = 16; i < 64; ++i) {
      const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint32_t a = state_[0];
    std::uint32_t b = state_[1];
    std::uint32_t c = state_[2];
    std::uint32_t d = state_[3];
    std::uint32_t e = state_[4];
    std::uint32_t f = state_[5];
    std::uint32_t g = state_[6];
    std::uint32_t h = state_[7];
    for (std::size_t i = 0; i < 64; ++i) {
      const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
      const std::uint32_t ch = (e & f) ^ ((~e) & g);
      const std::uint32_t temp1 = h + s1 + ch + kSha256K[i] + w[i];
      const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
      const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      const std::uint32_t temp2 = s0 + maj;
      h = g;
      g = f;
      f = e;
      e = d + temp1;
      d = c;
      c = b;
      b = a;
      a = temp1 + temp2;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
  }

  std::uint32_t state_[8];
  std::uint64_t total_bytes_{0};
  std::uint8_t buffer_[64]{};
  std::size_t buffer_length_{0};
};

std::string hex_of(const std::array<std::uint8_t, 32>& digest) {
  static const char* kHex = "0123456789abcdef";
  std::string out;
  out.resize(64);
  for (std::size_t i = 0; i < 32; ++i) {
    out[i * 2] = kHex[digest[i] >> 4];
    out[i * 2 + 1] = kHex[digest[i] & 0x0fu];
  }
  return out;
}

}  // namespace

bool compute_file_digest(std::string_view path, std::string& out_digest) {
  std::ifstream stream(std::string(path), std::ios::binary);
  if (!stream) return false;
  Sha256 hasher;
  std::vector<char> buffer(64 * 1024);
  while (stream) {
    stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize got = stream.gcount();
    if (got > 0) {
      hasher.update(reinterpret_cast<const std::uint8_t*>(buffer.data()),
                    static_cast<std::size_t>(got));
    }
  }
  if (stream.bad()) return false;
  out_digest = "sha256:" + hex_of(hasher.finish());
  return true;
}

std::string compute_buffer_digest(const void* data, std::size_t size) {
  Sha256 hasher;
  if (data != nullptr && size > 0) {
    hasher.update(static_cast<const std::uint8_t*>(data), size);
  }
  return "sha256:" + hex_of(hasher.finish());
}

bool valid_digest(std::string_view digest, std::size_t max_length) noexcept {
  if (digest.empty() || digest.size() > max_length) return false;
  if (digest.rfind("sha256:", 0) == 0) {
    if (digest.size() != 7 + 64) return false;
    for (std::size_t i = 7; i < digest.size(); ++i) {
      const char c = digest[i];
      const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
      if (!hex) return false;
    }
    return true;
  }
  for (char c : digest) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                    c == ':' || c == '_' || c == '-' || c == '.';
    if (!ok) return false;
  }
  return true;
}

}  // namespace mlf
