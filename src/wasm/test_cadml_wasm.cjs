// End-to-end test of the CADML WebAssembly module under node.
//   node test_cadml_wasm.cjs
// Exercises the full flat pipeline (parse -> bundle -> evaluate -> export)
// for single-file source AND the in-memory multi-file project path.

const createCadml = require('../../build/wasm/cadml.js');

// The README plate: 60x40x6 mm plate with two 6 mm holes (single file).
const PLATE = `version 0.1
units mm
param plate-w   = 60
param plate-h   = 40
param plate-t   = 6
param hole-r    = 3
param overshoot = 1
<part name="plate">
  <difference>
    <extrude height="{plate-t}">
      <rect x="{-plate-w/2}" y="{-plate-h/2}" width="{plate-w}" height="{plate-h}" rx="3"/>
    </extrude>
    <group transform="translate( 15, 0, {-overshoot})">
      <extrude height="{plate-t + 2*overshoot}"><circle r="{hole-r}"/></extrude>
    </group>
    <group transform="translate(-15, 0, {-overshoot})">
      <extrude height="{plate-t + 2*overshoot}"><circle r="{hole-r}"/></extrude>
    </group>
  </difference>
</part>`;

// A 32x32 RGB checker PNG (the same image as examples/showcase-texture).
const CHECKER_PNG = Uint8Array.from(Buffer.from(
  'iVBORw0KGgoAAAANSUhEUgAAACAAAAAgCAIAAAD8GO2jAAAAOUlEQVR42mM4uaIGK6qKssCKSFXPMGrBqAVDwAJqGYRL/agFoxYMBQtGi4pRC0YtGK0PRi0YtQCIAGby8kywFxDeAAAAAElFTkSuQmCC', 'base64'));

let failures = 0;
function check(name, cond, detail) {
  console.log(`${cond ? 'PASS' : 'FAIL'}  ${name}${detail ? '  — ' + detail : ''}`);
  if (!cond) failures++;
}

createCadml().then((M) => {
  // 1. Single-file compile → .fcadml
  const c = M.compileSource(PLATE);
  check('compileSource ok', c.ok, c.errors || '');
  check('fcadml has <part>', c.ok && c.fcadml.includes('<part name="plate"'));
  check('fcadml has <sources>', c.ok && c.fcadml.includes('<sources>'));

  // 2. STL export — read the binary triangle count and verify framing.
  const stl = M.exportStlFromSource(PLATE);
  const haveStl = stl && stl.length > 84;
  check('exportStl produced bytes', haveStl, haveStl ? stl.length + ' bytes' : 'null');
  if (haveStl) {
    const dv = new DataView(stl.buffer, stl.byteOffset, stl.byteLength);
    const ntri = dv.getUint32(80, true);
    check('STL framing 84 + 50*ntri', stl.length === 84 + 50 * ntri,
          ntri + ' triangles');
  }

  // 3. 3MF export — confirm it's a real ZIP (PK\x03\x04 magic).
  const mf = M.export3mfFromSource(PLATE);
  const haveMf = mf && mf.length > 4;
  check('export3mf produced bytes', haveMf, haveMf ? mf.length + ' bytes' : 'null');
  if (haveMf) {
    check('3MF ZIP magic', mf[0] === 0x50 && mf[1] === 0x4B &&
                            mf[2] === 0x03 && mf[3] === 0x04);
  }

  // 4. Multi-file project via the in-memory provider (imports by key).
  const proj = M.compileProject(
    [ { path: 'lib.cadml',
        contents: 'version 0.1\n<part><circle r="3"/></part>' },
      { path: 'main.cadml',
        contents: 'version 0.1\nimport "lib.cadml"\n<part><lib/></part>' } ],
    'main.cadml');
  check('compileProject ok', proj.ok, proj.errors || '');
  check('project inlined import as <def name="lib">',
        proj.ok && proj.fcadml.includes('<def name="lib"'));

  // 5. Scene API: one part per top-level <part>, each with its own STL,
  //    colour and (spec 0.3) texture. The PNG comes in as a Uint8Array
  //    file next to the source, exactly as a browser host supplies it.
  const scene = M.sceneFromProject(
    [ { path: 'main.cadml',
        contents: 'version 0.3\n' +
          '<part name="tile" texture="tex.png" texture-scale="7.5">' +
          '<extrude height="2"><rect width="10" height="10"/></extrude></part>\n' +
          '<part name="plain" color="#336699">' +
          '<extrude height="1"><circle r="3"/></extrude></part>' },
      { path: 'tex.png', contents: CHECKER_PNG } ],
    'main.cadml');
  check('sceneFromProject ok', scene.ok, scene.errors || '');
  check('scene has two parts', scene.parts.length === 2,
        scene.parts.length + ' parts');
  if (scene.parts.length === 2) {
    const [tile, plain] = scene.parts;
    check('part names in document order',
          tile.name === 'tile' && plain.name === 'plain');
    check('untextured part: color, texture null',
          plain.color === '#336699' && plain.texture === null);
    const t = tile.texture;
    check('textured part: mime + scale resolved',
          !!t && t.mime === 'image/png' && t.scale === 7.5,
          t ? `${t.mime} scale ${t.scale}` : 'null');
    check('texture bytes round-trip the PNG',
          !!t && t.bytes.length === CHECKER_PNG.length &&
          t.bytes.every((b, i) => b === CHECKER_PNG[i]));
    // A 10x10x2 box is 12 triangles; a disc has many more.
    const tri = (stl) => new DataView(stl.buffer, stl.byteOffset, stl.byteLength).getUint32(80, true);
    check('per-part STL framing', tile.stl.length === 84 + 50 * tri(tile.stl) &&
          plain.stl.length === 84 + 50 * tri(plain.stl));
    check('per-part STL is that part alone', tri(tile.stl) === 12,
          tri(tile.stl) + ' triangles');
  }
  // A missing asset is a compile error: ok=false, no parts, message.
  const noTex = M.sceneFromProject(
    [ { path: 'main.cadml',
        contents: 'version 0.3\n<part texture="gone.png"><extrude height="1"><circle r="1"/></extrude></part>' } ],
    'main.cadml');
  check('scene reports a missing texture file',
        !noTex.ok && noTex.parts.length === 0 && /gone\.png/.test(noTex.errors),
        (noTex.errors.split('\n')[0] || '(no message)'));
  const sceneSrc = M.sceneFromSource(PLATE);
  check('sceneFromSource ok with one part',
        sceneSrc.ok && sceneSrc.parts.length === 1 && sceneSrc.parts[0].name === 'plate' &&
        sceneSrc.parts[0].texture === null);

  // 6. Error path surfaces cleanly (no crash). Missing `version` is a
  //    real parse error (unlike an unknown element, which defers to an
  //    instance — matching native cadmlc, which also accepts that).
  const bad = M.compileSource('<part name="x"><circle r="5"/></part>');
  check('bad input reported, not crashed', !bad.ok && bad.errors.length > 0,
        (bad.errors.split('\n')[0] || '(no message)'));

  console.log(failures === 0 ? '\nALL PASS' : `\n${failures} FAILURE(S)`);
  process.exit(failures === 0 ? 0 : 1);
}).catch((e) => {
  console.error('module load/run threw:', e);
  process.exit(2);
});
