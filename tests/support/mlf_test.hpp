// Model Lifecycle Fabric — minimal deterministic test harness.
//
// No external dependency, no timing, no timeouts. A test either returns or it
// throws; a hang is a defect and is never masked by a timer.
#pragma once

#include <concepts>
#include <cstdint>
#include <exception>
#include <functional>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "mlf/status.hpp"

namespace mlftest {

struct TestCase {
  std::string suite;
  std::string name;
  std::function<void()> body;
};

[[nodiscard]] std::vector<TestCase>& registry();

struct Registrar {
  Registrar(const char* suite, const char* name, std::function<void()> body);
};

/// Thrown by a failed expectation. The harness records it and moves on.
class Failure : public std::exception {
 public:
  explicit Failure(std::string message) : message_(std::move(message)) {}
  [[nodiscard]] const char* what() const noexcept override { return message_.c_str(); }

 private:
  std::string message_;
};

[[noreturn]] void fail(const char* file, int line, const std::string& message);

/// Run every registered test. Returns the number of failures.
int run_all(int argc, char** argv);

/// Deterministic pseudo-random generator (xorshift64*). Seeded explicitly by
/// every randomized test so failures reproduce exactly.
class Rng {
 public:
  explicit Rng(std::uint64_t seed)
      : state_(seed == 0 ? 0x9E3779B97F4A7C15ull : seed), seed_(seed) {}

  std::uint64_t next() {
    state_ ^= state_ >> 12;
    state_ ^= state_ << 25;
    state_ ^= state_ >> 27;
    return state_ * 0x2545F4914F6CDD1Dull;
  }
  /// Uniform value in [0, bound).
  std::uint64_t below(std::uint64_t bound) { return bound == 0 ? 0 : next() % bound; }
  [[nodiscard]] std::uint64_t seed() const noexcept { return seed_; }

 private:
  std::uint64_t state_{0};
  std::uint64_t seed_{0};
};

/// Generic rendering used by equality failure messages.
template <class T>
std::string describe(const T& value) {
  std::ostringstream out;
  out << value;
  return out.str();
}
/// Enumerations render through the library name table when one exists.
template <class T>
  requires std::is_enum_v<T>
std::string describe(const T& value) {
  if constexpr (requires(const T& candidate) {
                  { mlf::to_string(candidate) } -> std::convertible_to<std::string_view>;
                }) {
    return std::string(mlf::to_string(value));
  } else {
    return std::to_string(static_cast<long long>(value));
  }
}
inline std::string describe(const std::string& value) { return "\"" + value + "\""; }
inline std::string describe(std::string_view value) { return "\"" + std::string(value) + "\""; }
inline std::string describe(bool value) { return value ? "true" : "false"; }

/// Integer types for which the C++20 comparison functions are defined. The
/// character types and bool are integral but are excluded by the standard, so
/// they fall back to plain equality.
template <class T>
inline constexpr bool kComparableInteger =
    std::is_integral_v<T> && !std::is_same_v<T, bool> && !std::is_same_v<T, char> &&
    !std::is_same_v<T, wchar_t> && !std::is_same_v<T, char8_t> &&
    !std::is_same_v<T, char16_t> && !std::is_same_v<T, char32_t>;

/// Warning-free equality that is correct across signed and unsigned operands.
template <class A, class B>
constexpr bool eq(const A& a, const B& b) {
  if constexpr (kComparableInteger<A> && kComparableInteger<B>) {
    return std::cmp_equal(a, b);
  } else {
    return a == b;
  }
}

template <class A, class B>
std::string describe_inequality(const char* left_name, const char* right_name, const A& left,
                                const B& right) {
  return std::string(left_name) + " = " + describe(left) + " but " + right_name + " = " +
         describe(right);
}

}  // namespace mlftest

#define MLF_TEST(SUITE, NAME)                                                             \
  static void mlftest_body_##SUITE##_##NAME();                                            \
  static ::mlftest::Registrar mlftest_reg_##SUITE##_##NAME(#SUITE, #NAME,                 \
                                                           mlftest_body_##SUITE##_##NAME); \
  static void mlftest_body_##SUITE##_##NAME()

#define MLF_CHECK(EXPR)                                        \
  do {                                                         \
    if (!(EXPR)) {                                             \
      ::mlftest::fail(__FILE__, __LINE__, "expected: " #EXPR);  \
    }                                                          \
  } while (false)

#define MLF_CHECK_MSG(EXPR, MSG)                                          \
  do {                                                                    \
    if (!(EXPR)) {                                                        \
      ::mlftest::fail(__FILE__, __LINE__,                                  \
                      std::string("expected: " #EXPR " -- ") + (MSG));     \
    }                                                                     \
  } while (false)

#define MLF_CHECK_EQ(A, B)                                                              \
  do {                                                                                  \
    const auto& mlftest_a = (A);                                                        \
    const auto& mlftest_b = (B);                                                        \
    if (!::mlftest::eq(mlftest_a, mlftest_b)) {                                         \
      ::mlftest::fail(__FILE__, __LINE__,                                               \
                      ::mlftest::describe_inequality(#A, #B, mlftest_a, mlftest_b));     \
    }                                                                                   \
  } while (false)