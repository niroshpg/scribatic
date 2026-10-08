# =============================================================================
#  The vendored backends as static libraries for the iOS app, which compiles
#  core/engine/src itself and links these (see `make setup-ios-backends`).
#
#  One project for both, for the reason core/engine/CMakeLists.txt gives:
#  llama.cpp and whisper.cpp each vendor ggml, and whichever is added first
#  defines it for the build. llama goes first, so there is one ggml — the
#  newer one — and the two libraries agree on it. Built apart, each would
#  bring its own libggml*.a and the app would link two copies.
# =============================================================================
cmake_minimum_required(VERSION 3.20)
project(scribatic_ios_backends C CXX)

set(SCRIBATIC_VENDOR "${CMAKE_CURRENT_LIST_DIR}/../../core/engine/vendor" CACHE PATH "")

set(LLAMA_BUILD_COMMON   OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_SERVER   OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TOOLS    OFF CACHE BOOL "" FORCE)
set(LLAMA_CURL           OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_SERVER   OFF CACHE BOOL "" FORCE)

# ggml sizes each CPU thread's slice of its work buffer with CACHE_LINE_SIZE
# as its C code sees it (64), but its C++ ops index the slices with
# std::hardware_destructive_interference_size where the standard library has
# it: 128 on Apple Silicon. The last thread then writes past the buffer, which
# corrupted the heap at random during summaries and answers on iOS (found
# under AddressSanitizer; Android's library says 64, so it never showed).
# Without these predefines libc++ offers no interference size, and ggml's C++
# falls back to 64 as its C does.
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-U__GCC_DESTRUCTIVE_SIZE>
                    $<$<COMPILE_LANGUAGE:CXX>:-U__GCC_CONSTRUCTIVE_SIZE>)

add_subdirectory(${SCRIBATIC_VENDOR}/llama.cpp   llama   EXCLUDE_FROM_ALL)
add_subdirectory(${SCRIBATIC_VENDOR}/whisper.cpp whisper EXCLUDE_FROM_ALL)

add_custom_target(backends ALL DEPENDS llama whisper)
