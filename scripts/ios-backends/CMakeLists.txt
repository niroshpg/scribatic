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

add_subdirectory(${SCRIBATIC_VENDOR}/llama.cpp   llama   EXCLUDE_FROM_ALL)
add_subdirectory(${SCRIBATIC_VENDOR}/whisper.cpp whisper EXCLUDE_FROM_ALL)

add_custom_target(backends ALL DEPENDS llama whisper)
