// Curated CADML examples for the homepage showcase. Source is inlined
// verbatim so the web app has no runtime dependency on the parent repo's
// `examples/` directory. Refresh these strings when the canonical
// examples change — they should track examples/<name>/<name>.cadml.

import type { InMemoryFile } from '@/lib/cadml';

export interface Example {
  id:    string;
  title: string;
  blurb: string;
  files: InMemoryFile[];
  entry: string;
  // Optional camera elevation override (radians from +Z). Default in
  // Viewport is ~0.65 (3/4-from-above). Lower = more top-down; higher
  // = more side-on. Set per-example when the default occludes a key
  // feature.
  cameraPhi?: number;
}

// First .cadml file an example is keyed to — what the editor renders.
export function entrySource(e: Example): string {
  const c = e.files.find((f) => f.path === e.entry)?.contents;
  return typeof c === 'string' ? c : '';
}

// Binary assets (texture images) are inlined as base64 so the example
// list stays one self-contained module.
function bytesFromBase64(b64: string): Uint8Array {
  const bin = atob(b64);
  const out = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
  return out;
}

const COMPRESSOR_CADML = `version 0.3
units mm

import "compressor.lua"

param bore-r        = 4
param hub-inlet-r   = 8
param exducer-r     = 26
param inducer-tip-r = 20
param height        = 25
param back-t        = 1.5
param overshoot     = 1

<def name="main-blade">
  <loft>
    <for var="t" from="0" to="1" steps="20">
      <sketch plane="xy" origin="{compressor.hub_r(t)} 0 {compressor.hub_z(t)}"
              rotation="{compressor.blade_beta(t)}"
              normal="{compressor.hub_tx(t)} 0 {compressor.hub_tz(t)}">
        <path d="{compressor.blade_section(compressor.blade_h(t), compressor.blade_t(t))}"/>
      </sketch>
    </for>
  </loft>
</def>

<def name="splitter-blade">
  <loft>
    <for var="t" from="0.35" to="1" steps="14">
      <sketch plane="xy" origin="{compressor.hub_r(t)} 0 {compressor.hub_z(t)}"
              rotation="{compressor.blade_beta(t)}"
              normal="{compressor.hub_tx(t)} 0 {compressor.hub_tz(t)}">
        <path d="{compressor.blade_section(compressor.blade_h(t), compressor.blade_t(t))}"/>
      </sketch>
    </for>
  </loft>
</def>

<part name="compressor-wheel" color="#A0A0B0">
  <difference>
    <union>
      <!-- Revolved hub from a Lua-generated meridional curve. -->
      <revolve axis="z" angle="360">
        <path d="{compressor.hub_profile()}"/>
      </revolve>

      <!-- 6 main blades around the axis. -->
      <pattern type="circular" count="6" axis="z">
        <main-blade/>
      </pattern>

      <!-- 6 splitter blades, offset 30 deg between main blades. -->
      <pattern type="circular" count="6" axis="z">
        <group transform="rotate(30, 0, 0, 1)">
          <splitter-blade/>
        </group>
      </pattern>
    </union>

    <!-- Shaft bore. -->
    <group transform="translate(0, 0, {-(back-t + overshoot)})">
      <extrude height="{height + back-t + overshoot * 2 + 4}">
        <circle r="{bore-r}"/>
      </extrude>
    </group>
  </difference>
</part>
`;

const COMPRESSOR_LUA = `-- Centrifugal turbo compressor wheel — parametric shape generator.
-- Imported by compressor.cadml via \`import "compressor.lua"\`.

local H_R0  = cadml.param("hub-inlet-r")
local H_R1  = cadml.param("exducer-r")
local H_H   = cadml.param("height")
local H_DR  = H_R1 - H_R0
local TIP_R = cadml.param("inducer-tip-r")

-- Concave hub meridional curve. z drops quickly (nearly vertical
-- inducer); r expands late (radial exducer). t: 0 = inducer top,
-- 1 = exducer bottom.
function hub_r(t) return H_R0 + H_DR * t ^ 2.4 end
function hub_z(t) return H_H * (1 - t ^ 0.65) end

-- Blade height (hub to shroud) — tapers from inducer to exducer.
function blade_h(t) return (TIP_R - H_R0) * (1 - 0.55 * t) end

-- Blade angle: +25 deg at inducer, -55 deg at exducer (backsweep).
function blade_beta(t) return 25 - 80 * t ^ 1.3 end

-- Blade thickness — thin realistic profile, peaks mid-blade.
function blade_t(t) return 0.8 + 0.6 * math.sin(math.pi * t) end

-- Negated hub meridional tangent direction. The loft uses
-- right = up x normal, so we negate the tangent to make blade span
-- (right direction) point radially outward.
function hub_tx(t)
    local dt = 0.001
    local t0 = math.max(t - dt, 0)
    local t1 = math.min(t + dt, 1)
    local dr = hub_r(t1) - hub_r(t0)
    local dz = hub_z(t1) - hub_z(t0)
    local len = math.sqrt(dr * dr + dz * dz)
    if len < 1e-9 then return 0 end
    return -dr / len
end

function hub_tz(t)
    local dt = 0.001
    local t0 = math.max(t - dt, 0)
    local t1 = math.min(t + dt, 1)
    local dr = hub_r(t1) - hub_r(t0)
    local dz = hub_z(t1) - hub_z(t0)
    local len = math.sqrt(dr * dr + dz * dz)
    if len < 1e-9 then return 1 end
    return -dz / len
end

-- Hub meridional profile for revolve (x = radius, y = z-height).
function hub_profile()
    local bore_r = cadml.param("bore-r")
    local bt     = cadml.param("back-t")
    local pts = {}
    table.insert(pts, { bore_r,    H_H + 2 })
    table.insert(pts, { H_R0 - 0.5, H_H + 2 })
    table.insert(pts, { H_R0,      H_H + 1.5 })
    table.insert(pts, { H_R0,      H_H })
    for i = 1, 50 do
        local t = i / 50
        table.insert(pts, { hub_r(t), hub_z(t) })
    end
    table.insert(pts, { H_R1,   -bt })
    table.insert(pts, { bore_r, -bt })
    return cadml.path(pts)
end

-- Blade cross-section: thin plate with rounded LE and tapered root.
function blade_section(h, t)
    local n = 30
    local pts = {}
    local le_r = t * 0.4
    for i = 0, n do
        local f = i / n
        local tap = math.min(f / 0.15, 1.0)
        local tip = 1.0 - 0.3 * math.max(f - 0.85, 0) / 0.15
        table.insert(pts, { f * h, -t / 2 * tap * tip })
    end
    for i = 1, 8 do
        local a = -math.pi / 2 + math.pi * i / 9
        table.insert(pts, { h + le_r * math.cos(a), le_r * math.sin(a) })
    end
    for i = n, 0, -1 do
        local f = i / n
        local tap = math.min(f / 0.15, 1.0)
        local tip = 1.0 - 0.3 * math.max(f - 0.85, 0) / 0.15
        table.insert(pts, { f * h, t / 2 * tap * tip })
    end
    return cadml.path(pts)
end
`;

// Caster wheel assembly — flattened from the canonical 4-file example
// (caster-wheel/{caster-wheel,fork,axle,wheel}.cadml) into one source
// for the editor pane. STL export merges all parts into a single
// triangle soup, so the rendered colour comes from the first <part>.
const CASTER_WHEEL_CADML = `version 0.3
units mm
description "Caster-wheel assembly: a fork, an axle, and a wheel emitted as three top-level <part>s. The canonical example splits these into four files; the geometry is flattened here for a single-pane view."

# Top-level sizing.
param leg-span        = 50     # fork leg outer-to-outer distance (along y)
param wheel-od        = 45
param axle-dia        = 8

# Fork plate / leg geometry.
param plate-x         = 60
param plate-y         = 60
param plate-t         = 4
param leg-x           = 14
param leg-thick       = 5
param leg-h           = 32
param plate-bot-z     = {leg-h}

# Wheel geometry.
param wheel-thickness = 16
param tread-d         = 38

<!-- Fork: U-bracket with a flat mounting plate on top and two legs
     that hang down to carry the axle. Local frame: +z is up, the
     axle line runs through z = 0, legs span z = 0..leg-h, plate sits
     on top of the legs. -->
<part name="fork" color="#788090">
  <difference>
    <union>
      <!-- Mounting plate. -->
      <group transform="translate(0, 0, {plate-bot-z})">
        <extrude height="{plate-t}">
          <rect x="{-plate-x / 2}" y="{-plate-y / 2}"
                width="{plate-x}" height="{plate-y}" rx="4"/>
        </extrude>
      </group>

      <!-- Two legs, unrolled by <for> over side in {-1, +1}. -->
      <for var="side" values="-1 1">
        <group transform="translate(0, {side * (leg-span - leg-thick) / 2}, 0)">
          <extrude height="{leg-h}">
            <rect x="{-leg-x / 2}" y="{-leg-thick / 2}"
                  width="{leg-x}" height="{leg-thick}" rx="1.5"/>
          </extrude>
        </group>
      </for>
    </union>

    <!-- Axle hole through both legs at z = 0. -->
    <group transform="rotate(90, 1, 0, 0) translate(0, 0, {-leg-span / 2 - 1})">
      <extrude height="{leg-span + 2}">
        <circle r="{(axle-dia + 0.4) / 2}"/>
      </extrude>
    </group>
  </difference>
</part>

<!-- Axle: cylinder spanning the two legs along the world y-axis. -->
<part name="axle" color="#c8c8d0">
  <group transform="translate(0, {-leg-span / 2}, 0) rotate(-90, 1, 0, 0)">
    <extrude height="{leg-span}">
      <circle r="{axle-dia / 2}"/>
    </extrude>
  </group>
</part>

<!-- Wheel: lofted between three coaxial circles so the running
     surface narrows to a crisp contact line. Hub bore through the
     rotation axis seats onto the axle. -->
<part name="wheel" color="#2a2a2a">
  <group transform="rotate(-90, 1, 0, 0)">
    <difference>
      <loft>
        <sketch plane="xy" origin="0 0 {-wheel-thickness / 2}">
          <circle r="{tread-d / 2}" segments="64"/>
        </sketch>
        <sketch plane="xy" origin="0 0 0">
          <circle r="{wheel-od / 2}" segments="64"/>
        </sketch>
        <sketch plane="xy" origin="0 0 {wheel-thickness / 2}">
          <circle r="{tread-d / 2}" segments="64"/>
        </sketch>
      </loft>
      <group transform="translate(0, 0, {-wheel-thickness / 2 - 1})">
        <extrude height="{wheel-thickness + 2}">
          <circle r="{(axle-dia + 0.2) / 2}"/>
        </extrude>
      </group>
    </difference>
  </group>
</part>
`;

const ENCLOSURE_CADML = `version 0.3
units mm
description "Tea-light holder exercising fillet, chamfer, shell, and revolve on one part. The base plate is a chamfered + filleted rectangle (bevel at the floor, rounded top rim). The candle cup is a hollow open-top cylinder built with <shell>. A collar revolved from a six-point ogee profile straddles the cup's top edge — the inner edge sits flush with the cup wall so the <union> melds into one continuous outer surface."

# Cup body.
param cup-r       = 18
param cup-h       = 22
param wall        = 2.5

# Decorative collar at the rim — revolved ogee profile.
param rim-flare  = 3       # outward extent of the collar
param rim-h      = 4       # collar height

# Base plate (literal 6 in the fillet selector below must match base-h).
param base-w      = 50
param base-d      = 50
param base-h      = 6
param base-fillet = 3
param base-bevel  = 2

<part name="tealight-holder" color="#5b7a90">
  <union>
    <!-- Base plate: bevelled along the floor, top rim rounded. -->
    <chamfer distance="{base-bevel}" select="edge:position.z=0">
      <fillet radius="{base-fillet}" select="edge:position.z=6">
        <extrude height="{base-h}">
          <rect x="{-base-w / 2}" y="{-base-d / 2}"
                width="{base-w}" height="{base-d}"/>
        </extrude>
      </fillet>
    </chamfer>

    <!-- Candle cup: hollow, open-top. <shell> requires a literal
         <extrude> as its direct child. -->
    <group transform="translate(0, 0, {base-h - 0.1})">
      <shell thickness="{wall}" open="end">
        <extrude height="{cup-h}">
          <circle r="{cup-r}"/>
        </extrude>
      </shell>
    </group>

    <!-- Decorative collar at the rim: revolved ogee. The profile
         (radial x, axial y) goes inner-bottom -> outer-bottom (flared
         out), up the outside, gently tucks inward, kicks back out at
         the lip, then closes across the top inside. The inner edge
         sits flush with the cup's inner wall so the union melds
         cleanly into one continuous outer surface. -->
    <group transform="translate(0, 0, {base-h + cup-h - rim-h * 0.4})">
      <revolve axis="z" angle="360">
        <path d="M {cup-r - wall}, 0
                 L {cup-r + rim-flare}, 0
                 L {cup-r + rim-flare}, {rim-h * 0.5}
                 L {cup-r + rim-flare * 0.5}, {rim-h * 0.8}
                 L {cup-r + rim-flare * 0.7}, {rim-h}
                 L {cup-r - wall}, {rim-h}
                 Z"/>
      </revolve>
    </group>
  </union>
</part>
`;

// ── Planter (textures) ────────────────────────────────────────────────
//
// Mirrors the authoring form of examples/showcase-texture: the source
// names image files next to it (`texture="brick.png"`), which the
// bundler inlines. The two tiles are tiny procedural PNGs (64 px
// brick, 32 px grass) held here as base64.

const PLANTER_CADML = `version 0.3
units mm
description "Raised garden bed: a brick wall ring with a timber coping and a lawn inside. The brick and grass are image textures on the parts, tiled by the renderer — the geometry is three plain bodies."

param bed-l    = 900
param bed-w    = 600
param bed-h    = 360
param wall-t   = 105
param cap-t    = 30
param cap-lip  = 20
param soil-gap = 60

<!-- Four brick courses per texture tile: 75 mm course pitch. -->
<part name="walls" texture="brick.png" texture-scale="300">
  <difference>
    <extrude height="{bed-h}">
      <rect width="{bed-l}" height="{bed-w}"/>
    </extrude>
    <group transform="translate({wall-t}, {wall-t}, -1)">
      <extrude height="{bed-h + 2}">
        <rect width="{bed-l - 2*wall-t}" height="{bed-w - 2*wall-t}"/>
      </extrude>
    </group>
  </difference>
</part>

<!-- Timber coping overhanging the wall by cap-lip on every side. -->
<part name="coping" color="#8a6a45">
  <group transform="translate({-cap-lip}, {-cap-lip}, {bed-h})">
    <difference>
      <extrude height="{cap-t}">
        <rect width="{bed-l + 2*cap-lip}" height="{bed-w + 2*cap-lip}"/>
      </extrude>
      <group transform="translate({wall-t + cap-lip}, {wall-t + cap-lip}, -1)">
        <extrude height="{cap-t + 2}">
          <rect width="{bed-l - 2*wall-t}" height="{bed-w - 2*wall-t}"/>
        </extrude>
      </group>
    </difference>
  </group>
</part>

<!-- The lawn fills the ring up to soil-gap below the rim. -->
<part name="lawn" texture="grass.png" texture-scale="120">
  <group transform="translate({wall-t}, {wall-t}, 0)">
    <extrude height="{bed-h - soil-gap}">
      <rect width="{bed-l - 2*wall-t}" height="{bed-w - 2*wall-t}"/>
    </extrude>
  </group>
</part>
`;

const BRICK_PNG = bytesFromBase64(
  'iVBORw0KGgoAAAANSUhEUgAAAEAAAABACAIAAAAlC+aJAAABV0lEQVR42u2ZMU4DMRBF/876' +
  'COEK0KRAFNQUHAQuEKSkiyCCFFsEKURRlCpcggOkpKCghJ4CwSHGojBaJVRr5FG8ynexu1P4' +
  'W9bssz3fxfP6CW1ugpY3F15vk1sRERFVrZ9JwqPBDYDP5SS58m8YJlCWpffeex8+EoZB30I5' +
  'PN3mAKoaJqOqqcJaP7lyCLcyYJEH6wyLdYr/6CcfyFmnuM5AcuUQEmJCTIgJMSHeb4gL1gM7' +
  'bsXi/DjqLH4yHAP4Wk2N6oeDyysAr9Woad/c/unYVcv9Y33IatVyuW1Mm/pN+kpuG1Ps0UNi' +
  'R6oHMJpD7L5EiAkxISbEhHi/IWY9kMn9wPv9nYl5L3LYvwbwMa+M6gcaWzS2aGzR2CLEhJgQ' +
  'E2JC3GKIWQ/s/H5gdtaNOoufjioA348PRvVD56IH4GU8bNiXxhaNLRpbNLYIMSEmxISYELcY' +
  '4h+s8CoX+OtiFgAAAABJRU5ErkJggg=='
);

const GRASS_PNG = bytesFromBase64(
  'iVBORw0KGgoAAAANSUhEUgAAACAAAAAgCAIAAAD8GO2jAAAD9klEQVR42m1WTW/bSAylbEXj' +
  '2NLAH2oRy24NuHvwYQ9G0VPv/Qf9CXvY39Jfl1tyUm4DF1ViY2QkGsOB9/DcV64cwTCoEcnh' +
  'kI+PE33996/VuhAREbm/dek4EZH90wErq3Vxf+u0AoRiaV3p9Vfv9yJibUodmHdojMcOe3bY' +
  'wzftkXKxtPRONe/31qbWplRGoPe3Lr6MCwIiuvQuItDBIkyKpaUTuEag1qYdBkVLyBSQB71Z' +
  'Ok7gMR0nMOGBYIIcIILu9fyq3oZsZOptOMe+a0JzDM3x0T2LSLWp4TSfDmBmerGInKKTiOTT' +
  'wWwxqTZ1tanhPTRHKIhIvQ3dD1/G+JCNTDYyftcghNAcaQxHfteYXuxKf4pOoTlCzZU+v8mS' +
  'fnSKTvCLf1f6ehvScdL98GU8W0we7qp6GxAUosAxH+6qdJwcXl7rbcB/sbT0jhirTZ2NjA7c' +
  '75p8OjhFJzvsRd9/fP5T2N/hI4R0nDCbfKBDyHERMgTq2GGv86Z3gsHvGld6a1NXeld6bYle' +
  'IaIoax0ROfcBQ6ABcAYvQBHxB30ANB0nftewMc/oVK661/OrYmkf3XM2MoANakgU8cmnAyQa' +
  'P2jaYe/d+2G1qWeLSdKPsDcU6m04owj4sTYFHlDh0BwPL6/og6QfZSPjSn94eYWQjQwA40oP' +
  'mKHUyA/Wi6Wtt6EDAkGX64aaf8zJBMzbal1Q1tWmGrOK12JpY00G7GFXeiceJKNJzfs9dJD3' +
  '+cfcidetzsYm8cQkO3pnCJeUqeGrj3j5kNZivYoleOR+mjKBJbCYTqnm85Zh93p+VW3q/CYL' +
  '4TBbTPKbTESAB1IYLEFZj+653oakHz3cVS3vNNS23W///J30I2MSY5L7W5f0o18/PeA4W0y0' +
  'JWC3WhfVpgZn4FVXERAHRsGAMY5p1+klAbSm2Bk8qs74yiLrTmRROy1Lv2tAD1jROeXYIKlw' +
  '6nEMtOjA75rO2cbv4RpAJgaYTb1CjkLZW8qAP0/THX4aQANMgDGA8QDWzUbGmCSEA7qX/QX2' +
  'BmOjHhA4LUA/MWy4Ic7xv2vA74sCdC6LhL5BlNoWK9H3H591Z+sU62y2Pl3qsFRtukbWNOR1' +
  '3+lFV/r90+FyBGm/9I5j+V0Ts0SoMzfnou7MNyNw8sbR+Rpbm6IJgDYGQptL5LWGmqaW/dMB' +
  'IOQ2se4jlAG+qKfRqZuRPMgCkCX102mxqbUpBL6u1gV1wHQtR4hJl5pQdKXvXs+vcMyHuwqM' +
  'D2jjnmNMQi4C02UjM1tMwIwIH90AjsJwfHTP+XSAEdkh4fC8aAtwMhPIqceRwrzrI7aY/3z5' +
  'JaVgD3jHUNOzjKj9Qz42xeDTnKivxiLyHzjLguSPzqzSAAAAAElFTkSuQmCC'
);

export const EXAMPLES: Example[] = [
  {
    id:    'compressor',
    title: 'Compressor',
    blurb: 'Revolved hub plus six main and six splitter blades, each a polyhedral loft of Lua-computed airfoil sections.',
    files: [
      { path: 'compressor.cadml', contents: COMPRESSOR_CADML },
      { path: 'compressor.lua',   contents: COMPRESSOR_LUA   },
    ],
    entry: 'compressor.cadml',
  },
  {
    id:    'caster-wheel',
    title: 'Castor Wheel',
    blurb: 'Fork, axle, and wheel emitted as three top-level <part> elements from one source. The wheel uses <loft> across three coaxial circles for a tread profile.',
    files: [
      { path: 'caster-wheel.cadml', contents: CASTER_WHEEL_CADML },
    ],
    entry: 'caster-wheel.cadml',
    // Side-on camera so the wheel beneath the plate is visible
    // (the default top-down view occludes it).
    cameraPhi: 1.15,
  },
  {
    id:    'tealight-holder',
    title: 'Tea-light holder',
    blurb: 'Chamfered base plate, filleted top rim, hollow open-top cup with a revolved ogee collar — fillet, chamfer, shell, and revolve composed on one part.',
    files: [
      { path: 'tealight-holder.cadml', contents: ENCLOSURE_CADML },
    ],
    entry: 'tealight-holder.cadml',
  },
  {
    id:    'planter',
    title: 'Planter',
    blurb: 'Brick ring, timber coping and a lawn — three plain bodies with spec-0.3 <part texture> images. No UVs are modelled: the viewer projects each tile onto the surface (triplanar), at the size the source gives with texture-scale.',
    files: [
      { path: 'planter.cadml', contents: PLANTER_CADML },
      { path: 'brick.png',     contents: BRICK_PNG     },
      { path: 'grass.png',     contents: GRASS_PNG     },
    ],
    entry: 'planter.cadml',
  },
];
