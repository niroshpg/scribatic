# =============================================================================
#  scribatic_database — header-only schema + query definitions.
#  Deliberately INTERFACE: the DDL must be compiled into both apps without
#  either one owning a divergent copy.
# =============================================================================
cmake_minimum_required(VERSION 3.22.1)
project(scribatic_database LANGUAGES CXX)

add_library(scribatic_database INTERFACE)

target_include_directories(scribatic_database
    INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(scribatic_database INTERFACE cxx_std_17)
