import type { NextConfig } from "next";
import { readFileSync } from "node:fs";
import { join } from "node:path";

// Single source of truth for the software version is project(VERSION …)
// in the parent repo's CMakeLists.txt (the same place wasm-refresh reads
// when stamping the vendored artifacts). Parsed once at build time and
// inlined, so the footer can never drift from a release.
const CADML_VERSION = (() => {
  const cml = readFileSync(join(__dirname, "..", "CMakeLists.txt"), "utf8");
  const m = cml.match(/project\(\s*cadml[\s\S]*?VERSION\s+(\d+\.\d+\.\d+)/);
  if (!m) throw new Error("next.config.ts: could not parse VERSION from ../CMakeLists.txt");
  return m[1];
})();

const nextConfig: NextConfig = {
  env: {
    NEXT_PUBLIC_CADML_VERSION: CADML_VERSION,
  },
};

export default nextConfig;
