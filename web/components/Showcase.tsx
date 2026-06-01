// The hero's code-vs-render card. Two layers (Monaco editor on top,
// Three.js render below) overlaid on the same rectangle, with a
// horizontal reveal slider that wipes between them by clipping the
// editor layer's right edge. On mobile (no md: breakpoint) the slider
// is hidden and the editor is clipped to its top half instead,
// producing a code-above / render-below stack.
//
// The editor is live: changes to the source debounce-trigger a fresh
// WASM compile, the render updates if the compile succeeds, and any
// compile errors land in Monaco as red-squiggle markers driven by
// lib/cadml-diagnostics.

'use client';

import { useEffect, useRef, useState } from 'react';

import { CadmlEditor }                              from './Editor';
import { RevealSlider }                             from './RevealSlider';
import { Viewport }                                 from './Viewport';
import { EXAMPLES, entrySource, firstPartColor }   from '@/data/examples';
import { loadCadml }                                from '@/lib/cadml';
import type { CadmlModule }                         from '@/lib/cadml';
import { parseBinarySTL }                           from '@/lib/stl';
import type { ParsedSTL }                           from '@/lib/stl';
import { errorsToMarkers, MarkerSeverity }          from '@/lib/cadml-diagnostics';
import type { EditorMarker }                        from '@/lib/cadml-diagnostics';

const COMPILE_DEBOUNCE_MS = 300;
// Don't show the loading overlay for compiles that finish quickly —
// only after this much wall time does the suspense layer fade in.
const OVERLAY_DELAY_MS    = 80;

export function Showcase() {
  const [exampleId, setExampleId] = useState(EXAMPLES[0].id);
  const example = EXAMPLES.find((e) => e.id === exampleId) ?? EXAMPLES[0];

  // Editable source text — initialised from the example entry and
  // reset on every tab switch. The compile loop derives the in-memory
  // file list by swapping this in for the entry file's contents.
  const [source, setSource] = useState(() => entrySource(example));

  const [mesh, setMesh]             = useState<ParsedSTL | null>(null);
  const [markers, setMarkers]       = useState<EditorMarker[]>([]);
  const [evaluating, setEvaluating] = useState(true);
  const [showOverlay, setShowOverlay] = useState(false);
  const [percent, setPercent]       = useState(50);
  const [isDesktop, setIsDesktop]   = useState(true);

  // Blurb under the card. We don't swap straight to `example.blurb` —
  // instead the visible blurb fades out, the text swaps mid-fade,
  // then fades in. Matches the render's suspense rhythm.
  const [blurbText, setBlurbText]   = useState(example.blurb);
  const [blurbShown, setBlurbShown] = useState(true);

  const moduleRef = useRef<CadmlModule | null>(null);
  // One ref per tab so arrow-key cycling can move DOM focus along
  // with the aria-selected state.
  const tabRefs   = useRef<(HTMLButtonElement | null)[]>([]);

  // Reset the editable source whenever the user picks a different
  // example. Markers reset on the next successful compile.
  useEffect(() => {
    setSource(entrySource(example));
  }, [example.id]);

  // Track md: breakpoint so we can swap the slider for a stacked layout.
  useEffect(() => {
    const mq = window.matchMedia('(min-width: 768px)');
    const update = () => setIsDesktop(mq.matches);
    update();
    mq.addEventListener('change', update);
    return () => mq.removeEventListener('change', update);
  }, []);

  // Delay the suspense overlay so fast compiles don't flash it.
  // When evaluating ends we hide immediately (no fade-out flicker).
  useEffect(() => {
    if (!evaluating) {
      setShowOverlay(false);
      return;
    }
    const t = setTimeout(() => setShowOverlay(true), OVERLAY_DELAY_MS);
    return () => clearTimeout(t);
  }, [evaluating]);

  // Cross-fade the blurb text on example switch.
  useEffect(() => {
    if (blurbText === example.blurb) return;
    setBlurbShown(false);
    const t = setTimeout(() => {
      setBlurbText(example.blurb);
      setBlurbShown(true);
    }, 200);
    return () => clearTimeout(t);
  }, [example.blurb, blurbText]);

  // Compile-and-render. Edits debounce so we don't recompile on
  // every keystroke; example switches skip the debounce so the
  // rendered mesh doesn't lag behind the new editor content and
  // camera angle. "Pristine" = source matches the example's stored
  // entry, i.e. no user edits since the last switch. On compile
  // error we surface markers and keep the previous mesh on screen
  // (so the render doesn't go blank while the user fixes the typo).
  useEffect(() => {
    let cancelled = false;
    setEvaluating(true);
    const pristine = source === entrySource(example);
    const delay    = pristine ? 0 : COMPILE_DEBOUNCE_MS;
    const timer = setTimeout(async () => {
      try {
        if (!moduleRef.current) {
          moduleRef.current = await loadCadml();
        }
        if (cancelled) return;
        const M = moduleRef.current;
        const files = example.files.map((f) =>
          f.path === example.entry ? { ...f, contents: source } : f,
        );
        // Compile first to get status + diagnostics; only re-run
        // the full pipeline (via exportStlFromProject) when the
        // compile succeeded. This pays one extra compile on the
        // success path — exportStlFromProject re-compiles internally
        // — but keeps warnings surfaced as squiggles. The proper fix
        // is a single WASM binding returning { stl, errors, warnings }
        // so the document gets compiled once and reused for both.
        const r = M.compileProject(files, example.entry);
        if (cancelled) return;
        if (r.ok) {
          const stl = M.exportStlFromProject(files, example.entry);
          if (cancelled) return;
          if (stl) setMesh(parseBinarySTL(stl));
          setMarkers(errorsToMarkers(r.warnings, MarkerSeverity.Warning, source));
        } else {
          setMarkers(errorsToMarkers(r.errors, MarkerSeverity.Error, source));
        }
      } catch (e) {
        if (!cancelled) {
          setMarkers([{
            startLineNumber: 1, endLineNumber: 1,
            startColumn:     1, endColumn:    16,
            severity:        MarkerSeverity.Error,
            message:         e instanceof Error ? e.message : String(e),
          }]);
        }
      } finally {
        if (!cancelled) setEvaluating(false);
      }
    }, delay);
    return () => { cancelled = true; clearTimeout(timer); };
  }, [source, example.id]);

  const color = firstPartColor(source);
  const editorClipPath = isDesktop
    ? `inset(0 ${100 - percent}% 0 0)`
    : `inset(0 0 50% 0)`;

  return (
    <div className="flex flex-col gap-3 w-full">
      <div
        className="flex gap-1 flex-wrap text-sm"
        role="tablist"
        aria-label="Example"
        onKeyDown={(e) => {
          // ARIA tablist convention: arrows cycle between tabs and
          // DOM focus follows the active tab.
          if (e.key !== 'ArrowLeft' && e.key !== 'ArrowRight' &&
              e.key !== 'Home'      && e.key !== 'End') return;
          e.preventDefault();
          const i = EXAMPLES.findIndex((x) => x.id === exampleId);
          const n = EXAMPLES.length;
          let next: number;
          switch (e.key) {
            case 'ArrowLeft':  next = (i - 1 + n) % n; break;
            case 'ArrowRight': next = (i + 1) % n;     break;
            case 'Home':       next = 0;               break;
            case 'End':        next = n - 1;           break;
            default:           return;
          }
          setExampleId(EXAMPLES[next].id);
          tabRefs.current[next]?.focus();
        }}
      >
        {EXAMPLES.map((e, i) => (
          <button
            key={e.id}
            type="button"
            role="tab"
            ref={(el) => { tabRefs.current[i] = el; }}
            aria-selected={e.id === exampleId}
            tabIndex={e.id === exampleId ? 0 : -1}
            onClick={() => setExampleId(e.id)}
            className={
              'px-3 py-1 rounded-md border transition-colors ' +
              'focus:outline-none focus-visible:ring-2 focus-visible:ring-zinc-400 ' +
              (e.id === exampleId
                ? 'bg-zinc-900 text-white border-zinc-900'
                : 'bg-white text-zinc-700 border-zinc-200 hover:border-zinc-400')
            }
          >
            {e.title}
          </button>
        ))}
      </div>

      <div className="relative w-full aspect-[3/4] md:aspect-[16/10] rounded-xl border border-zinc-200 bg-white overflow-hidden">
        {/* render layer (below) */}
        <div className="absolute inset-0 bg-[linear-gradient(180deg,#fafafa_0%,#f4f4f5_100%)]">
          <Viewport mesh={mesh} color={color} xShift={isDesktop ? 0.15 : 0} cameraPhi={example.cameraPhi} />
        </div>

        {/* suspense overlay — fades in on slow compiles, out on success */}
        <div
          aria-hidden
          className={
            'pointer-events-none absolute inset-0 ' +
            'bg-[linear-gradient(180deg,#fafafa_0%,#f4f4f5_100%)] ' +
            'transition-opacity duration-300 ease-out ' +
            (showOverlay ? 'opacity-70' : 'opacity-0')
          }
        />

        {/* editor layer (above, clipped) */}
        <div
          className="absolute inset-0 bg-white"
          style={{ clipPath: editorClipPath }}
        >
          <CadmlEditor value={source} onChange={setSource} markers={markers} />
        </div>

        {/* slider — desktop only */}
        {isDesktop && (
          <RevealSlider percent={percent} onPercent={setPercent} />
        )}

        {/* mobile divider label */}
        {!isDesktop && (
          <div className="pointer-events-none absolute top-1/2 left-3 -translate-y-1/2 px-2 py-0.5 text-[10px] font-mono uppercase tracking-wider text-zinc-500 bg-white/80 rounded">
            ↑ source · render ↓
          </div>
        )}

        {/* "Evaluating…" badge, gated on the same delay as the overlay */}
        <div
          className={
            'pointer-events-none absolute bottom-3 right-3 px-2 py-1 ' +
            'bg-zinc-900/85 text-white text-xs rounded ' +
            'transition-opacity duration-200 ' +
            (showOverlay ? 'opacity-100' : 'opacity-0')
          }
        >
          Evaluating…
        </div>
      </div>

      <p
        className={
          'text-sm text-zinc-500 transition-opacity duration-200 ' +
          (blurbShown ? 'opacity-100' : 'opacity-0')
        }
      >
        {blurbText}
      </p>
    </div>
  );
}
