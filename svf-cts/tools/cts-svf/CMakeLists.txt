add_executable(cts-svf
    cts-svf.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/CTSOverrides.cpp
)

target_link_libraries(cts-svf PRIVATE SvfCts)

target_include_directories(cts-svf PRIVATE
    ${tree-sitter_SOURCE_DIR}/lib/include
)

target_compile_options(cts-svf PRIVATE -Wno-error)
