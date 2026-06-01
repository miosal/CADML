add_executable(cadml_examples_tests
    test_examples_compile.cpp
    test_examples_metrics.cpp
)

target_link_libraries(cadml_examples_tests
    PRIVATE
        cadml::compile
        cadml_engine
        cadml_types
        GTest::gtest
        GTest::gtest_main
)

target_compile_features(cadml_examples_tests PRIVATE cxx_std_20)

target_compile_definitions(cadml_examples_tests PRIVATE
    CADML_EXAMPLES_DIR="${CMAKE_SOURCE_DIR}/examples"
    CADML_METRICS_MANIFEST="${CMAKE_CURRENT_SOURCE_DIR}/example-metrics.txt"
)

add_test(NAME cadml_examples COMMAND cadml_examples_tests)

# Helper binary to refresh example-metrics.txt after an intentional
# engine change. Not added to ctest — run manually:
#   cmake --build --preset default --target cadml_regen_example_metrics
#   then run the cadml_regen_example_metrics binary from the build tree
#   (pass --stdout to print instead of overwriting the manifest).
add_executable(cadml_regen_example_metrics
    regen_example_metrics.cpp
)
target_link_libraries(cadml_regen_example_metrics
    PRIVATE
        cadml::compile
        cadml_engine
        cadml_types
)
target_compile_features(cadml_regen_example_metrics PRIVATE cxx_std_20)
target_compile_definitions(cadml_regen_example_metrics PRIVATE
    CADML_EXAMPLES_DIR="${CMAKE_SOURCE_DIR}/examples"
    CADML_METRICS_MANIFEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
