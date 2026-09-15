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

# `cadmlstl --part`: per-part export (used by hosts that need one mesh per
# part, e.g. textured renders) and the unknown-name failure path.
if (TARGET cadmlstl)
    add_test(NAME cadmlstl_part_filter
        COMMAND cadmlstl ${CMAKE_SOURCE_DIR}/examples/showcase-texture/showcase-texture.cadml
                -p tile -p cube -o ${CMAKE_CURRENT_BINARY_DIR}/part_filter.stl)
    set_tests_properties(cadmlstl_part_filter PROPERTIES
        PASS_REGULAR_EXPRESSION "from 2 part\\(s\\)")
    add_test(NAME cadmlstl_part_filter_unknown_name
        COMMAND cadmlstl ${CMAKE_SOURCE_DIR}/examples/showcase-texture/showcase-texture.cadml
                -p no-such-part -o ${CMAKE_CURRENT_BINARY_DIR}/part_filter_bad.stl)
    set_tests_properties(cadmlstl_part_filter_unknown_name PROPERTIES WILL_FAIL TRUE)
endif()
