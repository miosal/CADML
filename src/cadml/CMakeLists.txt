# libcadml_types — AST types, parser, expression evaluator, Lua runtime,
# math/source utilities. Both cadml_compile and cadml_engine link
# against this.

add_library(cadml_types STATIC
    src/base64.cpp
    src/math.cpp
    src/types.cpp
    src/parser_frontmatter.cpp
    src/parser_body.cpp
    src/expression.cpp
    src/lua.cpp
    src/selector.cpp
    src/serializer.cpp
    src/sha256.cpp
)

target_include_directories(cadml_types PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(cadml_types PUBLIC cxx_std_20)
cadml_strict_warnings(cadml_types)
add_library(cadml::types ALIAS cadml_types)

target_link_libraries(cadml_types
    PRIVATE
        pugixml::pugixml
        sol2::sol2
        picosha2
)

if (CADML_BUILD_TESTS)
    add_executable(cadml_types_tests
        tests/test_base64.cpp
        tests/test_math.cpp
        tests/test_types.cpp
        tests/test_parser_frontmatter.cpp
        tests/test_parser_body.cpp
        tests/test_parser_full.cpp
        tests/test_expression.cpp
        tests/test_lua.cpp
        tests/test_selector.cpp
        tests/test_serializer.cpp
        tests/test_sha256.cpp
    )
    target_link_libraries(cadml_types_tests
        PRIVATE
            cadml_types
            GTest::gtest
            GTest::gtest_main
    )
    target_compile_features(cadml_types_tests PRIVATE cxx_std_20)
    add_test(NAME cadml_types_tests COMMAND cadml_types_tests)
endif()
