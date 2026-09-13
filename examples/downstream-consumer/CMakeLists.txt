cmake_minimum_required(VERSION 3.20)

# An independent downstream project. It is not part of the Model Lifecycle
# Fabric build tree: it consumes the installed package exactly as any other
# project would.
project(mlf_downstream_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(ModelLifecycleFabric CONFIG REQUIRED)

add_executable(mlf_downstream_consumer main.cpp)
target_link_libraries(mlf_downstream_consumer PRIVATE SummonSoftwareLabs::ModelLifecycleFabric)

if(MSVC)
    target_compile_options(mlf_downstream_consumer PRIVATE /W4 /WX /permissive- /Zc:__cplusplus)
endif()
