// Drag handle for the showcase card. Reports its position as a 0–100
// percent of the parent's width. Drives a clip-path on the editor
// layer in the parent (Showcase) so dragging right exposes more of
// the 3D render behind it.

'use client';

import { useEffect, useRef, type KeyboardEvent } from 'react';

interface RevealSliderProps {
  percent:   number;
  onPercent: (p: number) => void;
}

const clamp = (n: number) => Math.max(0, Math.min(100, n));

export function RevealSlider({ percent, onPercent }: RevealSliderProps) {
  const handleRef = useRef<HTMLDivElement>(null);

  // Keyboard control matches the ARIA slider convention:
  // arrow keys nudge, Page keys jump, Home/End snap to extremes.
  const onKeyDown = (e: KeyboardEvent<HTMLDivElement>) => {
    let next: number | null = null;
    switch (e.key) {
      case 'ArrowLeft':  case 'ArrowDown':  next = clamp(percent -  2); break;
      case 'ArrowRight': case 'ArrowUp':    next = clamp(percent +  2); break;
      case 'PageDown':                      next = clamp(percent - 10); break;
      case 'PageUp':                        next = clamp(percent + 10); break;
      case 'Home':                          next = 0;                   break;
      case 'End':                           next = 100;                 break;
    }
    if (next !== null) {
      e.preventDefault();
      onPercent(next);
    }
  };

  useEffect(() => {
    const handle = handleRef.current;
    if (!handle) return;
    const parent = handle.parentElement;
    if (!parent) return;

    let dragging = false;

    const setFromPointer = (clientX: number) => {
      const rect = parent.getBoundingClientRect();
      const p = ((clientX - rect.left) / rect.width) * 100;
      onPercent(Math.max(0, Math.min(100, p)));
    };

    const onDown = (e: PointerEvent) => {
      dragging = true;
      handle.setPointerCapture(e.pointerId);
      setFromPointer(e.clientX);
      e.preventDefault();
    };
    const onMove = (e: PointerEvent) => {
      if (!dragging) return;
      setFromPointer(e.clientX);
    };
    const onUp = (e: PointerEvent) => {
      dragging = false;
      try { handle.releasePointerCapture(e.pointerId); } catch {}
    };

    handle.addEventListener('pointerdown',   onDown);
    handle.addEventListener('pointermove',   onMove);
    handle.addEventListener('pointerup',     onUp);
    handle.addEventListener('pointercancel', onUp);
    return () => {
      handle.removeEventListener('pointerdown',   onDown);
      handle.removeEventListener('pointermove',   onMove);
      handle.removeEventListener('pointerup',     onUp);
      handle.removeEventListener('pointercancel', onUp);
    };
  }, [onPercent]);

  return (
    <div
      ref={handleRef}
      className="absolute inset-y-0 -ml-3 w-6 cursor-ew-resize z-20 focus:outline-none focus-visible:ring-2 focus-visible:ring-zinc-400 rounded"
      style={{ left: `${percent}%`, touchAction: 'none' }}
      role="slider"
      tabIndex={0}
      aria-valuenow={Math.round(percent)}
      aria-valuemin={0}
      aria-valuemax={100}
      aria-label="Reveal code vs render"
      onKeyDown={onKeyDown}
    >
      <div className="absolute inset-y-0 left-1/2 -translate-x-1/2 w-px bg-zinc-900/25" />
      <div className="absolute top-1/2 left-1/2 -translate-x-1/2 -translate-y-1/2 w-8 h-8 rounded-full bg-white border border-zinc-300 shadow-sm flex items-center justify-center text-zinc-600">
        <svg width="14" height="14" viewBox="0 0 14 14" fill="none" aria-hidden>
          <path
            d="M5 4 L2 7 L5 10 M9 4 L12 7 L9 10"
            stroke="currentColor"
            strokeWidth="1.5"
            strokeLinecap="round"
            strokeLinejoin="round"
          />
        </svg>
      </div>
    </div>
  );
}
