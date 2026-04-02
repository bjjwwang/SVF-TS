add_executable(cts-test
    cts-test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/CTSOverrides.cpp
)

target_link_libraries(cts-test PRIVATE SvfCts)

target_include_directories(cts-test PRIVATE
    ${tree-sitter_SOURCE_DIR}/lib/include
)

target_compile_options(cts-test PRIVATE -Wno-error)
