// Minimal binary STL parser. Returns flat positions + normals (Float32
// for direct upload into a Three.js BufferGeometry).
//
// Format (Wikipedia):
//   80-byte ASCII header (ignored)
//   uint32  triangle count
//   per triangle (50 bytes):
//     float32[3] normal
//     float32[3] vertex A
//     float32[3] vertex B
//     float32[3] vertex C
//     uint16     attribute byte count (ignored)

export interface ParsedSTL {
  positions: Float32Array;
  normals:   Float32Array;
  triangles: number;
}

export function parseBinarySTL(bytes: Uint8Array): ParsedSTL {
  if (bytes.length < 84) {
    throw new Error(`STL too short (${bytes.length} bytes)`);
  }
  const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const tri = dv.getUint32(80, true);
  const expected = 84 + tri * 50;
  if (bytes.length !== expected) {
    throw new Error(`STL framing mismatch: header says ${tri} triangles (${expected} bytes), got ${bytes.length} bytes`);
  }

  const positions = new Float32Array(tri * 9);
  const normals   = new Float32Array(tri * 9);

  let o = 84;
  for (let i = 0; i < tri; i++) {
    const nx = dv.getFloat32(o,     true);
    const ny = dv.getFloat32(o + 4, true);
    const nz = dv.getFloat32(o + 8, true);
    o += 12;
    for (let v = 0; v < 3; v++) {
      const pi = (i * 3 + v) * 3;
      positions[pi]     = dv.getFloat32(o,     true);
      positions[pi + 1] = dv.getFloat32(o + 4, true);
      positions[pi + 2] = dv.getFloat32(o + 8, true);
      normals[pi]       = nx;
      normals[pi + 1]   = ny;
      normals[pi + 2]   = nz;
      o += 12;
    }
    o += 2;
  }

  return { positions, normals, triangles: tri };
}
