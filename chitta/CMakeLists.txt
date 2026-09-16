cmake_minimum_required(VERSION 3.14)
project(chitta VERSION 0.1.0 LANGUAGES C CXX)

# Embedding identity — public default matches the bundled bge-large-en-v1.5 GGUF
# (1024-dim). Personal/local builds override via env CHITTA_EMBED_DIM (the same env var
# chitta-field's build.rs reads), so one environment drives both the Rust and C++ layers
# and the on-disk format never drifts between them. Cached so the value is seeded ONCE
# (from env, on the first configure of a build dir) and then sticks — a later plain
# `cmake --build` without the env cannot silently flip a 1536 personal build back to 768.
# To change an existing build dir, pass -DCHITTA_EMBED_DIM=<n> or use a fresh dir.
if(NOT DEFINED CHITTA_EMBED_DIM)
    if(DEFINED ENV{CHITTA_EMBED_DIM})
        set(CHITTA_EMBED_DIM "$ENV{CHITTA_EMBED_DIM}" CACHE STRING "Embedding dimension")
    else()
        set(CHITTA_EMBED_DIM "1024" CACHE STRING "Embedding dimension")
    endif()
endif()
math(EXPR _chitta_embed_mod "${CHITTA_EMBED_DIM} % 64")
if(NOT _chitta_embed_mod EQUAL 0)
    message(FATAL_ERROR "CHITTA_EMBED_DIM (${CHITTA_EMBED_DIM}) must be a multiple of 64")
endif()
add_compile_definitions(CHITTA_EMBED_DIM=${CHITTA_EMBED_DIM})
message(STATUS "chitta embed dim: ${CHITTA_EMBED_DIM}")

include(FetchContent)

# CRoaring - compressed bitmaps for tag indices (100M+ scale)
FetchContent_Declare(
    roaring
    GIT_REPOSITORY https://github.com/RoaringBitmap/CRoaring.git
    GIT_TAG v4.3.0
    GIT_SHALLOW TRUE
)
set(ROARING_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ENABLE_ROARING_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(roaring)

# Tree-sitter - incremental parsing for code intelligence
FetchContent_Declare(
    tree-sitter
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter.git
    GIT_TAG da6fe9beb4f7f67beb75914ca8e0d48ae48d6406 # v0.25.10, grammar ABI 15
)
FetchContent_GetProperties(tree-sitter)
if(NOT tree-sitter_POPULATED)
    FetchContent_Populate(tree-sitter)
    # Build tree-sitter as static library
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    add_subdirectory(${tree-sitter_SOURCE_DIR}/lib ${tree-sitter_BINARY_DIR})
endif()

# Tree-sitter C++ grammar
FetchContent_Declare(
    tree-sitter-cpp
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-cpp.git
    GIT_TAG v0.23.4
    GIT_SHALLOW TRUE
)
FetchContent_GetProperties(tree-sitter-cpp)
if(NOT tree-sitter-cpp_POPULATED)
    FetchContent_Populate(tree-sitter-cpp)
endif()

# Tree-sitter Python grammar
FetchContent_Declare(
    tree-sitter-python
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-python.git
    GIT_TAG v0.23.6
    GIT_SHALLOW TRUE
)
FetchContent_GetProperties(tree-sitter-python)
if(NOT tree-sitter-python_POPULATED)
    FetchContent_Populate(tree-sitter-python)
endif()

# Tree-sitter JavaScript grammar (includes TypeScript)
FetchContent_Declare(
    tree-sitter-javascript
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-javascript.git
    GIT_TAG v0.23.1
    GIT_SHALLOW TRUE
)
FetchContent_GetProperties(tree-sitter-javascript)
if(NOT tree-sitter-javascript_POPULATED)
    FetchContent_Populate(tree-sitter-javascript)
endif()

# Tree-sitter TypeScript grammar
FetchContent_Declare(
    tree-sitter-typescript
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-typescript.git
    GIT_TAG v0.23.2
    GIT_SHALLOW TRUE
)
FetchContent_GetProperties(tree-sitter-typescript)
if(NOT tree-sitter-typescript_POPULATED)
    FetchContent_Populate(tree-sitter-typescript)
endif()

# Tree-sitter Go grammar
FetchContent_Declare(
    tree-sitter-go
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-go.git
    GIT_TAG v0.23.4
    GIT_SHALLOW TRUE
)
FetchContent_GetProperties(tree-sitter-go)
if(NOT tree-sitter-go_POPULATED)
    FetchContent_Populate(tree-sitter-go)
endif()

# Tree-sitter Rust grammar
FetchContent_Declare(
    tree-sitter-rust
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-rust.git
    GIT_TAG v0.23.2
    GIT_SHALLOW TRUE
)
FetchContent_GetProperties(tree-sitter-rust)
if(NOT tree-sitter-rust_POPULATED)
    FetchContent_Populate(tree-sitter-rust)
endif()

# Tree-sitter Java grammar
FetchContent_Declare(
    tree-sitter-java
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-java.git
    GIT_TAG v0.23.5
    GIT_SHALLOW TRUE
)
FetchContent_GetProperties(tree-sitter-java)
if(NOT tree-sitter-java_POPULATED)
    FetchContent_Populate(tree-sitter-java)
endif()

# Tree-sitter Ruby grammar
FetchContent_Declare(
    tree-sitter-ruby
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-ruby.git
    GIT_TAG v0.23.1
    GIT_SHALLOW TRUE
)
FetchContent_GetProperties(tree-sitter-ruby)
if(NOT tree-sitter-ruby_POPULATED)
    FetchContent_Populate(tree-sitter-ruby)
endif()

# Tree-sitter C# grammar
FetchContent_Declare(
    tree-sitter-c-sharp
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-c-sharp.git
    GIT_TAG v0.23.1
    GIT_SHALLOW TRUE
)
FetchContent_GetProperties(tree-sitter-c-sharp)
if(NOT tree-sitter-c-sharp_POPULATED)
    FetchContent_Populate(tree-sitter-c-sharp)
endif()

# Tree-sitter Swift grammar (alex-pinkus maintained fork)
FetchContent_Declare(
    tree-sitter-swift
    GIT_REPOSITORY https://github.com/alex-pinkus/tree-sitter-swift.git
    GIT_TAG 0.7.1-with-generated-files
    GIT_SHALLOW TRUE
)
FetchContent_GetProperties(tree-sitter-swift)
if(NOT tree-sitter-swift_POPULATED)
    FetchContent_Populate(tree-sitter-swift)
endif()

FetchContent_Declare(
    tree-sitter-lua
    GIT_REPOSITORY https://github.com/tree-sitter-grammars/tree-sitter-lua.git
    GIT_TAG v0.2.0
    GIT_SHALLOW TRUE
)
FetchContent_GetProperties(tree-sitter-lua)
if(NOT tree-sitter-lua_POPULATED)
    FetchContent_Populate(tree-sitter-lua)
endif()

# Build tree-sitter parsers as static library
add_library(tree-sitter-parsers STATIC
    ${tree-sitter-cpp_SOURCE_DIR}/src/parser.c
    ${tree-sitter-cpp_SOURCE_DIR}/src/scanner.c
    ${tree-sitter-python_SOURCE_DIR}/src/parser.c
    ${tree-sitter-python_SOURCE_DIR}/src/scanner.c
    ${tree-sitter-javascript_SOURCE_DIR}/src/parser.c
    ${tree-sitter-javascript_SOURCE_DIR}/src/scanner.c
    ${tree-sitter-typescript_SOURCE_DIR}/typescript/src/parser.c
    ${tree-sitter-typescript_SOURCE_DIR}/typescript/src/scanner.c
    ${tree-sitter-go_SOURCE_DIR}/src/parser.c
    ${tree-sitter-rust_SOURCE_DIR}/src/parser.c
    ${tree-sitter-rust_SOURCE_DIR}/src/scanner.c
    ${tree-sitter-java_SOURCE_DIR}/src/parser.c
    ${tree-sitter-ruby_SOURCE_DIR}/src/parser.c
    ${tree-sitter-ruby_SOURCE_DIR}/src/scanner.c
    ${tree-sitter-c-sharp_SOURCE_DIR}/src/parser.c
    ${tree-sitter-c-sharp_SOURCE_DIR}/src/scanner.c
    ${tree-sitter-swift_SOURCE_DIR}/src/parser.c
    ${tree-sitter-swift_SOURCE_DIR}/src/scanner.c
    ${tree-sitter-lua_SOURCE_DIR}/src/parser.c
    ${tree-sitter-lua_SOURCE_DIR}/src/scanner.c
)
target_include_directories(tree-sitter-parsers PUBLIC
    ${tree-sitter_SOURCE_DIR}/lib/include
    ${tree-sitter-cpp_SOURCE_DIR}/src
    ${tree-sitter-python_SOURCE_DIR}/src
    ${tree-sitter-javascript_SOURCE_DIR}/src
    ${tree-sitter-typescript_SOURCE_DIR}/typescript/src
    ${tree-sitter-go_SOURCE_DIR}/src
    ${tree-sitter-rust_SOURCE_DIR}/src
    ${tree-sitter-java_SOURCE_DIR}/src
    ${tree-sitter-ruby_SOURCE_DIR}/src
    ${tree-sitter-c-sharp_SOURCE_DIR}/src
    ${tree-sitter-swift_SOURCE_DIR}/src
    ${tree-sitter-lua_SOURCE_DIR}/src
)
target_link_libraries(tree-sitter-parsers PUBLIC tree-sitter)

# Additional grammars use immutable revisions and the same offline source-cache
# overrides as the original FetchContent dependencies. Compile each scanner
# against its own generated headers (grammars can vendor different ABI headers).
function(chitta_grammar name repository revision)
    FetchContent_Declare(tree-sitter-${name}
        GIT_REPOSITORY https://github.com/${repository}.git GIT_TAG ${revision})
    FetchContent_GetProperties(tree-sitter-${name})
    if(NOT tree-sitter-${name}_POPULATED)
        FetchContent_Populate(tree-sitter-${name})
    endif()
    set(source "${tree-sitter-${name}_SOURCE_DIR}/src")
    if(ARGC GREATER 3)
        set(source "${tree-sitter-${name}_SOURCE_DIR}/${ARGV3}/src")
    endif()
    add_library(grammar-${name} STATIC "${source}/parser.c")
    foreach(scanner scanner.c scanner.cc)
        if(EXISTS "${source}/${scanner}")
            target_sources(grammar-${name} PRIVATE "${source}/${scanner}")
        endif()
    endforeach()
    target_include_directories(grammar-${name} PRIVATE "${source}")
    target_link_libraries(tree-sitter-parsers PUBLIC grammar-${name})
endfunction()
chitta_grammar(bash tree-sitter/tree-sitter-bash 487734f87fd87118028a65a4599352fa99c9cde8)
chitta_grammar(r r-lib/tree-sitter-r 58a22794466c0fc15b0d3b40531db751593721e8)
chitta_grammar(julia tree-sitter/tree-sitter-julia a8e1262997d5a45520a06cbe1b86c0737d507054)
chitta_grammar(fortran stadelmanma/tree-sitter-fortran 6f16bab7455cd2906a86679a1be83dd4f378db81)
chitta_grammar(nextflow nextflow-io/tree-sitter-nextflow 69a72c6cf4e90609b8558fc05d7d17a5f3df7894)
chitta_grammar(snakemake osthomas/tree-sitter-snakemake 315d8f4a55800bcacc2e420d2865d7f64b33efe6)
chitta_grammar(perl tree-sitter-perl/tree-sitter-perl aa5600c8df1eb9ed111db72e20c741dd73ec336a)
chitta_grammar(make alemuller/tree-sitter-make a4b9187417d6be349ee5fd4b6e77b4172c6827dd)
chitta_grammar(cmake uyha/tree-sitter-cmake 58993af75218bc99a1f5a04c832a5937e7c422cb)
chitta_grammar(sql DerekStride/tree-sitter-sql 593a5ecc5dc3889890d8b24ba8fa7487ee01bfe5)
option(CHITTA_EXTRA_GRAMMARS "Build the additional Graphify-parity language grammars" ON)
if(CHITTA_EXTRA_GRAMMARS)
    target_compile_definitions(tree-sitter-parsers PUBLIC CHITTA_EXTRA_GRAMMARS=1)
    chitta_grammar(zig tree-sitter-grammars/tree-sitter-zig 6479aa13f32f701c383083d8b28360ebd682fb7d)
    chitta_grammar(scala tree-sitter/tree-sitter-scala db390f312a54b04b13790e1767bfac32665c17ac)
    chitta_grammar(kotlin fwcd/tree-sitter-kotlin 1852ea17b7f60fb3f9d84e0b1555d56b46b39fb1)
    chitta_grammar(php tree-sitter/tree-sitter-php 3fda2fb9577166c6399834917f9844f30370beea php)
endif()


# Extract version from plugin.json
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../.claude-plugin/plugin.json" PLUGIN_JSON)
string(REGEX MATCH "\"version\"[^:]*:[^\"]*\"([^\"]+)\"" _ ${PLUGIN_JSON})
set(CHITTA_VERSION "${CMAKE_MATCH_1}")
message(STATUS "cc-soul version: ${CHITTA_VERSION}")

# Generate version header
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/include/chitta/version.hpp.in"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/chitta/version.hpp"
    @ONLY
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Output binaries to ../bin/ for cleaner plugin structure
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../bin)

# Options
option(CHITTA_BUILD_TESTS "Build tests" ON)
option(CHITTA_BUILD_RPC "Build RPC server executable" ON)
option(CHITTA_WITH_LLAMA_CPP "Enable inline GGUF embeddings via llama.cpp (no Ollama required)" OFF)

# Header-only library
add_library(chitta INTERFACE)
target_include_directories(chitta INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# Find threads
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
target_link_libraries(chitta INTERFACE OpenSSL::Crypto)
target_link_libraries(chitta INTERFACE Threads::Threads roaring tree-sitter-parsers)

# nlohmann/json for RPC server
find_package(nlohmann_json CONFIG QUIET)
if(nlohmann_json_FOUND)
    target_link_libraries(chitta INTERFACE nlohmann_json::nlohmann_json)
    target_compile_definitions(chitta INTERFACE CHITTA_WITH_JSON=1)
    message(STATUS "nlohmann_json found via CONFIG")
else()
    # Check user-specified path first, then conda
    if(NOT JSON_INCLUDE_DIR AND DEFINED ENV{CONDA_PREFIX})
        set(JSON_INCLUDE_DIR "$ENV{CONDA_PREFIX}/include")
    endif()
    if(JSON_INCLUDE_DIR AND EXISTS "${JSON_INCLUDE_DIR}/nlohmann/json.hpp")
        target_include_directories(chitta INTERFACE ${JSON_INCLUDE_DIR})
        target_compile_definitions(chitta INTERFACE CHITTA_WITH_JSON=1)
        message(STATUS "nlohmann_json found at ${JSON_INCLUDE_DIR}")
    else()
        # Download via CPM as last resort
        CPMAddPackage(
            NAME nlohmann_json
            VERSION 3.11.3
            GITHUB_REPOSITORY nlohmann/json
            OPTIONS "JSON_BuildTests OFF"
        )
        target_link_libraries(chitta INTERFACE nlohmann_json::nlohmann_json)
        target_compile_definitions(chitta INTERFACE CHITTA_WITH_JSON=1)
        message(STATUS "nlohmann_json downloaded via CPM")
    endif()
endif()

# llama.cpp (optional) — in-process GGUF embeddings, no external server required.
# Build with CUDA: cmake -DCHITTA_WITH_LLAMA_CPP=ON -DGGML_CUDA=ON (requires nvcc on build node)
# CPU-only:        cmake -DCHITTA_WITH_LLAMA_CPP=ON
if(CHITTA_WITH_LLAMA_CPP)
    # Use local clone if LLAMA_CPP_SOURCE_DIR is set (e.g. for Qwen3 support), else fetch b4990.
    if(DEFINED LLAMA_CPP_SOURCE_DIR AND EXISTS "${LLAMA_CPP_SOURCE_DIR}/CMakeLists.txt")
        CPMAddPackage(
            NAME llama_cpp
            SOURCE_DIR "${LLAMA_CPP_SOURCE_DIR}"
            OPTIONS
                "LLAMA_BUILD_TESTS OFF"
                "LLAMA_BUILD_EXAMPLES OFF"
                "LLAMA_BUILD_SERVER OFF"
                "LLAMA_CURL OFF"
                "BUILD_SHARED_LIBS OFF"
        )
        message(STATUS "llama.cpp enabled (local: ${LLAMA_CPP_SOURCE_DIR})")
    else()
        CPMAddPackage(
            NAME llama_cpp
            GITHUB_REPOSITORY ggml-org/llama.cpp
            GIT_TAG b9294
            OPTIONS
                "LLAMA_BUILD_TESTS OFF"
                "LLAMA_BUILD_EXAMPLES OFF"
                "LLAMA_BUILD_SERVER OFF"
                "LLAMA_CURL OFF"
                "BUILD_SHARED_LIBS OFF"
        )
        message(STATUS "llama.cpp enabled (tag b9294, GPU via -DGGML_CUDA=ON)")
    endif()
    target_compile_definitions(chitta INTERFACE CHITTA_WITH_LLAMA_CPP=1)
endif()

# chitta-field — required storage backend
# Resolve root: prefer CHITTA_FIELD_ROOT env/cmake var, then sibling submodule, then legacy path
if(NOT CHITTA_FIELD_ROOT)
    # Submodule path: cc-soul/chitta-field (one level up from chitta/)
    set(_CF_SUBMODULE "${CMAKE_CURRENT_SOURCE_DIR}/../chitta-field")
    if(EXISTS "${_CF_SUBMODULE}/Cargo.toml")
        set(CHITTA_FIELD_ROOT "${_CF_SUBMODULE}")
    else()
        message(FATAL_ERROR "chitta-field submodule not found at ${_CF_SUBMODULE}.\n"
            "Run: git submodule update --init chitta-field && cd chitta-field && ./build.sh\n"
            "Or set -DCHITTA_FIELD_ROOT=/path/to/chitta-field on the cmake command line.")
    endif()
endif()
set(CHITTA_FIELD_LIB "${CHITTA_FIELD_ROOT}/target/release/libchitta_field.a")
set(CHITTA_FIELD_INCLUDE "${CHITTA_FIELD_ROOT}/include")

if(NOT EXISTS "${CHITTA_FIELD_LIB}")
    message(FATAL_ERROR "chitta-field lib not found at ${CHITTA_FIELD_LIB} — build it first: cd chitta-field && ./build.sh")
endif()
message(STATUS "chitta-field: ${CHITTA_FIELD_LIB}")

# Warnings are enabled per-target, never globally: the vendored llama.cpp and
# tree-sitter trees are not ours to fix and would bury first-party diagnostics.
set(CHITTA_WARN_FLAGS -Wall -Wextra)

# One implementation for every CodeIntel user. New grammar dispatch lives in
# the .cpp; sharing it also avoids recompiling all extractors for each test.
add_library(chitta-code-intel STATIC src/code_intel.cpp)
target_link_libraries(chitta-code-intel PUBLIC chitta)
target_compile_options(chitta-code-intel PRIVATE ${CHITTA_WARN_FLAGS})

# Daemon/admin tool
add_executable(chittad
    src/simple_cli.cpp
    src/code_navigation.cpp
    src/queue_processor.cpp
    src/daemon_lifecycle.cpp
    src/distillation.cpp
    src/socket_server.cpp
    src/socket_client.cpp
    src/subconscious.cpp
    src/soul_controller.cpp
    src/transcript_parser.cpp
    src/ssl_parser.cpp
    src/native_distiller.cpp
    src/value_fact_extractor.cpp
    src/ingester.cpp
    src/wiki_export.cpp
    src/embedding_export.cpp
    src/temporal.cpp
    src/query_intent.cpp
    src/sadhana/brain_provider.cpp
    src/sadhana/sadhana_manager.cpp
    src/handlers/field_skill.cpp
    src/handlers/field_agent.cpp
    src/handlers/agent_protocol.cpp
    src/handlers/compact.cpp
    src/handlers/constraint.cpp
    src/handlers/drift_5w.cpp
    src/handlers/drift_consolidation.cpp
    src/handlers/drift_probe.cpp
    src/handlers/drift_recall.cpp
    src/handlers/drift_recon.cpp
    src/handlers/field_code_intel.cpp
    src/handlers/field_contradiction.cpp
    src/handlers/field_write_gate.cpp
    src/handlers/field_symbol_events.cpp
    src/handlers/field_introspection.cpp
    src/handlers/field_distill.cpp
    src/handlers/field_lookup.cpp
    src/handlers/field_memory_ops.cpp
    src/handlers/field_memory_recall.cpp
    src/handlers/field_memory_structured.cpp
    src/handlers/field_misc_sadhana.cpp
    src/handlers/field_misc_anticipation.cpp
    src/handlers/field_misc_profile_goal.cpp
    src/handlers/field_misc_memory.cpp
    src/handlers/field_misc_file_index.cpp
    src/handlers/field_misc_knowledge.cpp
    src/handlers/field_misc_io.cpp
    src/handlers/field_operator.cpp
    src/handlers/field_session.cpp
    src/handlers/field_task_ledger.cpp
    src/handlers/field_prompt_context.cpp
    src/handlers/field_system.cpp
    src/handlers/intervention.cpp
    src/handlers/learning.cpp
    src/handlers/ledger.cpp
    src/handlers/long_task.cpp
    src/handlers/meta_memory.cpp
    src/handlers/repl_sessions.cpp
    src/handlers/trajectory_compact.cpp
    src/handlers/wisdom_lineage.cpp
    src/handlers/register_tools.cpp
    src/handlers/register_memory_core_tools.cpp
    src/handlers/register_code_intel_tools.cpp
    src/handlers/register_distill_drift_tools.cpp
    src/handlers/register_session_transcript_tools.cpp
    src/handlers/register_system_tools.cpp
    src/handlers/register_misc_tools.cpp
    src/handlers/register_protocol_tools.cpp
)
target_include_directories(chittad PRIVATE "${CHITTA_FIELD_INCLUDE}")
find_package(Python3 REQUIRED COMPONENTS Interpreter Development)
find_package(BLAS REQUIRED)
target_link_libraries(chittad PRIVATE chitta-code-intel chitta "${CHITTA_FIELD_LIB}" dl pthread Python3::Python ${BLAS_LIBRARIES})
find_package(OpenSSL REQUIRED)
target_link_libraries(chittad PRIVATE OpenSSL::Crypto)
if(CHITTA_WITH_LLAMA_CPP)
    target_link_libraries(chittad PRIVATE llama ggml)
endif()
list(APPEND CHITTA_RUNTIME_LIBS "${Python3_LIBRARY_DIRS}")
target_compile_definitions(chittad PRIVATE CHITTA_FIELD_AVAILABLE=1)
target_compile_options(chittad PRIVATE ${CHITTA_WARN_FLAGS})

# Resident hint-extraction daemon (chitta_hintd) — wraps one HintYantra over a
# Unix socket so the realtime-hint backend loads the hint GGUF once instead of
# cold-loading ~850 MB on every fire. Only meaningful with llama.cpp compiled in.
if(CHITTA_WITH_LLAMA_CPP)
    add_executable(chitta_hintd src/hint_daemon.cpp)
    target_link_libraries(chitta_hintd PRIVATE chitta llama ggml pthread)
    install(TARGETS chitta_hintd DESTINATION bin)
endif()

# CLI tool for memory operations (thin client + MCP server)
# Output as 'chitta' binary (target named chitta_tool to avoid library conflict)
if(CHITTA_BUILD_RPC)
    add_executable(chitta_tool
        src/rpc_server.cpp
        src/socket_client.cpp
    )
    set_target_properties(chitta_tool PROPERTIES OUTPUT_NAME chitta)
    target_link_libraries(chitta_tool PRIVATE chitta)
    target_include_directories(chitta_tool PRIVATE "${CHITTA_FIELD_INCLUDE}")
    target_compile_options(chitta_tool PRIVATE ${CHITTA_WARN_FLAGS})
endif()

# Thinking block insight extractor — perception-change mining from session JSONL
add_executable(chitta_thinking src/thinking_extract.cpp src/socket_client.cpp)
target_link_libraries(chitta_thinking PRIVATE chitta)
target_include_directories(chitta_thinking PRIVATE "${CHITTA_FIELD_INCLUDE}")
install(TARGETS chitta_thinking DESTINATION bin)

# Unified migration tool: chitta_migrate
# Handles all source formats (DuckDB, soul.db) with auto-detection
add_executable(chitta_migrate src/migrate.cpp)
target_link_libraries(chitta_migrate PRIVATE chitta)
target_include_directories(chitta_migrate PRIVATE "${CHITTA_FIELD_INCLUDE}")
install(TARGETS chitta_migrate DESTINATION bin)

# Unit tests (assert-style, no framework) — run with: ctest --test-dir build
#
# These tests carry their checks in assert(), and CMAKE_BUILD_TYPE=Release adds
# -DNDEBUG, which compiles assert() to nothing. Every test therefore PASSED
# VACUOUSLY in a Release build — verified 2026-09-02: a `assert(1==2)` binary
# built with -O3 -DNDEBUG runs to completion and exits 0. CHITTA_TEST_TARGETS
# below re-enables assertions per test target so a green ctest means something.
# Add new test targets to that list, or their checks silently evaporate.
if(CHITTA_BUILD_TESTS)
    add_executable(hook_ledger_policy_test src/hook_ledger_policy_test.cpp)
    target_link_libraries(hook_ledger_policy_test PRIVATE chitta)
    add_test(NAME hook_ledger_policy_test COMMAND hook_ledger_policy_test)
    add_executable(prompt_policy_test src/prompt_policy_test.cpp)
    target_link_libraries(prompt_policy_test PRIVATE chitta)
    add_test(NAME prompt_policy_test COMMAND prompt_policy_test)
    enable_testing()
    if(CHITTA_BUILD_RPC)
        add_test(NAME daemon_isolation_test
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/src/daemon_isolation_test.py
                $<TARGET_FILE:chittad> $<TARGET_FILE:chitta_tool>
                ${CMAKE_CURRENT_SOURCE_DIR}/../hooks/lib.sh ${CHITTA_EMBED_DIM})
        set_tests_properties(daemon_isolation_test PROPERTIES TIMEOUT 240)
        add_test(NAME task_ledger_integration_test
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/src/task_ledger_integration_test.py
                $<TARGET_FILE:chittad> ${CHITTA_EMBED_DIM})
        set_tests_properties(task_ledger_integration_test PROPERTIES TIMEOUT 240)
    endif()
    # Compile the production probe verbatim without broadening its public API.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS src/simple_cli.cpp)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/simple_cli.cpp" _cli_source)
    string(FIND "${_cli_source}" "static unsigned long long spawn_format_id_probe(" _probe_start)
    string(FIND "${_cli_source}" "\nint cmd_daemon(" _probe_end)
    if(_probe_start LESS 0 OR _probe_end LESS _probe_start)
        message(FATAL_ERROR "Cannot locate production format-id probe for regression test")
    endif()
    math(EXPR _probe_length "${_probe_end} - ${_probe_start}")
    string(SUBSTRING "${_cli_source}" ${_probe_start} ${_probe_length} _probe_source)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/format_id_probe.inc" "${_probe_source}")
    find_package(BLAS REQUIRED)
    add_executable(subprocess_load_test src/subprocess_load_test.cpp)
    target_include_directories(subprocess_load_test PRIVATE "${CMAKE_CURRENT_BINARY_DIR}" ${BLAS_INCLUDE_DIRS})
    target_link_libraries(subprocess_load_test PRIVATE chitta ${BLAS_LIBRARIES} Threads::Threads)
    if(CHITTA_WITH_LLAMA_CPP)
        target_link_libraries(subprocess_load_test PRIVATE llama ggml)
    endif()
    target_compile_options(subprocess_load_test PRIVATE -UNDEBUG)
    add_test(NAME subprocess_load_test COMMAND subprocess_load_test $<TARGET_FILE:chittad>)
    set_tests_properties(subprocess_load_test PROPERTIES TIMEOUT 90 ENVIRONMENT "OPENBLAS_NUM_THREADS=2")
    if(UNIX AND NOT APPLE)
        add_library(save_fault_test SHARED src/save_fault_test.cpp)
        target_link_libraries(save_fault_test PRIVATE ${CMAKE_DL_LIBS})
        add_executable(chaos_fixture_test src/chaos_fixture_test.cpp)
        target_include_directories(chaos_fixture_test PRIVATE "${CHITTA_FIELD_INCLUDE}")
        target_compile_definitions(chaos_fixture_test PRIVATE CHITTA_FIELD_AVAILABLE=1)
        target_compile_options(chaos_fixture_test PRIVATE -UNDEBUG)
        target_link_libraries(chaos_fixture_test PRIVATE chitta "${CHITTA_FIELD_LIB}" dl pthread Python3::Python ${BLAS_LIBRARIES})
        if(CHITTA_BUILD_RPC)
            foreach(_case snapshot second wal lock disk queue hook mcp)
                add_test(NAME chaos_${_case}_test
                    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/chaos-replica.py
                        --fixture $<TARGET_FILE:chaos_fixture_test> --cases ${_case}
                        --daemon $<TARGET_FILE:chittad> --cli $<TARGET_FILE:chitta_tool>
                        --fault-library $<TARGET_FILE:save_fault_test> --dimension ${CHITTA_EMBED_DIM})
                set_tests_properties(chaos_${_case}_test PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77)
            endforeach()
        endif()

    endif()
    add_executable(task_ledger_test src/task_ledger_test.cpp)
    target_link_libraries(task_ledger_test PRIVATE chitta)
    add_test(NAME task_ledger_test COMMAND task_ledger_test)
    add_executable(rpc_publication_test src/rpc_publication_test.cpp src/code_navigation.cpp)
    target_include_directories(rpc_publication_test PRIVATE "${CHITTA_FIELD_INCLUDE}")
    target_link_libraries(rpc_publication_test PRIVATE chitta-code-intel chitta OpenSSL::Crypto)
    target_compile_options(rpc_publication_test PRIVATE -UNDEBUG ${CHITTA_WARN_FLAGS})
    add_test(NAME rpc_publication_test COMMAND rpc_publication_test)
    set_tests_properties(rpc_publication_test PROPERTIES TIMEOUT 15)
    if(UNIX AND NOT APPLE)
        add_executable(global_lock_test src/global_lock_test.cpp src/code_navigation.cpp)
        target_include_directories(global_lock_test PRIVATE "${CHITTA_FIELD_INCLUDE}")
        target_compile_options(global_lock_test PRIVATE -UNDEBUG ${CHITTA_WARN_FLAGS})
        target_link_libraries(global_lock_test PRIVATE chitta-code-intel chitta OpenSSL::Crypto "${CHITTA_FIELD_LIB}" dl pthread Python3::Python ${BLAS_LIBRARIES})
        target_link_options(global_lock_test PRIVATE "-Wl,--wrap=cf_sync")
        add_test(NAME global_lock_on_test COMMAND global_lock_test 1)
        set_tests_properties(global_lock_on_test PROPERTIES ENVIRONMENT "CHITTA_GLOBAL_LOCK=1" TIMEOUT 15)
        add_test(NAME global_lock_off_test COMMAND global_lock_test 0)
        set_tests_properties(global_lock_off_test PROPERTIES ENVIRONMENT "CHITTA_GLOBAL_LOCK=0" TIMEOUT 15)
        add_test(NAME global_lock_invalid_test COMMAND global_lock_test 1)
        set_tests_properties(global_lock_invalid_test PROPERTIES ENVIRONMENT "CHITTA_GLOBAL_LOCK=invalid" TIMEOUT 15)
        add_test(NAME global_lock_default_test COMMAND ${CMAKE_COMMAND} -E env --unset=CHITTA_GLOBAL_LOCK $<TARGET_FILE:global_lock_test> 1)
        set_tests_properties(global_lock_default_test PROPERTIES TIMEOUT 15)
    endif()
    add_executable(thread_pool_test src/thread_pool_test.cpp)
    target_link_libraries(thread_pool_test PRIVATE chitta)
    add_test(NAME thread_pool_test COMMAND thread_pool_test)
    add_executable(llm_probe_test src/llm_probe_test.cpp)
    target_link_libraries(llm_probe_test PRIVATE chitta)
    add_test(NAME llm_probe_test COMMAND llm_probe_test)
    add_executable(callsite_extract_test src/callsite_extract_test.cpp)
    target_compile_options(callsite_extract_test PRIVATE -UNDEBUG)
    target_link_libraries(callsite_extract_test PRIVATE chitta-code-intel chitta)
    add_test(NAME callsite_extract_test COMMAND callsite_extract_test)
    add_executable(repository_index_test ../hooks/tests/repository_index_test.cpp)
    target_compile_options(repository_index_test PRIVATE -UNDEBUG)
    target_link_libraries(repository_index_test PRIVATE chitta-code-intel chitta OpenSSL::Crypto)
    add_test(NAME repository_index_test COMMAND repository_index_test)
    add_executable(code_languages_test src/code_languages_test.cpp)
    target_compile_options(code_languages_test PRIVATE -UNDEBUG)
    target_link_libraries(code_languages_test PRIVATE chitta-code-intel chitta OpenSSL::Crypto)
    add_test(NAME code_languages_test COMMAND code_languages_test
        "${CMAKE_CURRENT_SOURCE_DIR}/../hooks/tests/fixtures/codenav")
    add_executable(code_index_test src/code_index_test.cpp)
    target_compile_options(code_index_test PRIVATE -UNDEBUG)
    target_link_libraries(code_index_test PRIVATE chitta-code-intel chitta OpenSSL::Crypto)
    add_test(NAME code_index_test COMMAND code_index_test)
    add_executable(code_navigation_test src/code_navigation_test.cpp src/code_navigation.cpp)
    target_compile_options(code_navigation_test PRIVATE -UNDEBUG)
    target_link_libraries(code_navigation_test PRIVATE chitta-code-intel chitta OpenSSL::Crypto)
    add_test(NAME code_navigation_test COMMAND code_navigation_test)
    add_test(NAME code_index_integration_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/src/code_index_integration_test.py
                $<TARGET_FILE:chittad> $<TARGET_FILE:chitta_tool>)
    add_executable(value_fact_extract_test
        src/value_fact_extract_test.cpp src/value_fact_extractor.cpp)
    add_test(NAME value_fact_extract_test COMMAND value_fact_extract_test)
    add_executable(prefilter_keep_test src/prefilter_keep_test.cpp)
    add_test(NAME prefilter_keep_test COMMAND prefilter_keep_test)
    add_executable(hit_line_test src/hit_line_test.cpp)
    target_link_libraries(hit_line_test PRIVATE chitta)
    target_compile_options(hit_line_test PRIVATE ${CHITTA_WARN_FLAGS})
    add_test(NAME hit_line_test COMMAND hit_line_test)
    add_executable(tool_spec_test src/tool_spec_test.cpp)
    target_link_libraries(tool_spec_test PRIVATE chitta)
    target_compile_options(tool_spec_test PRIVATE ${CHITTA_WARN_FLAGS})
    add_test(NAME tool_spec_test COMMAND tool_spec_test)
    add_executable(rpc_work_policy_test src/rpc_work_policy_test.cpp)
    target_link_libraries(rpc_work_policy_test PRIVATE chitta)
    add_test(NAME rpc_work_policy_test COMMAND rpc_work_policy_test)
    add_executable(recall_lanes_test src/recall_lanes_test.cpp)
    target_link_libraries(recall_lanes_test PRIVATE chitta)
    add_test(NAME recall_lanes_test COMMAND recall_lanes_test)
    add_executable(maintenance_jitter_test src/maintenance_jitter_test.cpp)
    target_link_libraries(maintenance_jitter_test PRIVATE chitta)
    add_test(NAME maintenance_jitter_test COMMAND maintenance_jitter_test)

    add_executable(embed_cache_test src/embed_cache_test.cpp)
    target_include_directories(embed_cache_test PRIVATE "${CHITTA_FIELD_INCLUDE}")
    target_link_libraries(embed_cache_test PRIVATE chitta)
    add_test(NAME embed_cache_test COMMAND embed_cache_test)
    if(CHITTA_WITH_LLAMA_CPP)
        add_executable(embed_pool_test src/embed_pool_test.cpp)
        target_link_libraries(embed_pool_test PRIVATE chitta llama ggml Threads::Threads)
        target_compile_options(embed_pool_test PRIVATE -UNDEBUG ${CHITTA_WARN_FLAGS})
        add_test(NAME embed_pool_test COMMAND embed_pool_test)
        set_tests_properties(embed_pool_test PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180)
    endif()

    set(CHITTA_TEST_TARGETS
        hook_ledger_policy_test
        prompt_policy_test
        task_ledger_test
        thread_pool_test llm_probe_test callsite_extract_test
        value_fact_extract_test prefilter_keep_test
        hit_line_test tool_spec_test rpc_work_policy_test recall_lanes_test
        maintenance_jitter_test embed_cache_test)
    foreach(t ${CHITTA_TEST_TARGETS})
        target_compile_options(${t} PRIVATE -UNDEBUG ${CHITTA_WARN_FLAGS})
    endforeach()
    add_test(NAME prefilter_keep_sync COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/src/prefilter_keep_sync.sh)
endif()

# RPATH configuration for runtime library discovery
# CI builds can override with CMAKE_INSTALL_RPATH=$ORIGIN to bundle libs
set(CHITTA_RUNTIME_LIBS "")


# Remove duplicates
list(REMOVE_DUPLICATES CHITTA_RUNTIME_LIBS)

# Set RPATH for all executables
# If CMAKE_INSTALL_RPATH is set externally (e.g., $ORIGIN for bundled builds), use it
# Otherwise, use the discovered library paths
if(CMAKE_INSTALL_RPATH)
    set(CHITTA_RPATH "${CMAKE_INSTALL_RPATH}")
    message(STATUS "Using externally specified RPATH: ${CHITTA_RPATH}")
else()
    string(JOIN ":" CHITTA_RPATH ${CHITTA_RUNTIME_LIBS})
    message(STATUS "Using discovered library RPATH: ${CHITTA_RPATH}")
endif()

# Apply RPATH to all executables
foreach(exe chittad)
    if(TARGET ${exe})
        set_target_properties(${exe} PROPERTIES
            BUILD_RPATH "${CHITTA_RPATH}"
            INSTALL_RPATH "${CHITTA_RPATH}"
        )
    endif()
endforeach()

if(TARGET chitta_tool)
    set_target_properties(chitta_tool PROPERTIES
        BUILD_RPATH "${CHITTA_RPATH}"
        INSTALL_RPATH "${CHITTA_RPATH}"
    )
endif()


# Install
install(DIRECTORY include/ DESTINATION include)
install(TARGETS chitta EXPORT chittaTargets)
