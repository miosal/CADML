// Monaco editor wrapper. Read-only by default; pass `onChange` to
// enable editing. We lazy-load Monaco via next/dynamic so it never
// enters the SSR pass (Monaco depends on window / document / web
// workers).
//
// On mount we register the CADML Monarch language with the page-wide
// Monaco singleton; subsequent editor instances reuse the same
// registration. Markers (squiggles) come in via the `markers` prop —
// the caller drives them from WASM compile errors.

'use client';

import dynamic from 'next/dynamic';
import { useEffect, useRef } from 'react';
import type { OnMount, OnChange } from '@monaco-editor/react';
import type * as monacoNs from 'monaco-editor';

import { CADML_LANGUAGE_ID, CADML_THEME_ID, registerCadmlLanguage } from '@/lib/cadml-monarch';
import type { EditorMarker }                        from '@/lib/cadml-diagnostics';

const Monaco = dynamic(
  () => import('@monaco-editor/react').then((m) => m.default),
  {
    ssr: false,
    loading: () => (
      <div className="w-full h-full flex items-center justify-center text-xs text-zinc-400">
        Loading editor…
      </div>
    ),
  },
);

const MARKER_OWNER = 'cadml-wasm';

interface EditorProps {
  value:     string;
  onChange?: (v: string) => void;
  markers?:  EditorMarker[];
}

export function CadmlEditor({ value, onChange, markers }: EditorProps) {
  const editorRef = useRef<monacoNs.editor.IStandaloneCodeEditor | null>(null);
  const monacoRef = useRef<typeof monacoNs | null>(null);

  const handleMount: OnMount = (editor, monaco) => {
    editorRef.current = editor;
    monacoRef.current = monaco;
    registerCadmlLanguage(monaco);
    // The Editor was instantiated with `defaultLanguage="cadml"` so
    // the model already speaks CADML by the time onMount fires; this
    // is a belt-and-braces switch in case Monaco fell back to plain
    // text (which it does silently if the language wasn't registered
    // at model-creation time).
    const model = editor.getModel();
    if (model && model.getLanguageId() !== CADML_LANGUAGE_ID) {
      monaco.editor.setModelLanguage(model, CADML_LANGUAGE_ID);
    }
  };

  const handleChange: OnChange = (v) => {
    if (v !== undefined) onChange?.(v);
  };

  // Push markers when the prop changes. setModelMarkers replaces all
  // markers under our owner string, so passing `[]` clears.
  useEffect(() => {
    const editor = editorRef.current;
    const monaco = monacoRef.current;
    if (!editor || !monaco) return;
    const model = editor.getModel();
    if (!model) return;
    monaco.editor.setModelMarkers(model, MARKER_OWNER, markers ?? []);
  }, [markers]);

  return (
    <Monaco
      defaultLanguage={CADML_LANGUAGE_ID}
      value={value}
      onChange={handleChange}
      onMount={handleMount}
      theme={CADML_THEME_ID}
      options={{
        readOnly:             !onChange,
        minimap:              { enabled: false },
        fontSize:             12,
        lineHeight:           18,
        lineNumbers:          'on',
        scrollBeyondLastLine: false,
        wordWrap:             'off',
        renderWhitespace:     'none',
        guides:               { indentation: false },
        folding:              false,
        contextmenu:          false,
        automaticLayout:      true,
        scrollbar:            { verticalScrollbarSize: 8, horizontalScrollbarSize: 8 },
        padding:              { top: 12 },
      }}
    />
  );
}
