# ============================================================================
# NUMA Fabric - host-locality governance runtime
# ============================================================================
cmake_minimum_required(VERSION 3.25)

project(NUMAFabric
    VERSION 1.0.0
    DESCRIPTION "Explicit host-locality governance runtime for accelerator infrastructure"
    HOMEPAGE_URL "https://github.com/pngen/NUMA-Fabric"
    LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(NUMAFABRIC_ENABLE_CUDA "Build the CUDA integration/proof backend against the installed NVIDIA toolkit" OFF)
option(NUMAFABRIC_BUILD_TESTS "Build NUMA Fabric tests" ON)
option(NUMAFABRIC_BUILD_EXAMPLES "Build NUMA Fabric runnable examples" ON)
option(NUMAFABRIC_BUILD_BENCHMARKS "Build NUMA Fabric benchmarks" ON)
option(NUMAFABRIC_BUILD_CLI "Build the nf command-line tool" ON)
option(NUMAFABRIC_BUILD_DISTRIBUTED "Build the coordinator/worker/proof executables" ON)
option(NUMAFABRIC_WARNINGS_AS_ERRORS "Treat project warnings as errors" ON)

# ---------------------------------------------------------------------------
# C++ standard and runtime
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    # Static CRT for fully self-contained executables (proof processes, package consumers).
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# ---------------------------------------------------------------------------
# CUDA (optional). Always select the installed CUDA 13.x explicitly so the
# toolkit version is unambiguous when both 12.x and 13.x are present.
# ---------------------------------------------------------------------------
if(NUMAFABRIC_ENABLE_CUDA)
    find_program(NUMAFABRIC_CUDA_NVCC NAMES nvcc
        PATHS "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/"
        PATH_SUFFIXES v13.1/bin v13.0/bin v13.0.1/bin
        NO_DEFAULT_PATH)
    if(NOT NUMAFABRIC_CUDA_NVCC)
        find_program(NUMAFABRIC_CUDA_NVCC NAMES nvcc)
    endif()
    if(NUMAFABRIC_CUDA_NVCC)
        set(CMAKE_CUDA_COMPILER "${NUMAFABRIC_CUDA_NVCC}")
        enable_language(CUDA)
        find_package(CUDAToolkit REQUIRED)
        set(CMAKE_CUDA_STANDARD 20)
        # RTX 5090 is sm_120 (Blackwell). Request both compute and real-arch code.
        set(CMAKE_CUDA_ARCHITECTURES 120)
        message(STATUS "NUMA Fabric: CUDA enabled (nvcc=${CMAKE_CUDA_COMPILER}, sm_120)")
    else()
        message(WARNING "NUMA Fabric: CUDA requested but no nvcc found; building without CUDA.")
        set(NUMAFABRIC_ENABLE_CUDA OFF)
    endif()
endif()

# ---------------------------------------------------------------------------
# Compiler warnings. Project-controlled source must be clean under /W4 /WX.
# ---------------------------------------------------------------------------
add_library(numafabric_warnings INTERFACE)
if(MSVC)
    target_compile_options(numafabric_warnings INTERFACE /W4 /WX /permissive- /Zc:externC)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(numafabric_warnings INTERFACE -Wall -Wextra -Wpedantic -Werror)
endif()

# ---------------------------------------------------------------------------
# Core library
# ---------------------------------------------------------------------------
add_library(numafabric STATIC
    src/windows_backend.cpp
    src/synthetic_backend.cpp
    src/topology_impl.cpp
    src/memory_manager.cpp
    src/placement_engine.cpp
    src/reservation_store.cpp
    src/worker_registry.cpp
    src/runtime.cpp
    src/protocol_frame.cpp
    src/persistence_store.cpp
    src/coordinator.cpp
    src/worker.cpp
    src/runtime_core.cpp
)
add_library(NUMAFabric::numafabric ALIAS numafabric)

target_include_directories(numafabric
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)
target_link_libraries(numafabric PRIVATE numafabric_warnings)
target_compile_definitions(numafabric PUBLIC NUMAFABRIC_VERSION_MAJOR=1 NUMAFABRIC_VERSION_MINOR=0 NUMAFABRIC_VERSION_PATCH=0)

if(WIN32)
    target_link_libraries(numafabric PRIVATE ws2_32)
    target_compile_definitions(numafabric PRIVATE _WIN32_WINNT=0x0A00 WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

# ---------------------------------------------------------------------------
# CUDA integration (optional, separate target so the core stays CUDA-free)
# ---------------------------------------------------------------------------
if(NUMAFABRIC_ENABLE_CUDA)
    add_library(numafabric_cuda STATIC
        src/cuda_backend.cu)
    add_library(NUMAFabric::numafabric_cuda ALIAS numafabric_cuda)
    target_include_directories(numafabric_cuda PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)
    target_link_libraries(numafabric_cuda PUBLIC numafabric CUDA::cudart)
    # Project CUDA source must be warning-clean on the HOST compiler; nvcc
    # device code stays clean by construction. Suppress only the unavoidable
    # host-stub warning narrowly at this target boundary, never globally.
    set_target_properties(numafabric_cuda PROPERTIES
        CUDA_STANDARD 17
        CUDA_ARCHITECTURES 120
        CUDA_FLAGS "-Xcompiler=/W4 -Xcompiler=/WX -Xcompiler=/wd4503")
endif()

# ---------------------------------------------------------------------------
# Registration / helpers
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# Command-line tool
if(NUMAFABRIC_BUILD_CLI)
    add_executable(nf apps/nf/main.cpp)
    target_link_libraries(nf PRIVATE numafabric numafabric_warnings)
endif()

# Distributed executables
if(NUMAFABRIC_BUILD_DISTRIBUTED)
    add_executable(nf-coordinator apps/coordinator/main.cpp)
    target_link_libraries(nf-coordinator PRIVATE numafabric numafabric_warnings)
    add_executable(nf-worker apps/worker/main.cpp)
    target_link_libraries(nf-worker PRIVATE numafabric numafabric_warnings)
    add_executable(nf-proof apps/proof/main.cpp)
    target_link_libraries(nf-proof PRIVATE numafabric numafabric_warnings)
endif()

# ---------------------------------------------------------------------------
# Tests, examples, benchmarks
# ---------------------------------------------------------------------------
if(NUMAFABRIC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
if(NUMAFABRIC_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
if(NUMAFABRIC_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

# ---------------------------------------------------------------------------
# Install + export package
# ---------------------------------------------------------------------------
set(NUMAFABRIC_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/NUMAFabric")

install(TARGETS numafabric numafabric_warnings
    EXPORT NUMAFabricTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(DIRECTORY include/numafabric DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

if(NUMAFABRIC_ENABLE_CUDA)
    install(TARGETS numafabric_cuda
        EXPORT NUMAFabricTargets
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()

install(EXPORT NUMAFabricTargets
    FILE NUMAFabricTargets.cmake
    NAMESPACE NUMAFabric::
    DESTINATION "${NUMAFABRIC_INSTALL_CMAKEDIR}")

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/NUMAFabricConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/NUMAFabricConfig.cmake"
    INSTALL_DESTINATION "${NUMAFABRIC_INSTALL_CMAKEDIR}")
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/NUMAFabricConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY SameMajorVersion)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/NUMAFabricConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/NUMAFabricConfigVersion.cmake"
    DESTINATION "${NUMAFABRIC_INSTALL_CMAKEDIR}")
