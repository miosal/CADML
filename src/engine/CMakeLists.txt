add_library(cadml_engine
    src/flat_3mf.cpp
    src/flat_analysis.cpp
    src/flat_check.cpp
    src/flat_color.cpp
    src/flat_edge_topology.cpp
    src/flat_evaluator.cpp
    src/flat_geometry.cpp
    src/flat_gltf.cpp
    src/flat_mesh_cache.cpp
    src/flat_stl.cpp
)

target_include_directories(cadml_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(cadml_engine PUBLIC cxx_std_20)
cadml_strict_warnings(cadml_engine)
add_library(cadml::engine ALIAS cadml_engine)

target_link_libraries(cadml_engine
    PUBLIC
        cadml_types
    PRIVATE
        manifold::manifold
        Clipper2::Clipper2
        rapidjson
        miniz::miniz
)

if (CADML_BUILD_TESTS)
    add_executable(cadml_engine_tests
        tests/test_flat_analysis.cpp
        tests/test_flat_color.cpp
        tests/test_flat_evaluator.cpp
    )
    target_link_libraries(cadml_engine_tests
        PRIVATE
            cadml_engine
            cadml_types
            cadml_compile
            rapidjson
            miniz::miniz
            pugixml::pugixml
            GTest::gtest
            GTest::gtest_main
    )
    target_compile_features(cadml_engine_tests PRIVATE cxx_std_20)
    target_include_directories(cadml_engine_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME cadml_engine_tests COMMAND cadml_engine_tests)
endif()
