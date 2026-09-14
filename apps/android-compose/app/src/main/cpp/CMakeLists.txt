# =============================================================================
#  Android native build — produces libscribatic_engine.so.
#
#  Responsibilities:
#    1. Compile the shared core (core/engine) as a STATIC library.
#    2. Statically link it into one thin JNI shared object, so exactly one .so
#       is packaged and the dynamic loader does no cross-library relocation at
#       startup.
#    3. Apply the aggressive ARM64/NEON optimisation profile the ggml kernels
#       need to stay realtime.
# =============================================================================
cmake_minimum_required(VERSION 3.22.1)
project(scribatic_engine LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Resolve the monorepo root. Six levels up:
#   cpp -> main -> src -> app -> android-compose -> apps -> <root>
# Verified rather than assumed: a wrong depth here fails at configure time with
# a confusing "add_subdirectory: source directory does not exist" message.
get_filename_component(SCRIBATIC_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../.." ABSOLUTE)

if (NOT EXISTS "${SCRIBATIC_ROOT}/core/engine/CMakeLists.txt")
    message(FATAL_ERROR
        "scribatic: monorepo root resolved to '${SCRIBATIC_ROOT}', which does not "
        "contain core/engine. Check the relative depth above.")
endif()

# --- Shared modules -----------------------------------------------------------
add_subdirectory(${SCRIBATIC_ROOT}/core/database ${CMAKE_BINARY_DIR}/scribatic_database)
add_subdirectory(${SCRIBATIC_ROOT}/core/engine   ${CMAKE_BINARY_DIR}/scribatic_core)

# --- JNI translation unit -----------------------------------------------------
add_library(scribatic_engine SHARED native-lib.cpp)

target_link_libraries(scribatic_engine
    PRIVATE
        scribatic_core      # static; fully absorbed into libscribatic_engine.so
        android
        log
)

# --- ARM64 / NEON optimisation profile ---------------------------------------
if (ANDROID_ABI STREQUAL "arm64-v8a")
    target_compile_options(scribatic_core PRIVATE
        -O3
        -march=armv8.2-a+fp16+dotprod   # FP16 arithmetic + SDOT/UDOT for Q8 GEMM
        -mtune=cortex-a78
        -ffast-math                     # ggml tolerates non-IEEE reassociation
        -funroll-loops
        -fomit-frame-pointer
        -DGGML_USE_ACCELERATE=OFF
        -DGGML_NEON=1
    )
    target_compile_options(scribatic_engine PRIVATE -O3 -march=armv8.2-a+fp16+dotprod)
else()
    message(FATAL_ERROR
        "scribatic: only arm64-v8a is supported. Received ABI '${ANDROID_ABI}'.")
endif()

# Link-time optimisation across the static core and the JNI shim.
#
# Set the flags directly rather than via INTERPROCEDURAL_OPTIMIZATION: CMake's
# IPO support for Android also injects -fuse-ld=gold, and NDK r27 removed the
# gold linker, so the property fails the link with "invalid linker name". The
# NDK default linker (lld) handles thin LTO natively.
target_compile_options(scribatic_engine PRIVATE -flto=thin)

target_link_options(scribatic_engine PRIVATE
    -flto=thin
    -Wl,--gc-sections        # drop unreferenced ggml kernels
    -Wl,--exclude-libs,ALL   # do not re-export static-lib symbols from the .so
    -Wl,-z,max-page-size=16384  # 16 KB page alignment required by Android 15+
)

target_compile_options(scribatic_engine PRIVATE -fdata-sections -ffunction-sections)
