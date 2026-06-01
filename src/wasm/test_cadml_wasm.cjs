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

  // 5. Error path surfaces cleanly (no crash). Missing `version` is a
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
