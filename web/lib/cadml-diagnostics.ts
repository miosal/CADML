// Convert WASM compile errors into Monaco editor markers (the red
// squiggles). The WASM `errors` field is a newline-joined string;
// each message may or may not carry a `:line:column:` location. When
// a location is present we extract it; otherwise the marker is
// pinned to line 1.

export interface EditorMarker {
  startLineNumber: number;
  endLineNumber:   number;
  startColumn:     number;
  endColumn:       number;
  severity:        MarkerSeverity;
  message:         string;
}

// Matches monaco.MarkerSeverity values without importing the
// (browser-only) monaco-editor module from non-client code.
export enum MarkerSeverity {
  Hint    = 1,
  Info    = 2,
  Warning = 4,
  Error   = 8,
}

// Patterns we try in order. CADML's compiler tends to format
// diagnostics like `foo.cadml:12:8: message` or `at line 12, column
// 8`. Fall back to attaching the message at line 1 if nothing parses.
const LOCATION_PATTERNS: RegExp[] = [
  /(?:^|\s)([^:\s]+\.cadml):(\d+):(\d+)/, // path:line:col
  /(?:^|\s):(\d+):(\d+)/,                 // :line:col
  /\bline\s+(\d+)(?:[,\s]+col(?:umn)?\s+(\d+))?/i, // line N, col M
];

function parseLocation(message: string): { line: number; col: number } {
  for (const re of LOCATION_PATTERNS) {
    const m = message.match(re);
    if (!m) continue;
    // The first numeric capture is line, the second (if any) is col.
    const nums = m.slice(1).filter((s) => /^\d+$/.test(s));
    if (nums.length === 0) continue;
    return {
      line: parseInt(nums[0], 10),
      col:  nums.length > 1 ? parseInt(nums[1], 10) : 1,
    };
  }
  return { line: 1, col: 1 };
}

export function errorsToMarkers(
  errors:   string,
  severity: MarkerSeverity,
  source:   string,
): EditorMarker[] {
  if (!errors) return [];
  const sourceLines = source.split('\n').length;
  return errors
    .split('\n')
    .map((s) => s.trim())
    .filter(Boolean)
    .map((message) => {
      const { line, col } = parseLocation(message);
      const clampedLine = Math.min(Math.max(1, line), sourceLines);
      const clampedCol  = Math.max(1, col);
      return {
        startLineNumber: clampedLine,
        endLineNumber:   clampedLine,
        startColumn:     clampedCol,
        endColumn:       clampedCol + 16, // arbitrary span; Monaco draws to the next char anyway
        severity,
        message,
      };
    });
}
