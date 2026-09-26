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

# --- SQLite ------------------------------------------------------------------
# The amalgamation, compiled into the core on every platform: the NDK exposes
# no SQLite, and iOS's system build varies by OS release. One build means one
# feature set and one file format on both. Fetched and checksum-pinned by
# scripts/fetch_native_deps.sh.
set(SCRIBATIC_SQLITE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendor/sqlite)
if (NOT EXISTS ${SCRIBATIC_SQLITE_DIR}/sqlite3.c)
    message(FATAL_ERROR
        "scribatic: SQLite amalgamation not found in ${SCRIBATIC_SQLITE_DIR}. "
        "Run: make fetch-deps")
endif()

add_library(scribatic_sqlite STATIC ${SCRIBATIC_SQLITE_DIR}/sqlite3.c)
set_target_properties(scribatic_sqlite PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(scribatic_sqlite PUBLIC ${SCRIBATIC_SQLITE_DIR})
target_compile_definitions(scribatic_sqlite PRIVATE
    SQLITE_ENABLE_FTS5            # the lexical half of hybrid retrieval
    SQLITE_DQS=0                  # "text" is an identifier, never a string
    SQLITE_DEFAULT_FOREIGN_KEYS=1 # cascades hold even if a PRAGMA is missed
    SQLITE_THREADSAFE=1
    SQLITE_OMIT_DEPRECATED
)
# -fno-fast-math matters: SQLite's float formatting and NaN handling assume
# IEEE semantics, and the engine is built with -ffast-math. -w because the
# amalgamation is not ours to warn about.
target_compile_options(scribatic_sqlite PRIVATE -O2 -fno-fast-math -w)
target_link_libraries(scribatic_sqlite PUBLIC ${CMAKE_DL_LIBS})
