cmake_minimum_required(VERSION 3.20)

project(ModelLifecycleFabric
    VERSION 1.0.0
    DESCRIPTION "Vendor-neutral C++20 runtime for governing model promotion, rollout, canary, rollback, retirement, compatibility, residency transition, and fleet-wide version authority."
    HOMEPAGE_URL "https://github.com/pngen/Model-Lifecycle-Fabric"
    LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(MLF_BUILD_TESTS        "Build the Model Lifecycle Fabric test suite"       ON)
option(MLF_BUILD_EXAMPLES     "Build the examples"                                ON)
option(MLF_BUILD_BENCHMARKS   "Build the benchmark driver"                        ON)
option(MLF_BUILD_CLI          "Build the mlf_cli inspection and mutation tool"    ON)
option(MLF_WARNINGS_AS_ERRORS "Treat first-party compiler warnings as errors"     ON)
option(MLF_ENABLE_SANITIZERS  "Build with AddressSanitizer where supported"       OFF)

# AddressSanitizer for MSVC x64 needs a runtime that is a separate Visual Studio
# component. Detecting it here turns a confusing link error into an actionable
# message, and keeps the "unsupported in this environment" claim honest.
if(MLF_ENABLE_SANITIZERS AND MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    find_library(MLF_ASAN_RUNTIME
        NAMES clang_rt.asan_dynamic-x86_64
        PATHS ${CMAKE_CXX_IMPLICIT_LINK_DIRECTORIES}
        NO_DEFAULT_PATH)
    if(NOT MLF_ASAN_RUNTIME)
        message(FATAL_ERROR
            "MLF_ENABLE_SANITIZERS=ON but the x64 AddressSanitizer runtime "
            "(clang_rt.asan_dynamic-x86_64.lib) is not installed. Install the "
            "'C++ AddressSanitizer' component of the Visual Studio C++ workload, or "
            "configure without MLF_ENABLE_SANITIZERS. AddressSanitizer is reported as "
            "UNSUPPORTED in this environment; the Debug configuration (iterator "
            "debugging, debug heap, /RTC1 runtime checks) is the available equivalent.")
    endif()
    message(STATUS "AddressSanitizer runtime: ${MLF_ASAN_RUNTIME}")
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# ---------------------------------------------------------------------------
# First-party warning policy: strict, and zero warnings tolerated.
#
# The policy is applied directly to each first-party target rather than through
# an INTERFACE target, so that nothing internal leaks into the exported package.
# ---------------------------------------------------------------------------
function(mlf_strict_compile_options target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4 /permissive- /Zc:__cplusplus /Zc:preprocessor /utf-8 /EHsc /MP /bigobj)
        if(MLF_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
        target_compile_definitions(${target} PRIVATE
            _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
        if(MLF_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
    if(MLF_ENABLE_SANITIZERS)
        if(MSVC)
            # AddressSanitizer needs debug information to produce useful reports;
            # without it the compiler emits C5072 for every translation unit.
            target_compile_options(${target} PRIVATE /fsanitize=address /Zi /Od)
            target_link_options(${target} PRIVATE /DEBUG /INCREMENTAL:NO)
        else()
            target_compile_options(${target} PRIVATE
                -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
        endif()
        target_compile_definitions(${target} PRIVATE MLF_SANITIZERS_ENABLED=1)
    endif()
endfunction()

function(mlf_strict_link_options target)
    if(MLF_ENABLE_SANITIZERS AND NOT MSVC)
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
endfunction()

# ---------------------------------------------------------------------------
# The library
# ---------------------------------------------------------------------------
set(MLF_PUBLIC_HEADERS
    include/mlf/artifact.hpp
    include/mlf/authority.hpp
    include/mlf/client.hpp
    include/mlf/compatibility.hpp
    include/mlf/coordinator.hpp
    include/mlf/engine.hpp
    include/mlf/evidence.hpp
    include/mlf/host_env.hpp
    include/mlf/identity.hpp
    include/mlf/lifecycle.hpp
    include/mlf/model_record.hpp
    include/mlf/policy.hpp
    include/mlf/process.hpp
    include/mlf/protocol.hpp
    include/mlf/provenance.hpp
    include/mlf/rollout.hpp
    include/mlf/scope.hpp
    include/mlf/state_store.hpp
    include/mlf/status.hpp
    include/mlf/synthetic_backend.hpp
    include/mlf/version.hpp
    include/mlf/worker.hpp)

set(MLF_SOURCES
    src/artifact.cpp
    src/authority.cpp
    src/client.cpp
    src/compatibility.cpp
    src/coordinator.cpp
    src/engine_authority.cpp
    src/engine_promotion.cpp
    src/engine_reconcile.cpp
    src/engine_registry.cpp
    src/engine_rollback.cpp
    src/engine_rollout.cpp
    src/engine_snapshot.cpp
    src/evidence.cpp
    src/host_env.cpp
    src/lifecycle.cpp
    src/policy.cpp
    src/process.cpp
    src/protocol.cpp
    src/rollout.cpp
    src/scope.cpp
    src/state_store.cpp
    src/status.cpp
    src/synthetic_backend.cpp
    src/worker.cpp)

add_library(mlf ${MLF_SOURCES} ${MLF_PUBLIC_HEADERS})
add_library(SummonSoftwareLabs::ModelLifecycleFabric ALIAS mlf)

target_include_directories(mlf
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

mlf_strict_compile_options(mlf)
if(WIN32)
    # Winsock is part of the public control-plane API, so it is a public link
    # requirement of the exported target on Windows.
    target_link_libraries(mlf PUBLIC ws2_32)
endif()
target_compile_definitions(mlf
    PUBLIC MLF_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    PUBLIC MLF_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    PUBLIC MLF_VERSION_PATCH=${PROJECT_VERSION_PATCH})

if(BUILD_SHARED_LIBS)
    target_compile_definitions(mlf PUBLIC MLF_SHARED=1)
    set_target_properties(mlf PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

set_target_properties(mlf PROPERTIES
    OUTPUT_NAME "ModelLifecycleFabric"
    # The installed imported target must carry the same name as the build-tree
    # alias, so that downstream code is identical in both configurations.
    EXPORT_NAME ModelLifecycleFabric
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    DEBUG_POSTFIX "d")

# ---------------------------------------------------------------------------
# Test support harness (first-party, no external dependency)
# ---------------------------------------------------------------------------
add_library(mlf_test_support STATIC
    tests/support/test_main.cpp
    tests/support/test_process_util.cpp)
target_include_directories(mlf_test_support PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/support
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(mlf_test_support PUBLIC mlf)
mlf_strict_compile_options(mlf_test_support)

function(mlf_add_test NAME)
    add_executable(${NAME} ${ARGN})
    target_link_libraries(${NAME} PRIVATE mlf mlf_test_support)
    mlf_strict_compile_options(${NAME})
    mlf_strict_link_options(${NAME})
    add_test(NAME ${NAME} COMMAND ${NAME})
    # No TIMEOUT test property is ever set: the suite must complete naturally and
    # a hang is treated as a defect, not masked by a timer.
    set_tests_properties(${NAME} PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
endfunction()

if(MLF_BUILD_TESTS)
    enable_testing()

    mlf_add_test(mlf_test_identity_scope         tests/unit/test_identity_scope.cpp)
    mlf_add_test(mlf_test_lifecycle_machine      tests/unit/test_lifecycle_machine.cpp)
    mlf_add_test(mlf_test_compatibility          tests/unit/test_compatibility.cpp)
    mlf_add_test(mlf_test_registry_bounds        tests/unit/test_registry_bounds.cpp)
    mlf_add_test(mlf_test_promotion_eligibility  tests/unit/test_promotion_eligibility.cpp)
    mlf_add_test(mlf_test_rollout_canary         tests/unit/test_rollout_canary.cpp)
    mlf_add_test(mlf_test_scope_authority        tests/unit/test_scope_authority.cpp)
    mlf_add_test(mlf_test_rollback_retirement    tests/unit/test_rollback_retirement.cpp)
    mlf_add_test(mlf_test_supersession           tests/unit/test_supersession.cpp)
    mlf_add_test(mlf_test_residency_readiness    tests/unit/test_residency_readiness.cpp)
    mlf_add_test(mlf_test_reconciliation         tests/unit/test_reconciliation.cpp)

    mlf_add_test(mlf_test_stale_generation       tests/adversarial/test_stale_generation.cpp)
    mlf_add_test(mlf_test_protocol_adversarial   tests/adversarial/test_protocol_adversarial.cpp)
    mlf_add_test(mlf_test_persistence_adversarial tests/adversarial/test_persistence_adversarial.cpp)
    mlf_add_test(mlf_test_compatibility_adversarial tests/adversarial/test_compatibility_adversarial.cpp)
    mlf_add_test(mlf_test_rollout_adversarial    tests/adversarial/test_rollout_adversarial.cpp)
    mlf_add_test(mlf_test_backpressure_bounds    tests/adversarial/test_backpressure_bounds.cpp)

    mlf_add_test(mlf_test_state_store            tests/persistence/test_state_store.cpp)
    mlf_add_test(mlf_test_recovery               tests/persistence/test_recovery.cpp)

    mlf_add_test(mlf_test_property_lifecycle     tests/property/test_property_lifecycle.cpp)
    mlf_add_test(mlf_test_race_interleavings     tests/property/test_race_interleavings.cpp)
    mlf_add_test(mlf_test_concurrency            tests/property/test_concurrency.cpp)

    mlf_add_test(mlf_test_host_validation        tests/hardware/test_host_validation.cpp)
    mlf_add_test(mlf_test_artifact_proof         tests/hardware/test_artifact_proof.cpp)

    mlf_add_test(mlf_test_multiprocess           tests/distributed/test_multiprocess_proofs.cpp)
    target_compile_definitions(mlf_test_multiprocess PRIVATE
        MLF_WORKER_BINARY="$<TARGET_FILE:mlf_worker>"
        MLF_COORDINATOR_BINARY="$<TARGET_FILE:mlf_coordinator>"
        MLF_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}")
    add_dependencies(mlf_test_multiprocess mlf_worker mlf_coordinator)
endif()

# ---------------------------------------------------------------------------
# CLI, worker, coordinator, examples, benchmarks
# ---------------------------------------------------------------------------
if(MLF_BUILD_CLI)
    add_executable(mlf_cli tools/mlf_cli/main.cpp)
    target_link_libraries(mlf_cli PRIVATE mlf)
    mlf_strict_compile_options(mlf_cli)
    mlf_strict_link_options(mlf_cli)
endif()

# Worker and coordinator hosts are production entry points, not test shims: the
# multiprocess proofs and the CLI both drive these binaries.
add_executable(mlf_worker tools/mlf_worker/main.cpp)
target_link_libraries(mlf_worker PRIVATE mlf)
mlf_strict_compile_options(mlf_worker)
mlf_strict_link_options(mlf_worker)

add_executable(mlf_coordinator tools/mlf_coordinator/main.cpp)
target_link_libraries(mlf_coordinator PRIVATE mlf)
mlf_strict_compile_options(mlf_coordinator)
mlf_strict_link_options(mlf_coordinator)

if(MLF_BUILD_EXAMPLES)
    foreach(example register_and_promote
                    canary_rollout
                    scope_authority
                    rollback
                    retirement
                    artifact_generation_change
                    worker_fencing
                    persistence_recovery
                    installed_consumer)
        add_executable(mlf_example_${example} examples/${example}.cpp)
        target_link_libraries(mlf_example_${example} PRIVATE mlf)
        mlf_strict_compile_options(mlf_example_${example})
        mlf_strict_link_options(mlf_example_${example})
    endforeach()
endif()

if(MLF_BUILD_BENCHMARKS)
    add_executable(mlf_benchmarks benchmarks/benchmarks.cpp)
    target_link_libraries(mlf_benchmarks PRIVATE mlf)
    mlf_strict_compile_options(mlf_benchmarks)
    mlf_strict_link_options(mlf_benchmarks)
endif()

# ---------------------------------------------------------------------------
# Install / export
# ---------------------------------------------------------------------------
install(TARGETS mlf
    EXPORT ModelLifecycleFabricTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/mlf DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

set(MLF_CMAKE_CONFIG_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ModelLifecycleFabric)

install(EXPORT ModelLifecycleFabricTargets
    FILE ModelLifecycleFabricTargets.cmake
    NAMESPACE SummonSoftwareLabs::
    DESTINATION ${MLF_CMAKE_CONFIG_DESTINATION})

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ModelLifecycleFabricConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/ModelLifecycleFabricConfig.cmake
    INSTALL_DESTINATION ${MLF_CMAKE_CONFIG_DESTINATION})

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/ModelLifecycleFabricConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/ModelLifecycleFabricConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/ModelLifecycleFabricConfigVersion.cmake
    DESTINATION ${MLF_CMAKE_CONFIG_DESTINATION})

install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/ModelLifecycleFabric)
install(FILES README.md DESTINATION ${CMAKE_INSTALL_DATADIR}/ModelLifecycleFabric)
