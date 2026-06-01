include(FetchContent)

set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING
    "Floor for fetched deps that predate CMake 3.5")

set(_CADML_HEADER_ONLY_SUBDIR "_cadml_headers_only_")

# Manifold + every other fetched dep that respects BUILD_SHARED_LIBS:
# set local (non-CACHE) overrides so the values flow into the
# fetched sub-builds without polluting the parent project's CMake
# cache. A downstream project that does
# `FetchContent_MakeAvailable(cadml)` then
# `FetchContent_MakeAvailable(some_other_dep)` keeps its own
# BUILD_SHARED_LIBS / BUILD_TESTS / BUILD_EXAMPLES intact —
# `CACHE BOOL "" FORCE` here would have written them.
set(MANIFOLD_TEST          OFF)
set(MANIFOLD_CROSS_SECTION OFF)
set(MANIFOLD_EXPORT        OFF)
set(MANIFOLD_DEBUG         OFF)
set(BUILD_SHARED_LIBS      OFF)
if(EMSCRIPTEN)
    set(MANIFOLD_PAR OFF)
else()
    set(MANIFOLD_PAR             ON)
    set(MANIFOLD_USE_BUILTIN_TBB ON)
endif()


FetchContent_Declare(manifold
    GIT_REPOSITORY https://github.com/elalish/manifold.git
    GIT_TAG v3.3.2 GIT_SHALLOW TRUE)

# pugixml
FetchContent_Declare(pugixml
    GIT_REPOSITORY https://github.com/zeux/pugixml.git
    GIT_TAG v1.15 GIT_SHALLOW TRUE)

# Clipper2
set(CLIPPER2_TESTS    OFF)
set(CLIPPER2_EXAMPLES OFF)
set(CLIPPER2_UTILS    OFF)
FetchContent_Declare(clipper2
    GIT_REPOSITORY https://github.com/AngusJohnson/Clipper2.git
    GIT_TAG Clipper2_1.5.4 SOURCE_SUBDIR CPP GIT_SHALLOW TRUE)

# miniz — uses the generic BUILD_EXAMPLES / BUILD_TESTS names so we
# scope these as local overrides (see the comment above the Manifold
# block for why CACHE FORCE would be wrong).
set(BUILD_EXAMPLES OFF)
set(BUILD_TESTS    OFF)
FetchContent_Declare(miniz
    GIT_REPOSITORY https://github.com/richgel999/miniz.git
    GIT_TAG 3.0.2 GIT_SHALLOW TRUE)

# Lua VM
FetchContent_Declare(lua
    URL https://www.lua.org/ftp/lua-5.4.7.tar.gz
    URL_HASH SHA256=9fbf5e28ef86c69858f6d3d34eccc32e911c1a28b4120ff3e84aaa70cfbf1e30)


# Header-only libs — fetched without invoking their CMake build.
FetchContent_Declare(sol2
    GIT_REPOSITORY https://github.com/ThePhD/sol2.git
    GIT_TAG v3.5.0
    SOURCE_SUBDIR ${_CADML_HEADER_ONLY_SUBDIR})
FetchContent_Declare(rapidjson
    GIT_REPOSITORY https://github.com/Tencent/rapidjson.git
    GIT_TAG 24b5e7a8b27f42fa16b96fc70aade9106cf7102f
    SOURCE_SUBDIR ${_CADML_HEADER_ONLY_SUBDIR})
FetchContent_Declare(picosha2
    GIT_REPOSITORY https://github.com/okdshin/PicoSHA2.git
    GIT_TAG 161cb3fc4170fa7a3eca9e582cebd27cc4d1fe29
    SOURCE_SUBDIR ${_CADML_HEADER_ONLY_SUBDIR})

FetchContent_MakeAvailable(manifold pugixml clipper2 miniz lua
                            sol2 rapidjson picosha2)

# ── Lua: build the library objects (everything except the lua/luac mains).
file(GLOB _CADML_LUA_SRC ${lua_SOURCE_DIR}/src/*.c)
list(REMOVE_ITEM _CADML_LUA_SRC
    ${lua_SOURCE_DIR}/src/lua.c
    ${lua_SOURCE_DIR}/src/luac.c)
add_library(cadml_lua_static STATIC ${_CADML_LUA_SRC})
target_include_directories(cadml_lua_static PUBLIC ${lua_SOURCE_DIR}/src)

# The C Lua tarball ships no lua.hpp (that C++ wrapper is a packager
# addition); sol2 includes <lua.hpp>. Generate a clean one that binds to
# the FetchContent Lua C headers.
set(_CADML_LUA_HPP_DIR ${CMAKE_BINARY_DIR}/lua_compat)
file(WRITE ${_CADML_LUA_HPP_DIR}/lua.hpp
"// Generated: C++ include wrapper for the FetchContent Lua C headers.\n"
"extern \"C\" {\n"
"#include \"lua.h\"\n"
"#include \"lualib.h\"\n"
"#include \"lauxlib.h\"\n"
"}\n")

# ── Normalised dependency targets ──────────────────────────────────────
# Make every dep reachable under one stable name regardless of how its
# upstream build happens to name things, so the rest of the tree (native
# AND wasm) links an identical set.

# Lua::Lua — the static lib plus its headers and the generated lua.hpp.
add_library(Lua::Lua INTERFACE IMPORTED)
target_link_libraries(Lua::Lua INTERFACE cadml_lua_static)
target_include_directories(Lua::Lua INTERFACE
    ${lua_SOURCE_DIR}/src
    ${_CADML_LUA_HPP_DIR})

# sol2::sol2 — header-only binding; pull in Lua so consumers get <lua.hpp>.
add_library(sol2::sol2 INTERFACE IMPORTED)
target_include_directories(sol2::sol2 INTERFACE ${sol2_SOURCE_DIR}/include)
target_link_libraries(sol2::sol2 INTERFACE Lua::Lua)

# rapidjson — header-only. (Name left un-namespaced to match the existing
# `if(TARGET rapidjson)` checks in the LSP / engine CMake.)
add_library(rapidjson INTERFACE IMPORTED)
target_include_directories(rapidjson INTERFACE ${rapidjson_SOURCE_DIR}/include)

# picosha2 — single-header SHA-256 (header lives at the repo root).
add_library(picosha2 INTERFACE IMPORTED)
target_include_directories(picosha2 INTERFACE ${picosha2_SOURCE_DIR})

# Clipper2 / miniz: upstream build-tree targets are un-namespaced. Add the
# conventional namespaced aliases so consumers can use either form.
if(TARGET Clipper2 AND NOT TARGET Clipper2::Clipper2)
    add_library(Clipper2::Clipper2 ALIAS Clipper2)
endif()
if(TARGET miniz AND NOT TARGET miniz::miniz)
    add_library(miniz::miniz ALIAS miniz)
endif()

# manifold::manifold and pugixml::pugixml are already created by their
# upstream FetchContent builds under those exact names.

# ── GoogleTest (native test build only) ────────────────────────────────
if(NOT EMSCRIPTEN AND CADML_BUILD_TESTS)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)  # match MSVC /MD
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.15.2 GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(googletest)
endif()
