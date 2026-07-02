import type { NextConfig } from "next";

// The displayed software version comes from the generated
// lib/cadml-version.ts (stamped by scripts/wasm-refresh.mjs from
// project(VERSION …) in the repo-root CMakeLists.txt), so the config
// needs no filesystem access and web/ builds standalone.
const nextConfig: NextConfig = {
  /* config options here */
};

export default nextConfig;
