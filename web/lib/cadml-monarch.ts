// Monaco Monarch language definition for CADML. Pure regex
// tokenisation; no AST, no semantic analysis (the WASM compiler
// handles that and reports back via diagnostics in lib/cadml-
// diagnostics.ts). Highlights:
//   * Line comments (`#`) and XML block comments (`<!-- -->`)
//   * Frontmatter directives at start of line
//   * XML tags, with CADML-specific element names tokenised as
//     `type` so they read differently from generic XML
//   * Attribute names and quoted attribute values
//   * `{ ... }` expression interpolation inside attribute values
//     and on the frontmatter RHS

import type * as monaco from 'monaco-editor';

export const CADML_LANGUAGE_ID = 'cadml';

// Frontmatter directives that lead a line (before any XML body).
const FRONTMATTER_KEYWORDS = [
  'version', 'units', 'description', 'param', 'import',
];

// Element names defined by the CADML 0.1 spec, mirrored verbatim
// from src/cadml/src/types.cpp. Tokenised as `type` so they pick up
// a different theme colour than ad-hoc tags (which fall through to
// `tag`). Excludes the flat-output-only tags param/source/sources
// that authors don't type.
const CADML_ELEMENTS = [
  // structure + composition
  'part', 'def', 'group', 'pattern', 'for', 'port',
  'assembly', 'connect', 'script', 'svg',
  // 2D primitives
  'sketch', 'circle', 'rect', 'path',
  // 2D-to-3D
  'extrude', 'revolve', 'sweep', 'loft', 'helix',
  // mesh import
  'stl',
  // booleans + hull
  'union', 'difference', 'intersect', 'hull',
  // modifiers
  'shell', 'fillet', 'chamfer', 'cut',
];

export const cadmlLanguage: monaco.languages.IMonarchLanguage = {
  defaultToken:  '',
  tokenPostfix:  '.cadml',
  ignoreCase:    false,

  frontmatterKeywords: FRONTMATTER_KEYWORDS,
  cadmlElements:       CADML_ELEMENTS,

  tokenizer: {
    root: [
      // Line comment (frontmatter style).
      [/#.*$/, 'comment'],

      // XML comment.
      [/<!--/, { token: 'comment', next: '@xmlComment' }],

      // Closing tag — split into delimiters + name so the name uses
      // the same colour as the matching opening tag and the angle
      // brackets sit at the subdued delimiter colour.
      [/(<\/)([a-zA-Z][\w-]*)(\s*)(>)/, {
        cases: {
          '$2@cadmlElements': ['delimiter.angle', 'type', '', 'delimiter.angle'],
          '@default':         ['delimiter.angle', 'tag',  '', 'delimiter.angle'],
        },
      }],

      // Opening tag start — angle bracket as delimiter, name as
      // tag/type, then push into @tag for attribute parsing. Monarch
      // disallows the array form of `token` with a sibling `next`, so
      // the transition rides on the second capture's action.
      [/(<)([a-zA-Z][\w-]*)/, {
        cases: {
          '$2@cadmlElements': ['delimiter.angle', { token: 'type', next: '@tag' }],
          '@default':         ['delimiter.angle', { token: 'tag',  next: '@tag' }],
        },
      }],

      // Frontmatter directive at the start of a line.
      [/^[ \t]*([a-z]+)\b/, {
        cases: {
          '$1@frontmatterKeywords': 'keyword',
          '@default':               'identifier',
        },
      }],

      // Quoted string (e.g. the `description "..."` directive value).
      [/"/, { token: 'string.quote', next: '@string' }],

      // `{ ... }` expression on the RHS of `param NAME = { ... }`.
      [/\{/, { token: 'delimiter.curly', next: '@expression' }],

      // Numbers (including a leading sign in expressions, but in root
      // we keep it simple — sign would normally be its own token).
      [/\b\d+(\.\d+)?\b/, 'number'],

      [/=/,           'delimiter'],
      [/[+\-*/^]/,    'operator'],
      [/[a-zA-Z][\w-]*/, 'identifier'],
      [/[ \t]+/,      ''],
    ],

    // Inside `<tag ...` — attributes until `>` or `/>`.
    tag: [
      [/[ \t]+/,                  ''],
      [/([a-zA-Z][\w-]*)\s*(=)/,  ['attribute.name', 'delimiter']],
      [/"/,                       { token: 'string.quote', next: '@attrString' }],
      [/\/>/,                     { token: 'delimiter.angle', next: '@pop' }],
      [/>/,                       { token: 'delimiter.angle', next: '@pop' }],
    ],

    // Inside a `"..."` attribute value. `{ ... }` slips back into
    // expression mode mid-string so e.g. `width="{w - 2*wall}"`
    // tokenises with the inner expression highlighted.
    attrString: [
      [/\{/,        { token: 'delimiter.curly', next: '@expression' }],
      [/[^"{]+/,    'string'],
      [/"/,         { token: 'string.quote', next: '@pop' }],
    ],

    // Inside a `{ ... }` expression — numbers, operators, identifiers
    // (which may include the `lua-module.function` dotted form), and
    // grouping. The expression mini-language is small.
    expression: [
      [/\}/,                                  { token: 'delimiter.curly', next: '@pop' }],
      [/[ \t]+/,                              ''],
      [/[+\-*/^]/,                            'operator'],
      [/\b\d+(\.\d+)?\b/,                     'number'],
      [/[a-zA-Z][\w-]*(\.[a-zA-Z][\w_]*)*/,   'identifier'],
      [/[(),]/,                               'delimiter'],
    ],

    // Plain `"..."` string outside of an attribute (frontmatter
    // `description "..."` mostly). No interpolation.
    string: [
      [/[^"]+/, 'string'],
      [/"/,     { token: 'string.quote', next: '@pop' }],
    ],

    xmlComment: [
      [/-->/,    { token: 'comment', next: '@pop' }],
      [/[^-]+/,  'comment'],
      [/-/,      'comment'],
    ],
  },
};

// Editor niceties: line/block comment toggles (Ctrl+/), bracket
// matching, and a small set of auto-closing pairs.
export const cadmlLanguageConfiguration: monaco.languages.LanguageConfiguration = {
  comments: {
    lineComment:  '#',
    blockComment: ['<!--', '-->'],
  },
  brackets: [
    ['<', '>'],
    ['{', '}'],
    ['(', ')'],
  ],
  autoClosingPairs: [
    { open: '"', close: '"' },
    { open: '{', close: '}' },
    { open: '(', close: ')' },
  ],
  surroundingPairs: [
    { open: '"', close: '"' },
    { open: '{', close: '}' },
    { open: '(', close: ')' },
  ],
};

// Custom theme keyed to CADML's tag taxonomy. Palette is GitHub
// Light; CADML built-in elements (`type` token) and generic XML tags
// (`tag` token) get different foregrounds so the eye separates them.
// Inherits from `vs` so anything we don't override keeps the default
// light-mode treatment.
export const CADML_THEME_ID = 'cadml-light';

const themeRules: monaco.editor.ITokenThemeRule[] = [
  { token: 'delimiter.angle.cadml', foreground: '6E6E80' },
  { token: 'delimiter.cadml',       foreground: '6E6E80' },
  { token: 'delimiter.curly.cadml', foreground: '6E6E80' },
  { token: 'tag.cadml',             foreground: '0550AE' },
  { token: 'type.cadml',            foreground: '116329' },
  { token: 'attribute.name.cadml',  foreground: '953800' },
  { token: 'string.cadml',          foreground: '0A3069' },
  { token: 'string.quote.cadml',    foreground: '0A3069' },
  { token: 'keyword.cadml',         foreground: 'CF222E' },
  { token: 'number.cadml',          foreground: '0550AE' },
  { token: 'operator.cadml',        foreground: 'CF222E' },
  { token: 'comment.cadml',         foreground: '6E7781', fontStyle: 'italic' },
];

// Idempotent global registration: Monaco's language registry is a
// singleton across the page, so re-registering is fine to no-op.
let registered = false;
export function registerCadmlLanguage(monaco: typeof import('monaco-editor')): void {
  if (registered) return;
  registered = true;
  monaco.languages.register({ id: CADML_LANGUAGE_ID, extensions: ['.cadml'] });
  monaco.languages.setMonarchTokensProvider(CADML_LANGUAGE_ID, cadmlLanguage);
  monaco.languages.setLanguageConfiguration(CADML_LANGUAGE_ID, cadmlLanguageConfiguration);
  monaco.editor.defineTheme(CADML_THEME_ID, {
    base:    'vs',
    inherit: true,
    rules:   themeRules,
    colors:  {},
  });
}
