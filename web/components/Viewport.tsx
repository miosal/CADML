// Three.js viewport. Mounts a WebGLRenderer, holds one Mesh whose
// geometry is swapped when `mesh` changes, and runs a simple orbit
// camera driven by pointer events (mouse / touch / pen unified). No
// external controls library — the math is short enough to inline.

'use client';

import { useEffect, useRef } from 'react';
import * as THREE from 'three';
import type { ParsedSTL } from '@/lib/stl';

interface ViewportProps {
  mesh:    ParsedSTL | null;
  color:   string;
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
  renderer:     THREE.WebGLRenderer;
  scene:        THREE.Scene;
  camera:       THREE.PerspectiveCamera;
  material:     THREE.MeshStandardMaterial;
  edgeMaterial: THREE.LineBasicMaterial;
  object:       THREE.Mesh | null;
  edges:        THREE.LineSegments | null;
  target:       THREE.Vector3;
  radius:       number;
  theta:        number;
  phi:          number;
  xShift:       number;
  autoRotate:   boolean;
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

export function Viewport({ mesh, color, xShift = 0, cameraPhi }: ViewportProps) {
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
    // Push the shaded mesh back by one depth unit so the edge overlay
    // renders in front without z-fighting.
    const material = new THREE.MeshStandardMaterial({
      color: 0x9090a0,
      metalness: 0.15,
      roughness: 0.6,
      polygonOffset:       true,
      polygonOffsetFactor: 1,
      polygonOffsetUnits:  1,
    });
    const edgeMaterial = new THREE.LineBasicMaterial({
      color:       0x202028,
      transparent: true,
      opacity:     0.6,
    });

    const state: ViewportState = {
      renderer, scene, camera, material, edgeMaterial,
      object: null,
      edges:  null,
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
      if (state.object) {
        scene.remove(state.object);
        state.object.geometry.dispose();
      }
      if (state.edges) {
        scene.remove(state.edges);
        (state.edges.geometry as THREE.BufferGeometry).dispose();
      }
      material.dispose();
      edgeMaterial.dispose();
      renderer.dispose();
      el.remove();
      if (stateRef.current === state) stateRef.current = null;
    };
  }, []);

  // Mesh swap + camera fit. Each new example resets the idle spin so
  // a freshly-loaded model rotates until the user clicks it.
  useEffect(() => {
    const s = stateRef.current;
    if (!s) return;
    if (s.object) {
      s.scene.remove(s.object);
      s.object.geometry.dispose();
      s.object = null;
    }
    if (s.edges) {
      s.scene.remove(s.edges);
      (s.edges.geometry as THREE.BufferGeometry).dispose();
      s.edges = null;
    }
    if (!mesh) return;

    const g = new THREE.BufferGeometry();
    g.setAttribute('position', new THREE.BufferAttribute(mesh.positions, 3));
    g.setAttribute('normal',   new THREE.BufferAttribute(mesh.normals,   3));
    g.computeBoundingSphere();
    s.object = new THREE.Mesh(g, s.material);
    s.scene.add(s.object);

    const edgesGeom = new THREE.EdgesGeometry(g, EDGE_THRESHOLD_DEG);
    s.edges = new THREE.LineSegments(edgesGeom, s.edgeMaterial);
    s.scene.add(s.edges);

    if (g.boundingSphere) {
      s.target.copy(g.boundingSphere.center);
      s.radius =
        g.boundingSphere.radius /
        Math.sin((s.camera.fov * Math.PI) / 360) * 1.35;
    }
    s.theta = DEFAULT_THETA;
    s.phi   = cameraPhi ?? DEFAULT_PHI;
    s.autoRotate = true;
  }, [mesh, cameraPhi]);

  // Colour swap. Edge colour follows the body at 25% brightness so
  // edges stay visible without competing with the body fill.
  useEffect(() => {
    const s = stateRef.current;
    if (!s) return;
    s.material.color.set(color);
    s.edgeMaterial.color.copy(s.material.color).multiplyScalar(0.25);
  }, [color]);

  // xShift live-updates without rebuilding the renderer.
  useEffect(() => {
    if (stateRef.current) stateRef.current.xShift = xShift;
  }, [xShift]);

  return <div ref={containerRef} className="w-full h-full" />;
}
