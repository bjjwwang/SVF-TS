add_executable(cts-ae
    cts-ae.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/CTSOverrides.cpp
)

target_link_libraries(cts-ae PRIVATE SvfCts)

target_include_directories(cts-ae PRIVATE
    ${tree-sitter_SOURCE_DIR}/lib/include
)

target_compile_options(cts-ae PRIVATE -Wno-error)
