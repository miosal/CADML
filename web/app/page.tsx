import { Showcase }            from '@/components/Showcase';
import { GitHubIcon, ExternalLinkIcon } from '@/components/Icons';

const REPO_URL = 'https://github.com/miosal/cadml';
const SPEC_URL = 'https://github.com/miosal/cadml/blob/main/docs/spec/language.md';
const EXAMPLES_URL = 'https://github.com/miosal/cadml/tree/main/examples';

export default function Home() {
  return (
    <div className="min-h-full flex flex-col">
      <header className="border-b border-zinc-200">
        <div className="mx-auto max-w-7xl px-4 sm:px-6 h-14 flex items-center justify-between">
          <div className="flex items-center gap-2 text-sm">
            {/* eslint-disable-next-line @next/next/no-img-element */}
            <img src="/icon.svg" alt="" aria-hidden className="w-5 h-5" />
            <span className="font-semibold tracking-tight">CADML</span>
          </div>
          <nav className="flex items-center gap-3 sm:gap-5 text-sm text-zinc-600">
            <a className="inline-flex items-center gap-1 hover:text-zinc-900" href={SPEC_URL}>
              <span>Spec</span>
              <ExternalLinkIcon className="w-3 h-3 text-zinc-400 hidden sm:inline" />
            </a>
            <a className="inline-flex items-center gap-1 hover:text-zinc-900" href={EXAMPLES_URL}>
              <span>Examples</span>
              <ExternalLinkIcon className="w-3 h-3 text-zinc-400 hidden sm:inline" />
            </a>
            <a className="inline-flex items-center gap-1.5 hover:text-zinc-900" href={REPO_URL} aria-label="GitHub">
              <GitHubIcon className="w-4 h-4" />
              <span className="hidden sm:inline">GitHub</span>
              <ExternalLinkIcon className="w-3 h-3 text-zinc-400 hidden sm:inline" />
            </a>
          </nav>
        </div>
      </header>

      <main className="flex-1">
        <section className="mx-auto max-w-7xl px-4 sm:px-6 pt-10 pb-16 md:pt-24 md:pb-32">
          <div className="grid grid-cols-1 md:grid-cols-[2fr_3fr] gap-8 md:gap-12 items-start">
            <div>
              <h1 className="text-4xl sm:text-5xl md:text-6xl font-semibold tracking-tight">
                CADML
              </h1>
              <p className="mt-2 text-base text-zinc-500">
                Computer-Aided Design Markup Language
              </p>
              <p className="mt-5 md:mt-6 text-lg md:text-xl text-zinc-700 leading-snug">
                A declarative XML-based language for parametric
                solid modelling.
              </p>
              <p className="mt-4 text-sm md:text-base text-zinc-500 leading-relaxed">
                A C++ reference toolchain compiles{' '}
                <code className="text-[0.9em] px-1 py-0.5 bg-zinc-100 rounded">.cadml</code>{' '}
                to triangle meshes and exports STL, glTF, or 3MF. The
                same pipeline runs on this page as WebAssembly.
              </p>
              <div className="mt-6 md:mt-8 flex flex-wrap items-center gap-2 sm:gap-3 text-sm">
                <a
                  href={SPEC_URL}
                  className="inline-flex items-center gap-1.5 px-4 py-2 rounded-md bg-zinc-900 text-white hover:bg-zinc-800"
                >
                  <span>Read the spec</span>
                  <ExternalLinkIcon className="w-3.5 h-3.5 opacity-70" />
                </a>
                <a
                  href={REPO_URL}
                  className="inline-flex items-center gap-1.5 px-4 py-2 rounded-md border border-zinc-200 text-zinc-800 hover:border-zinc-400"
                >
                  <GitHubIcon className="w-4 h-4" />
                  <span>View on GitHub</span>
                  <ExternalLinkIcon className="w-3 h-3 text-zinc-400" />
                </a>
                <a
                  href={`${REPO_URL}/blob/main/LICENSE`}
                  className="inline-flex items-center gap-1.5 px-2.5 py-1 text-xs font-mono bg-zinc-100 text-zinc-600 rounded hover:bg-zinc-200"
                  aria-label="License: Apache 2.0"
                >
                  <span>Apache-2.0</span>
                  <ExternalLinkIcon className="w-3 h-3 text-zinc-400" />
                </a>
              </div>
            </div>

            <div className="md:pt-3">
              <Showcase />
            </div>
          </div>
        </section>

        <Divider />

        <section id="what" className="mx-auto max-w-3xl px-4 sm:px-6 py-12 md:py-20">
          <h2 className="text-2xl font-semibold tracking-tight">What CADML is</h2>
          <div className="mt-6 space-y-4 text-zinc-700 leading-relaxed">
            <p>
              CADML is a language specification with a C++ reference
              implementation. A{' '}
              <code className="text-[0.9em] px-1 py-0.5 bg-zinc-100 rounded">.cadml</code>{' '}
              file is line-oriented frontmatter — version, params,
              imports — followed by an XML body of CSG operations,
              sketches, and modifiers (fillet, chamfer, shell).
              Expressions live in a compact{' '}
              <code className="text-[0.9em] px-1 py-0.5 bg-zinc-100 rounded">{`{...}`}</code>{' '}
              mini-language; a sandboxed Lua module handles cases the
              declarative grammar doesn&apos;t cover.
            </p>
            <p>
              The reference toolchain is single-process and
              deterministic: parser → bundler → evaluator → STL / glTF
              / 3MF exporters.
            </p>
          </div>
        </section>

        <Divider />

        <section id="why" className="mx-auto max-w-3xl px-4 sm:px-6 py-12 md:py-20">
          <h2 className="text-2xl font-semibold tracking-tight">
            Why another CAD DSL?
          </h2>
          <div className="mt-6 space-y-4 text-zinc-700 leading-relaxed">
            <p>
              Proposing a new format invites the{' '}
              <a
                href="https://xkcd.com/927"
                className="inline-flex items-center gap-1 text-[var(--color-accent)] hover:underline"
              >
                <span>xkcd 927</span>
                <ExternalLinkIcon className="w-3 h-3 text-zinc-400" />
              </a>{' '}
              problem — text-first CAD languages already exist, and the
              comparison table below covers the ones we know of.
            </p>
            <p>
              LLMs author existing CAD DSLs competently today with a
              skill file. The open question is scale: complex
              assemblies, larger projects, less hand-holding. Markup
              is heavily represented in LLM training data and CAD
              DSLs less so; frontier models already author non-trivial
              SVGs from prose. The hunch is that the broader markup
              substrate scales further than a CAD-DSL skill file
              alone.
            </p>
          </div>
        </section>

        <Divider />

        <section id="compared" className="mx-auto max-w-3xl px-4 sm:px-6 py-12 md:py-20">
          <h2 className="text-2xl font-semibold tracking-tight">
            Compared to existing tools
          </h2>
          <div className="mt-6 -mx-4 sm:mx-0 overflow-x-auto">
            <table className="w-full min-w-[640px] text-xs sm:text-sm border-collapse">
              <thead>
                <tr className="text-left border-b border-zinc-200 text-xs uppercase tracking-wider">
                  <th className="py-2 px-3 sm:pl-4 font-medium text-zinc-500">Tool</th>
                  <th className="py-2 pr-3 font-medium text-zinc-500">Source</th>
                  <th className="py-2 pr-3 font-medium text-zinc-500">Engine</th>
                  <th className="py-2 pr-3 font-medium text-zinc-500">Kernel</th>
                  <th className="py-2 pr-3 font-medium text-zinc-500">Solver</th>
                  <th className="py-2 pr-3 sm:pr-0 font-medium text-zinc-500">GUI</th>
                </tr>
              </thead>
              <tbody className="align-top">
                <tr className="border-b border-zinc-100 bg-zinc-50/60">
                  <td className="py-3 pr-3 font-mono text-zinc-900">CADML</td>
                  <td className="py-3 pr-3 text-zinc-700">XML markup</td>
                  <td className="py-3 pr-3 text-zinc-700">Manifold</td>
                  <td className="py-3 pr-3 text-zinc-700">Mesh</td>
                  <td className="py-3 pr-3 text-zinc-400">—</td>
                  <td className="py-3 text-zinc-400">—</td>
                </tr>
                <tr className="border-b border-zinc-100">
                  <td className="py-3 pr-3 font-mono text-zinc-800">OpenSCAD</td>
                  <td className="py-3 pr-3 text-zinc-700">Functional DSL</td>
                  <td className="py-3 pr-3 text-zinc-700">CGAL / Manifold</td>
                  <td className="py-3 pr-3 text-zinc-700">Mesh</td>
                  <td className="py-3 pr-3 text-zinc-400">—</td>
                  <td className="py-3 text-zinc-700">Preview</td>
                </tr>
                <tr className="border-b border-zinc-100">
                  <td className="py-3 pr-3 font-mono text-zinc-800">JSCAD</td>
                  <td className="py-3 pr-3 text-zinc-700">JavaScript</td>
                  <td className="py-3 pr-3 text-zinc-700">manifold-3d</td>
                  <td className="py-3 pr-3 text-zinc-700">Mesh</td>
                  <td className="py-3 pr-3 text-zinc-400">—</td>
                  <td className="py-3 text-zinc-700">Web</td>
                </tr>
                <tr className="border-b border-zinc-100">
                  <td className="py-3 pr-3 font-mono text-zinc-800">CadQuery</td>
                  <td className="py-3 pr-3 text-zinc-700">Python</td>
                  <td className="py-3 pr-3 text-zinc-700">OpenCascade</td>
                  <td className="py-3 pr-3 text-zinc-700">B-rep</td>
                  <td className="py-3 pr-3 text-zinc-400">—</td>
                  <td className="py-3 text-zinc-400">—</td>
                </tr>
                <tr className="border-b border-zinc-100">
                  <td className="py-3 pr-3 font-mono text-zinc-800">FreeCAD</td>
                  <td className="py-3 pr-3 text-zinc-700">Python · GUI</td>
                  <td className="py-3 pr-3 text-zinc-700">OpenCascade</td>
                  <td className="py-3 pr-3 text-zinc-700">B-rep</td>
                  <td className="py-3 pr-3 text-zinc-700">Sketcher</td>
                  <td className="py-3 text-zinc-700">Native</td>
                </tr>
                <tr className="border-b border-zinc-100">
                  <td className="py-3 pr-3 font-mono text-zinc-800">Zoo KCL</td>
                  <td className="py-3 pr-3 text-zinc-700">Imperative DSL</td>
                  <td className="py-3 pr-3 text-zinc-700">Hosted (Zoo)</td>
                  <td className="py-3 pr-3 text-zinc-700">B-rep</td>
                  <td className="py-3 pr-3 text-zinc-400">—</td>
                  <td className="py-3 text-zinc-700">Zoo Studio</td>
                </tr>
                <tr>
                  <td className="py-3 pr-3 font-mono text-zinc-800">Onshape / Fusion / SolidWorks</td>
                  <td className="py-3 pr-3 text-zinc-700">GUI only</td>
                  <td className="py-3 pr-3 text-zinc-700">Proprietary</td>
                  <td className="py-3 pr-3 text-zinc-700">B-rep</td>
                  <td className="py-3 pr-3 text-zinc-700">Sketch + mate</td>
                  <td className="py-3 text-zinc-700">Native</td>
                </tr>
              </tbody>
            </table>
          </div>
        </section>

        <Divider />

        <section id="start" className="mx-auto max-w-3xl px-4 sm:px-6 py-12 md:py-20">
          <h2 className="text-2xl font-semibold tracking-tight">Get started</h2>
          <ul className="mt-6 space-y-3 text-zinc-700">
            <li>
              <a className="inline-flex items-center gap-1 text-[var(--color-accent)] hover:underline" href={SPEC_URL}>
                <span>docs/spec/language.md</span>
                <ExternalLinkIcon className="w-3 h-3 text-zinc-400" />
              </a>{' '}
              <span className="text-zinc-500">— the normative language specification.</span>
            </li>
            <li>
              <a className="inline-flex items-center gap-1 text-[var(--color-accent)] hover:underline" href={EXAMPLES_URL}>
                <span>examples/</span>
                <ExternalLinkIcon className="w-3 h-3 text-zinc-400" />
              </a>{' '}
              <span className="text-zinc-500">— curated examples exercising major language feature.</span>
            </li>
            <li>
              <a className="inline-flex items-center gap-1.5 text-[var(--color-accent)] hover:underline" href={REPO_URL}>
                <GitHubIcon className="w-3.5 h-3.5" />
                <span>miosal/cadml</span>
                <ExternalLinkIcon className="w-3 h-3 text-zinc-400" />
              </a>{' '}
              <span className="text-zinc-500">— source code and build instructions.</span>
            </li>
          </ul>
        </section>
      </main>

      <footer className="border-t border-zinc-200 mt-12">
        <div className="mx-auto max-w-7xl px-4 sm:px-6 py-8 text-sm text-zinc-500 flex flex-wrap items-center gap-x-4 gap-y-2 justify-between">
          <span>CADML v{process.env.NEXT_PUBLIC_CADML_VERSION} · © {new Date().getFullYear()} miosal</span>
          <a
            href={REPO_URL}
            className="inline-flex items-center gap-1.5 hover:text-zinc-900"
          >
            <GitHubIcon className="w-4 h-4" />
            <span>github.com/miosal/cadml</span>
            <ExternalLinkIcon className="w-3 h-3 text-zinc-400" />
          </a>
        </div>
      </footer>
    </div>
  );
}

function Divider() {
  return (
    <div className="mx-auto max-w-3xl px-4 sm:px-6">
      <div className="h-px bg-zinc-200" />
    </div>
  );
}
