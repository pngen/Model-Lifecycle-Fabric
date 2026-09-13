#include "mlf_test.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sstream>

namespace mlftest {
namespace {

int g_failures = 0;
std::string g_current;

}  // namespace

std::vector<TestCase>& registry() {
  static std::vector<TestCase> tests;
  return tests;
}

Registrar::Registrar(const char* suite, const char* name, std::function<void()> body) {
  registry().push_back(TestCase{suite, name, std::move(body)});
}

void fail(const char* file, int line, const std::string& message) {
  std::ostringstream out;
  out << g_current << ": " << message << "\n    at " << file << ":" << line;
  throw Failure(out.str());
}

int run_all(int argc, char** argv) {
  // Unbuffered output: a test that hangs must be identifiable from the last line
  // it printed, not from a buffer that never flushes.
  std::setvbuf(stdout, nullptr, _IONBF, 0);

  std::string filter;
  bool list_only = false;
  bool verbose = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--list") == 0) list_only = true;
    if (std::strcmp(argv[i], "--verbose") == 0) verbose = true;
    if (std::strcmp(argv[i], "--filter") == 0 && i + 1 < argc) filter = argv[++i];
  }

  std::vector<TestCase>& tests = registry();
  std::stable_sort(tests.begin(), tests.end(), [](const TestCase& a, const TestCase& b) {
    if (a.suite != b.suite) return a.suite < b.suite;
    return a.name < b.name;
  });

  if (list_only) {
    for (const TestCase& test : tests) {
      std::printf("%s.%s\n", test.suite.c_str(), test.name.c_str());
    }
    return 0;
  }

  int executed = 0;
  for (const TestCase& test : tests) {
    const std::string qualified = test.suite + "." + test.name;
    if (!filter.empty() && qualified.find(filter) == std::string::npos) continue;
    g_current = qualified;
    ++executed;
    if (verbose) std::printf("RUN  %s\n", qualified.c_str());
    try {
      test.body();
    } catch (const Failure& failure) {
      ++g_failures;
      std::printf("FAIL %s\n%s\n", qualified.c_str(), failure.what());
    } catch (const std::exception& error) {
      ++g_failures;
      std::printf("FAIL %s\n    unexpected exception: %s\n", qualified.c_str(), error.what());
    } catch (...) {
      ++g_failures;
      std::printf("FAIL %s\n    unexpected non-standard exception\n", qualified.c_str());
    }
  }

  std::printf("%s: %d test(s) executed, %d failure(s)\n",
              g_failures == 0 ? "PASS" : "FAIL", executed, g_failures);
  std::fflush(stdout);
  return g_failures;
}

}  // namespace mlftest

int main(int argc, char** argv) { return mlftest::run_all(argc, argv); }
