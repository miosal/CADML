# The bundler: lowers .cadml authoring files to flat .fcadml. Target
# name is cadml_compile; alias cadml::compile.

add_library(cadml_compile STATIC
    src/bundler.cpp
    src/lua_string_eval.cpp
    src/subtree_ops.cpp
    src/unrollers.cpp
    src/assembly_compiler.cpp
)

target_include_directories(cadml_compile PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(cadml_compile PUBLIC cadml_types)
target_compile_features(cadml_compile PUBLIC cxx_std_20)
cadml_strict_warnings(cadml_compile)
add_library(cadml::compile ALIAS cadml_compile)

if (CADML_BUILD_TESTS)
    add_executable(cadml_compile_tests
        tests/test_bundler.cpp
        tests/test_imports.cpp
        tests/test_in_memory.cpp
        tests/test_lua_imports.cpp
        tests/test_param_validation.cpp
        tests/test_dotted_port_paths.cpp
        tests/test_subtree_ops.cpp
        tests/test_for_unroller.cpp
        tests/test_pattern_unroller.cpp
        tests/test_assembly_compiler.cpp
        tests/test_port_alignment.cpp
    )
    target_link_libraries(cadml_compile_tests
        PRIVATE
            cadml_compile
            GTest::gtest
            GTest::gtest_main
    )
    # Tests need to include internal headers (detail::clone_subtree etc.).
    target_include_directories(cadml_compile_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME cadml_compile_tests COMMAND cadml_compile_tests)
endif()
