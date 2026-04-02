# =================================================================================
# svf-cts: C TreeSitter Frontend for SVF
# =================================================================================

# TreeSitter dependencies (FetchContent)
include(FetchContent)

FetchContent_Declare(
    tree-sitter
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter.git
    GIT_TAG v0.22.6
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(tree-sitter)

# Build tree-sitter core as object library
add_library(tree-sitter-obj OBJECT
    ${tree-sitter_SOURCE_DIR}/lib/src/lib.c
)
target_include_directories(tree-sitter-obj PUBLIC
    ${tree-sitter_SOURCE_DIR}/lib/include
    ${tree-sitter_SOURCE_DIR}/lib/src
)
set_target_properties(tree-sitter-obj PROPERTIES POSITION_INDEPENDENT_CODE ON)

FetchContent_Declare(
    tree-sitter-c
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-c.git
    GIT_TAG v0.21.4
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(tree-sitter-c)

# Build tree-sitter-c parser as object library
add_library(tree-sitter-c-obj OBJECT
    ${tree-sitter-c_SOURCE_DIR}/src/parser.c
)
target_include_directories(tree-sitter-c-obj PUBLIC
    ${tree-sitter-c_SOURCE_DIR}/src
    ${tree-sitter_SOURCE_DIR}/lib/include
)
set_target_properties(tree-sitter-c-obj PROPERTIES POSITION_INDEPENDENT_CODE ON)

# =================================================================================
# SvfCts - C TreeSitter frontend library
# =================================================================================

file(GLOB CTS_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/lib/*.cpp)

add_library(SvfCts STATIC
    ${CTS_SOURCES}
    $<TARGET_OBJECTS:tree-sitter-obj>
    $<TARGET_OBJECTS:tree-sitter-c-obj>
)

target_include_directories(SvfCts
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${tree-sitter_SOURCE_DIR}/lib/include
)

target_link_libraries(SvfCts
    PUBLIC
        SvfCore
)

# Disable -Werror for CTS code (SVF flags include -Werror)
target_compile_options(SvfCts PRIVATE -Wno-error)

# =================================================================================
# Tools
# =================================================================================

add_subdirectory(tools/cts-svf)
add_subdirectory(tools/cts-ae)
add_subdirectory(tools/cts-test)
