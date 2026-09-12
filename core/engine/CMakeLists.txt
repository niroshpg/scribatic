# =============================================================================
#  scribatic_core — the shared, platform-agnostic engine.
#
#  Built identically for both targets; only the toolchain file differs.
#    Android : $ANDROID_NDK/build/cmake/android.toolchain.cmake (arm64-v8a)
#    iOS     : -G Xcode -DCMAKE_SYSTEM_NAME=iOS (arm64 device + simulator)
#  Contains zero platform conditionals beyond SIMD dispatch.
# =============================================================================
cmake_minimum_required(VERSION 3.22.1)
project(scribatic_core LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(Threads REQUIRED)

add_library(scribatic_core STATIC
    src/AudioRingBuffer.cpp
    src/ModelResidency.cpp
    src/EngineImpl.cpp
)

target_include_directories(scribatic_core
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Shared schema headers (SQLite-VSS DDL) live in a sibling module. The guard
# lets this CMakeLists configure standalone (host CI, `make build-core`) as well
# as from the Android app's CMakeLists, which adds the module itself.
if (NOT TARGET scribatic_database)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../database
                     ${CMAKE_BINARY_DIR}/scribatic_database)
endif()
target_link_libraries(scribatic_core PUBLIC scribatic_database)

target_compile_options(scribatic_core PRIVATE
    -Wall -Wextra -Wpedantic
    # Exceptions stay ON. llama.cpp throws internally during model load and
    # catches at its own boundary; a -fno-exceptions frame anywhere in that call
    # chain turns a recoverable load failure into std::terminate. RTTI likewise
    # stays on because ggml-backend uses dynamic_cast in its dispatch path.
    -fexceptions
    -frtti
    -fvisibility=hidden        # only the module-mapped symbols are exported
)

target_compile_definitions(scribatic_core PUBLIC
    $<$<CONFIG:Release>:SCRIBATIC_RELEASE=1>
    $<$<CONFIG:Debug>:SCRIBATIC_DEBUG=1>
)

# --- Backend submodules --------------------------------------------------
# Vendored by `make setup-all`; guarded so the tree configures before fetch.
if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/vendor/whisper.cpp/CMakeLists.txt)
    add_subdirectory(vendor/whisper.cpp EXCLUDE_FROM_ALL)
    target_link_libraries(scribatic_core PRIVATE whisper)
endif()

if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/vendor/llama.cpp/CMakeLists.txt)
    add_subdirectory(vendor/llama.cpp EXCLUDE_FROM_ALL)
    target_link_libraries(scribatic_core PRIVATE llama)
endif()

# --- Tests ----------------------------------------------------------------
# Host-only. Excluded from the Android/iOS builds by the cross-compile guard.
if (NOT CMAKE_CROSSCOMPILING)
    enable_testing()
    add_executable(scribatic_core_tests tests/RingBufferTests.cpp)
    target_link_libraries(scribatic_core_tests PRIVATE scribatic_core Threads::Threads)
    target_include_directories(scribatic_core_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME scribatic_core_tests COMMAND scribatic_core_tests)
endif()
