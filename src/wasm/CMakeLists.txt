# WebAssembly module (Emscripten only). The size/exception flags and the
# dependency fetch are set by the root CMakeLists before this is added, so
# the fetched deps inherit them. Sources are enumerated explicitly so a
# new .cpp added to a native lib doesn't silently become part of the
# WASM target (it might pull in a platform-specific header, or it might
# legitimately be native-only).

set(CADML_WASM_SRC
    # src/cadml — parser, types, expression evaluator, SHA-256, Lua, selector.
    ${CMAKE_SOURCE_DIR}/src/cadml/src/expression.cpp
    ${CMAKE_SOURCE_DIR}/src/cadml/src/lua.cpp
    ${CMAKE_SOURCE_DIR}/src/cadml/src/math.cpp
    ${CMAKE_SOURCE_DIR}/src/cadml/src/parser_body.cpp
    ${CMAKE_SOURCE_DIR}/src/cadml/src/parser_frontmatter.cpp
    ${CMAKE_SOURCE_DIR}/src/cadml/src/selector.cpp
    ${CMAKE_SOURCE_DIR}/src/cadml/src/serializer.cpp
    ${CMAKE_SOURCE_DIR}/src/cadml/src/sha256.cpp
    ${CMAKE_SOURCE_DIR}/src/cadml/src/types.cpp
    # src/cadml_compile — bundler + composition lowerings.
    ${CMAKE_SOURCE_DIR}/src/cadml_compile/src/assembly_compiler.cpp
    ${CMAKE_SOURCE_DIR}/src/cadml_compile/src/bundler.cpp
    ${CMAKE_SOURCE_DIR}/src/cadml_compile/src/lua_string_eval.cpp
    ${CMAKE_SOURCE_DIR}/src/cadml_compile/src/subtree_ops.cpp
    ${CMAKE_SOURCE_DIR}/src/cadml_compile/src/unrollers.cpp
    # src/engine — flat evaluator + exporters + analysis.
    ${CMAKE_SOURCE_DIR}/src/engine/src/flat_3mf.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/src/flat_analysis.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/src/flat_check.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/src/flat_color.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/src/flat_edge_topology.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/src/flat_evaluator.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/src/flat_geometry.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/src/flat_gltf.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/src/flat_mesh_cache.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/src/flat_stl.cpp)

add_executable(cadml_wasm cadml_wasm.cpp ${CADML_WASM_SRC})

target_include_directories(cadml_wasm PRIVATE
    ${CMAKE_SOURCE_DIR}/src/cadml/include
    ${CMAKE_SOURCE_DIR}/src/cadml_compile/include
    ${CMAKE_SOURCE_DIR}/src/engine/include
    ${CMAKE_SOURCE_DIR}/src/cadml_compile/src   # source_provider.hpp etc.
    ${CMAKE_SOURCE_DIR}/src/engine/src)         # private flat_* headers

target_link_libraries(cadml_wasm PRIVATE
    manifold::manifold
    Clipper2::Clipper2
    pugixml::pugixml
    miniz::miniz
    sol2::sol2          # brings Lua::Lua (and <lua.hpp>) transitively
    rapidjson
    picosha2)

target_link_options(cadml_wasm PRIVATE
    --bind
    -sMODULARIZE=1
    -sEXPORT_NAME=createCadml
    -sENVIRONMENT=web,worker,node
    -sALLOW_MEMORY_GROWTH=1
    -sSTACK_SIZE=4MB        # default 64 KB is too small for deep import chains
    -sFILESYSTEM=0          # pipeline is pure in-memory
    -sEXPORTED_RUNTIME_METHODS=['ccall'])

# Emit cadml.js (+ cadml.wasm) at the build-dir root (build/wasm/), not the
# subdir default — keeps the node test + CI paths stable.
set_target_properties(cadml_wasm PROPERTIES
    OUTPUT_NAME "cadml"
    SUFFIX ".js"
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
