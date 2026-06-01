# Native command-line tools. Built only for the native target.

# Compile-only tool: needs the bundler, not the engine.
add_executable(cadmlc cadml_compile_main.cpp)
target_link_libraries(cadmlc PRIVATE cadml_compile)

# Pipeline tools: compile -> evaluate_flat -> {export, analysis, check}.
set(CADML_PIPELINE_TOOLS
    cadmlstl:cadml_to_stl_main.cpp
    cadml3mf:cadml_to_3mf_main.cpp
    cadmlcheck:cadml_check_main.cpp
    cadmlbuild:cadml_build_main.cpp
    cadmlmass:cadml_mass_main.cpp
    cadmlbounds:cadml_bounds_main.cpp
    cadmltopo:cadml_topo_main.cpp
    cadmldiff:cadml_diff_main.cpp
    cadmlmeasure:cadml_measure_main.cpp
    cadmlholes:cadml_holes_main.cpp
    cadmlwalls:cadml_walls_main.cpp
)
foreach(entry IN LISTS CADML_PIPELINE_TOOLS)
    string(REPLACE ":" ";" parts "${entry}")
    list(GET parts 0 tool_name)
    list(GET parts 1 tool_src)
    add_executable(${tool_name} ${tool_src})
    target_link_libraries(${tool_name} PRIVATE cadml_engine cadml_compile)
endforeach()

# Language server: parser diagnostics over stdio (rapidjson for LSP JSON).
add_executable(cadmllsp cadml_lsp_main.cpp)
target_link_libraries(cadmllsp PRIVATE cadml_types rapidjson)

set(CADML_ALL_TOOLS
    cadmlc cadmlstl cadml3mf cadmlcheck cadmlbuild
    cadmlmass cadmlbounds cadmltopo cadmldiff cadmlmeasure
    cadmlholes cadmlwalls cadmllsp)
foreach(tool IN LISTS CADML_ALL_TOOLS)
    target_compile_features(${tool} PRIVATE cxx_std_20)
    cadml_strict_warnings(${tool})
endforeach()
