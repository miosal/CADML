# CADML → WebAssembly

A WebAssembly build of CADML's v0.1 flat pipeline (parse → bundle →
evaluate → STL / 3MF / glTF), exposed to JavaScript via embind. Lets a
browser-side caller compile `.cadml` and export geometry with **no
filesystem access** — multi-file projects come in through the
in-memory provider (`compile_in_memory`).

This is the **`wasm` build target of the one CADML project** — the same
root `CMakeLists.txt` builds either the native toolchain (`--preset
default`) or this module (`--preset wasm`), selected by the toolchain.
There is no separate CMake project here; this directory just holds the
embind wrapper (`cadml_wasm.cpp`) and the node test. The native build
never compiles anything under `if(EMSCRIPTEN)`, so it is unaffected.

## Prerequisites

- **emsdk** (Emscripten). Verified with 5.0.7.
- **ninja** (emsdk can install it: `emsdk install ninja-git-release-64bit`).

All dependencies are fetched (FetchContent) — no prior native
configure is required. The first configure clones + builds everything
(Manifold, Clipper2, pugixml, miniz, Lua, …) and takes ~10 min.

## Build

Activate the emsdk environment first so `EMSDK` is set and `emcc` / `ninja`
are on `PATH`, then use the `wasm` preset:

```bash
# 1. Activate emsdk (sets EMSDK + PATH). Do this once per shell:
#      Windows:   C:\path\to\emsdk\emsdk_env.bat
#      Linux/Mac: source /path/to/emsdk/emsdk_env.sh

# 2. Configure (FetchContent clones the deps on first run):
cmake --preset wasm

# 3. Build — plain `cmake --build`, NOT emcmake (the Emscripten toolchain
#    is already baked into the build dir by the preset):
cmake --build build/wasm
```

Output: `build/wasm/cadml.js` (~40 KB glue) + `build/wasm/cadml.wasm`
(~905 KB, ~370 KB gzipped — what a browser actually downloads),
size-optimized with `-Oz` + `-sFILESYSTEM=0`. (The native build lives in
a sibling per-OS tree — `build/Windows`, `build/Linux`, `build/Darwin` —
so the two never share a directory.)

> The preset locates the Emscripten toolchain via the `EMSDK` environment
> variable and uses the Ninja generator (so `ninja` must be on `PATH` —
> `emsdk_env` adds the bundled copy). If your emsdk doesn't put ninja on
> `PATH`, pass it: `cmake --preset wasm -DCMAKE_MAKE_PROGRAM=/path/to/ninja`.

## Test

```bash
cd wasm && node test_cadml_wasm.cjs   # expect ALL PASS
```

Exercises the full pipeline: single-file compile → `.fcadml`, STL export
(triangle-count + framing check), 3MF export (ZIP-magic check), the
multi-file in-memory project path, and clean error reporting.

## JS API (embind)

```js
const createCadml = require('../build/wasm/cadml.js');
const M = await createCadml();

// Single file:
const { ok, fcadml, errors, warnings } = M.compileSource(srcString);
const stl = M.exportStlFromSource(srcString);   // Uint8Array | null
const mf  = M.export3mfFromSource(srcString);   // Uint8Array | null

// Multi-file project (imports resolve by path key, no filesystem):
const files = [ { path: 'lib.cadml', contents: '...' },
                { path: 'main.cadml', contents: 'import "lib.cadml" ...' } ];
const r    = M.compileProject(files, 'main.cadml');
const stl2 = M.exportStlFromProject(files, 'main.cadml');
```

## Notes

- **Dependency pins.** FetchContent resolves exact refs: Manifold v3.3.2,
  pugixml v1.15, Clipper2 1.5.4, miniz 3.0.2, Lua 5.4.7, and sol2 /
  rapidjson / picosha2 pinned to specific commits (the released sol2
  v3.3.0 and rapidjson 1.1.0 tags predate modern-clang fixes). These are
  shared with the native build via `cmake/CadmlDependencies.cmake`.
- **Serial Manifold** (`MANIFOLD_PAR=OFF`) — single-threaded CSG. Browser
  threads (pthreads + SharedArrayBuffer + COOP/COEP headers) are a later
  optimization, not needed for correctness. The native build runs Manifold
  parallel (TBB); that is the one toolchain-gated difference.
- **Exception model:** `-fwasm-exceptions` (modern; needs Chrome 95+ /
  Firefox 100+ / Safari 15.2+ / Node 17+). Manifold's own CMake selects
  the same, so the ABI is consistent.
- **Generated `lua.hpp`.** The Lua C tarball ships no `lua.hpp`; the
  dependencies module generates a clean one so sol2 binds to the fetched
  Lua headers.
- **CI:** `.github/workflows/wasm.yml` builds + runs the node test on a
  Linux runner (emsdk 5.0.7).
```
