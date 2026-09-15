// Three.js viewport. Mounts a WebGLRenderer, holds one Mesh per CADML
// part (swapped when `parts` changes), and runs a simple orbit camera
// driven by pointer events (mouse / touch / pen unified). No external
// controls library — the math is short enough to inline.
//
// Each part gets its own material so `<part color>` shows per part,
// and a part with a spec-0.3 `texture` is shaded by triplanar
// projection: CADML meshes carry no UVs, so the image is sampled along
// the three world axes and blended by the surface normal.

'use client';

import { useEffect, useRef } from 'react';
import * as THREE from 'three';
import type { ParsedSTL } from '@/lib/stl';

export interface ViewTexture {
  // Decoded image (the caller awaits createImageBitmap so the swap
  // here stays synchronous and never renders a half-loaded frame).
  image: ImageBitmap;
  // World-space size of one tile, document units.
  scale: number;
}

export interface ViewPart {
  name:    string;
  mesh:    ParsedSTL;
  // '#rrggbb'; the caller substitutes the default when a part has none.
  color:   string;
  texture: ViewTexture | null;
}

interface ViewportProps {
  parts:   ViewPart[] | null;
  // 0..0.5: mesh appears xShift * canvasWidth pixels right of canvas
  // centre. Implemented by shifting both camera position and lookAt
  // target laterally in the camera's horizontal plane, so the mesh
  // size in the rendered image is unchanged.
  xShift?:   number;
  // Camera elevation in radians from +Z, applied on each mesh swap.
  // Default falls back to DEFAULT_PHI. Set per-mesh when the default
  // view occludes a key feature.
  cameraPhi?: number;
}

interface ViewportState {
  renderer:   THREE.WebGLRenderer;
  scene:      THREE.Scene;
  camera:     THREE.PerspectiveCamera;
  // Everything belonging to the current parts: meshes, edge overlays,
  // and their materials / textures. Disposed wholesale on swap.
  group:      THREE.Group | null;
  target:     THREE.Vector3;
  radius:     number;
  theta:      number;
  phi:        number;
  xShift:     number;
  autoRotate: boolean;
}

// Radians per animation frame for the idle spin (~17°/s @ 60fps).
const AUTO_ROTATE_SPEED = 0.005;

// Default camera elevation. Lower phi = camera higher up looking
// further down, which makes Z-axis rotation more obvious because the
// top face of the part sweeps across the view.
const DEFAULT_PHI   = 0.65;
const DEFAULT_THETA = 0.9;

// EdgesGeometry feature-edge threshold (degrees). Below ~25° the
// surface is treated as continuous, above it as a hard edge.
const EDGE_THRESHOLD_DEG = 25;

// Body colour for a part that declares none.
export const DEFAULT_PART_COLOR = '#9090a0';

// Shared surface look. Push the shaded mesh back by one depth unit so
// the edge overlay renders in front without z-fighting.
const SURFACE: THREE.MeshStandardMaterialParameters = {
  metalness:           0.15,
  roughness:           0.6,
  polygonOffset:       true,
  polygonOffsetFactor: 1,
  polygonOffsetUnits:  1,
};

// Triplanar shading grafted onto MeshStandardMaterial: the texture
// replaces the base colour (a textured part's `color` is not applied),
// and every other lighting term is three.js's own. Positions are in
// mesh space, which is document space — the meshes carry no transform.
const TRIPLANAR_VERT_DECL = /* glsl */ `
#include <common>
varying vec3 vTriPos;
varying vec3 vTriNrm;`;
const TRIPLANAR_VERT_BODY = /* glsl */ `
#include <begin_vertex>
vTriPos = position;
vTriNrm = normal;`;
const TRIPLANAR_FRAG_DECL = /* glsl */ `
#include <common>
uniform sampler2D uTriTex;
uniform float     uTriScale;
varying vec3 vTriPos;
varying vec3 vTriNrm;`;
const TRIPLANAR_FRAG_BODY = /* glsl */ `
#include <map_fragment>
{
  vec3 w = abs(normalize(vTriNrm));
  w = w * w * w * w;
  w /= (w.x + w.y + w.z);
  vec3 p = vTriPos / uTriScale;
  vec4 tx = texture2D(uTriTex, p.yz);
  vec4 ty = texture2D(uTriTex, p.xz);
  vec4 tz = texture2D(uTriTex, p.xy);
  diffuseColor = tx * w.x + ty * w.y + tz * w.z;
}`;

function makeTexture(t: ViewTexture): THREE.Texture {
  const tex = new THREE.Texture(t.image);
  tex.wrapS = tex.wrapT = THREE.RepeatWrapping;
  tex.colorSpace = THREE.SRGBColorSpace;
  tex.flipY = false;
  tex.needsUpdate = true;
  return tex;
}

function makeMaterial(part: ViewPart): THREE.MeshStandardMaterial {
  if (!part.texture) {
    return new THREE.MeshStandardMaterial({ ...SURFACE, color: part.color });
  }
  const tex   = makeTexture(part.texture);
  const scale = part.texture.scale;
  const m = new THREE.MeshStandardMaterial({ ...SURFACE, color: 0xffffff });
  m.onBeforeCompile = (shader) => {
    shader.uniforms.uTriTex   = { value: tex };
    shader.uniforms.uTriScale = { value: scale };
    shader.vertexShader = shader.vertexShader
      .replace('#include <common>',       TRIPLANAR_VERT_DECL)
      .replace('#include <begin_vertex>', TRIPLANAR_VERT_BODY);
    shader.fragmentShader = shader.fragmentShader
      .replace('#include <common>',       TRIPLANAR_FRAG_DECL)
      .replace('#include <map_fragment>', TRIPLANAR_FRAG_BODY);
  };
  // Distinguish the patched program from the stock one in three's cache.
  m.customProgramCacheKey = () => 'cadml-triplanar';
  // Keep the texture reachable for disposal alongside the material.
  m.userData.texture = tex;
  return m;
}

function disposeGroup(group: THREE.Group) {
  group.traverse((obj) => {
    if (obj instanceof THREE.Mesh || obj instanceof THREE.LineSegments) {
      (obj.geometry as THREE.BufferGeometry).dispose();
      const mat = obj.material as THREE.Material;
      const tex = mat.userData?.texture as THREE.Texture | undefined;
      if (tex) tex.dispose();
      mat.dispose();
    }
  });
}

export function Viewport({ parts, xShift = 0, cameraPhi }: ViewportProps) {
  const containerRef = useRef<HTMLDivElement>(null);
  const stateRef     = useRef<ViewportState | null>(null);

  // Mount: create renderer, scene, lights, camera, input handlers.
  // A closure-bound `cancelled` flag aborts any stale-scheduled rAF
  // callbacks left over from a StrictMode double-mount cycle, so the
  // first mount's render loop can never tick after its cleanup ran.
  useEffect(() => {
    const container = containerRef.current;
    if (!container) return;

    let cancelled = false;

    const renderer = new THREE.WebGLRenderer({ antialias: true, alpha: true });
    renderer.setPixelRatio(window.devicePixelRatio);
    renderer.setClearColor(0x000000, 0);
    renderer.outputColorSpace = THREE.SRGBColorSpace;

    // Anchor the canvas's CSS size to the container; drawing-buffer
    // size is set independently via setSize(..., false) on each
    // ResizeObserver tick so high-DPI displays still get crisp output.
    const canvasEl = renderer.domElement;
    canvasEl.style.display    = 'block';
    canvasEl.style.width      = '100%';
    canvasEl.style.height     = '100%';
    canvasEl.style.touchAction = 'none';
    container.appendChild(canvasEl);

    const w0 = container.clientWidth  || 1;
    const h0 = container.clientHeight || 1;
    renderer.setSize(w0, h0, false);

    const scene = new THREE.Scene();
    scene.add(new THREE.HemisphereLight(0xffffff, 0xa0a0b0, 0.4));
    const key = new THREE.DirectionalLight(0xffffff, 1.7);
    key.position.set(3, 4, 5);
    scene.add(key);
    // Cool back-rim picks up the silhouette away from the key, so the
    // body never goes fully flat-shaded on the dark side.
    const rim = new THREE.DirectionalLight(0xa5b3cf, 0.7);
    rim.position.set(-4, -2, 1);
    scene.add(rim);

    const camera = new THREE.PerspectiveCamera(38, w0 / h0, 0.1, 10000);

    const state: ViewportState = {
      renderer, scene, camera,
      group:  null,
      target: new THREE.Vector3(),
      radius: 80, theta: DEFAULT_THETA, phi: DEFAULT_PHI,
      xShift,
      autoRotate: true,
    };

    const updateCamera = () => {
      if (state.autoRotate) state.theta += AUTO_ROTATE_SPEED;

      const { radius, theta, phi, target, xShift: sx } = state;
      const sinP = Math.sin(phi), cosP = Math.cos(phi);
      const sinT = Math.sin(theta), cosT = Math.cos(theta);

      let posX = target.x + radius * sinP * cosT;
      let posY = target.y + radius * sinP * sinT;
      const posZ = target.z + radius * cosP;
      let lookX = target.x, lookY = target.y;
      const lookZ = target.z;

      if (sx !== 0) {
        // With camera.up = +Z, the screen-right vector projected into
        // the world XY plane is (-sin(theta), cos(theta), 0). Shifting
        // camera + target leftward along that vector makes the mesh
        // appear right-of-centre in the rendered image without
        // changing its size.
        const halfFovTan = Math.tan((camera.fov * Math.PI) / 360);
        const shift = 2 * sx * radius * halfFovTan * camera.aspect;
        const rightX = -sinT, rightY = cosT;
        posX  -= rightX * shift;
        posY  -= rightY * shift;
        lookX -= rightX * shift;
        lookY -= rightY * shift;
      }

      camera.up.set(0, 0, 1);
      camera.position.set(posX, posY, posZ);
      camera.lookAt(lookX, lookY, lookZ);
    };

    const render = () => {
      if (cancelled) return;
      updateCamera();
      renderer.render(scene, camera);
      requestAnimationFrame(render);
    };
    requestAnimationFrame(render);

    // Pointer-based orbit + wheel zoom.
    const el = renderer.domElement;
    let dragging = false;
    let lastX = 0, lastY = 0;
    const onDown = (e: PointerEvent) => {
      state.autoRotate = false;
      dragging = true;
      lastX = e.clientX; lastY = e.clientY;
      el.setPointerCapture(e.pointerId);
    };
    const onMove = (e: PointerEvent) => {
      if (!dragging) return;
      const dx = e.clientX - lastX; lastX = e.clientX;
      const dy = e.clientY - lastY; lastY = e.clientY;
      state.theta -= dx * 0.008;
      state.phi   -= dy * 0.008;
      const eps = 0.05;
      state.phi = Math.max(eps, Math.min(Math.PI - eps, state.phi));
    };
    const onUp = (e: PointerEvent) => {
      dragging = false;
      try { el.releasePointerCapture(e.pointerId); } catch {}
    };
    const onWheel = (e: WheelEvent) => {
      e.preventDefault();
      state.autoRotate = false;
      state.radius = Math.max(0.5,
        Math.min(20000, state.radius * Math.exp(e.deltaY * 0.001)));
    };
    el.addEventListener('pointerdown',   onDown);
    el.addEventListener('pointermove',   onMove);
    el.addEventListener('pointerup',     onUp);
    el.addEventListener('pointercancel', onUp);
    el.addEventListener('wheel',         onWheel, { passive: false });

    // Size with parent.
    const resizeObs = new ResizeObserver(() => {
      const W = container.clientWidth  || 1;
      const H = container.clientHeight || 1;
      renderer.setSize(W, H, false);
      camera.aspect = W / H;
      camera.updateProjectionMatrix();
    });
    resizeObs.observe(container);

    stateRef.current = state;
    return () => {
      cancelled = true;
      resizeObs.disconnect();
      el.removeEventListener('pointerdown',   onDown);
      el.removeEventListener('pointermove',   onMove);
      el.removeEventListener('pointerup',     onUp);
      el.removeEventListener('pointercancel', onUp);
      el.removeEventListener('wheel',         onWheel);
      if (state.group) {
        scene.remove(state.group);
        disposeGroup(state.group);
      }
      renderer.dispose();
      el.remove();
      if (stateRef.current === state) stateRef.current = null;
    };
  }, []);

  // Parts swap + camera fit. Each new example resets the idle spin so
  // a freshly-loaded model rotates until the user clicks it.
  useEffect(() => {
    const s = stateRef.current;
    if (!s) return;
    if (s.group) {
      s.scene.remove(s.group);
      disposeGroup(s.group);
      s.group = null;
    }
    if (!parts || parts.length === 0) return;

    const group = new THREE.Group();
    const bounds = new THREE.Box3();
    const geometries: THREE.BufferGeometry[] = [];
    for (const part of parts) {
      const g = new THREE.BufferGeometry();
      g.setAttribute('position', new THREE.BufferAttribute(part.mesh.positions, 3));
      g.setAttribute('normal',   new THREE.BufferAttribute(part.mesh.normals,   3));
      g.computeBoundingBox();
      if (g.boundingBox) bounds.union(g.boundingBox);
      geometries.push(g);

      const material = makeMaterial(part);
      group.add(new THREE.Mesh(g, material));

      // Edge colour follows the body at 25% brightness so edges stay
      // visible without competing with the body fill.
      const edgeMaterial = new THREE.LineBasicMaterial({
        color:       new THREE.Color(part.color).multiplyScalar(0.25),
        transparent: true,
        opacity:     0.6,
      });
      group.add(new THREE.LineSegments(
        new THREE.EdgesGeometry(g, EDGE_THRESHOLD_DEG), edgeMaterial));
    }
    s.group = group;
    s.scene.add(group);

    // Fit: sphere about the union box's centre, radius from the
    // farthest vertex (tighter than the box's half-diagonal).
    if (!bounds.isEmpty()) {
      const center = bounds.getCenter(new THREE.Vector3());
      let r2 = 0;
      const v = new THREE.Vector3();
      for (const g of geometries) {
        const pos = g.getAttribute('position');
        for (let i = 0; i < pos.count; i++) {
          v.fromBufferAttribute(pos, i);
          r2 = Math.max(r2, v.distanceToSquared(center));
        }
      }
      s.target.copy(center);
      s.radius =
        Math.sqrt(r2) /
        Math.sin((s.camera.fov * Math.PI) / 360) * 1.35;
    }
    s.theta = DEFAULT_THETA;
    s.phi   = cameraPhi ?? DEFAULT_PHI;
    s.autoRotate = true;
  }, [parts, cameraPhi]);

  // xShift live-updates without rebuilding the renderer.
  useEffect(() => {
    if (stateRef.current) stateRef.current.xShift = xShift;
  }, [xShift]);

  return <div ref={containerRef} className="w-full h-full" />;
}
