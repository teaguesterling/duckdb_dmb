# DCS: Cascading Duck Sheets

Status: **approved by Teague (2026-11-13).** Engine name proposed by Teague: **dmb** ("Duck Mutation Blocks");
sheets stay `.dcs`. It lives in duckeye-android until its own repo
exists. Tracked in duckeye-android#56. Working sketch: https://claude.ai/artifact/7n8jfrub5UQaxnG1cKYUr5

## What it is

**DCS is a CSS-shaped mutator and style language for duck_blocks documents.** It isn't a builder:
the builder that turns rows into blocks is #55, and DCS styles what it builds. A `.dcs` sheet is a
list of rules, each a selector plus declarations. Applying the sheet to a document's blocks gives
new blocks, which any duck_block_utils renderer writes out (Markdown, HTML, text, and panduck's
writers).

```css
/* Teague's first use (2026-11-13): a ¶ anchor after every heading */
h2, h3 { -dcs-id: slug(text()); }
h2::after, h3::after {
  content: " [¶](#" attr(id) ")";
  -dcs-content-as: markdown;
}
```

## Decisions so far (Teague, 2026-11-12/13)

| Decision | His words |
|---|---|
| A mutator and style language, not a builder | "that's not a builder... it's a mutator/style language" |
| Name and extension | "cascading duck sheets .cds", then "I like dcs" |
| Styling where the output supports it | "Should it also allows styling if the output language supports it?" |
| Many content formats | "text, md\|markdown, rst\|restructured, latex, html, etc." |
| Prune, flatten and more | "What other mods should it support? Pruning? Flattening? ???" |
| Its own repo, on duck_block_utils | "it can be a separate repo ... New extension that users duck block utils and maybe depends on sitting_duck (or on sitting_duckling formally, once it's ready)" |
| Maybe the home of ast_select | "Might actually be worth having that repo handle ast_select too" (open: see below) |
| Shared by both duckeyes | the reader regression suite (teaguesterling/duckeye#28) "should probably land in the duck_block_mods package eventually but should be shared between duckeyes" |

**Property prefix: `-dcs-`.** The sketch and #56 said `-duckeye-` and `-cds-`. The extension is
`.dcs`, so the prefix matches it (Claude's call).

## Goals and non-goals

**Goals:**
- One rule language that the CLI (`duckeye --sheet file.dcs`), the Android app and SQL all use.
- Reader mode, "Clean up for Obsidian" and site recipes become sheets.
- Every change can be explained: which rule changed which block (the "flip" view, #55).

**Non-goals:**
- Not a programming language: no loops or variables beyond CSS's own counters.
- No network access from a sheet, and no file access beyond `@import` of other sheets.
- Not a layout engine. Presentational properties pass through to renderers, which decide.

## How it works: reuse the self-hosting trick

sitting_duck's `ast_select` already parses CSS with **tree-sitter-css** and compiles selectors to
SQL over its node table (sitting_duck docs/explanation/selector-engine.md). Two facts make DCS a
thin layer on top:

1. **tree-sitter-css parses whole sheets,** declarations included (rule_set, declaration,
   property_name, call_expression), not only selectors. So a `.dcs` file parses the same way, with
   no hand-written parser.
2. **Blocks are already an AST.** The duckeye CLI runs ast_select over document blocks
   (duckeye:2105). "a depth-first ordering plus a level column fully determines a tree, so node_id
   is row position and a node's parent is the nearest preceding row at a smaller level."
   sitting_duck#117 asks for the two changes that remove the CLI's workarounds.

```
sheet.dcs ──tree-sitter-css──▶ rules table (selector, property, value, specificity, order)
                                      │
blocks ──block tree──▶ DCS's own C++ matcher ──▶ matches (rule, block)
                         (differential-tested against ast_select_from on projected blocks)
                                      │
                         phases (below) ──▶ new blocks ──▶ any renderer
```

### What the sitting_duck session found (ast-select-perf, 2026-11-13)

- **Parsing works.** ast_select already parses with `parse_ast_list_table(selector, 'css')`
  (css_selectors.sql:94). It ran parse_ast on this spec's own rules: `h2::after` became a
  pseudo_element_selector, `content` a declaration with string_value and `attr(id)`, and
  `-cds-content-as: markdown` a declaration with plain_value. **Measured.**
- **The selector engine is not separable today.** ast_select_from binds against read_ast's full
  schema: dropping `parameters` or `language` gives a Binder Error, on main and on v1.18.4.
  css_selectors.sql (2,466 lines) uses parent_id, descendant_count, depth, flags, peek, semantic_type
  and more. The duckeye CLI works only because it fills that full schema. A core over just
  `(node_id, descendant_count, type, name, attributes)` doesn't exist. **Measured.**
- **Planning, not execution, is the cost.** Before a fix, one call spent about 16.7 s planning
  against 0.125 s executing. Decorrelating one pseudo-class took a call from about 17.3 s to 2.3 s.
  `PREPARE` amortizes planning only with constant arguments: 18.1 s once, then 0.2 s per EXECUTE.
  DuckDB 2.0's planner is about 20 times slower than v1.5 on this statement shape
  (duckdb/duckdb#26036, open). **Measured.**
- **Selector lists (`.a, .b`) return 0 rows today, with no error.** **Measured.** Filed as
  teaguesterling/sitting_duck#330. Until it's fixed, the DCS/ast_select_from differential leaves out
  selector lists, or marks them as expected failures citing #330.
- **The plan** (read from sitting_duck's docs, not started):
  - v2 splits into sitting_duckling (the engine, registry and a runtime grammar loader, with no
    languages), sitting_duck_languages, and sitting_duck as the full set. That's milestone M3,
    gated by "split after stability". The engine moves from sitting_duck into sitting_duckling,
    not through any third repo.
  - **Spec 046** (tracker/features/046-cpp-selector-engine.md) moves only selector parsing,
    validation and planning into C++. It "keeps the row-producing work in SQL/DuckDB", and it is
    "explicitly not a rewrite of the semantics".
  - 046's acceptance is sitting_duck's own selector suite, passing unchanged, and those semantics
    read the full read_ast schema.
  - 046 names "two implementations in flight" as a risk, checked by an EXCEPT-both-ways differential.

**What this changes (Claude's reading, corrected by the sitting_duck session):**
- **Calling ast_select once per rule is too slow for DCS** (the sitting_duck session agrees). A
  ten-rule sheet would cost tens of seconds of planning per document, and on a phone more. So DCS
  must not match through per-rule ast_select calls.
- **046 is not a shared narrow engine.** It stays coupled to read_ast's full schema by design. A
  narrow node interface would be a new design, DCS's own, not part of 046.
- **The honest option for DCS v1 is its own small matcher** for the block selector subset (block
  types, ids, attributes, the tree combinators, `:has`, `:not`, `:contains`, `section()`).
  - It's written in C++ in the DCS extension and runs in one pass, with no SQL planned per rule.
  - To keep its semantics aligned with ast_select, DCS adopts 046's own check: an EXCEPT-both-ways
    differential against ast_select_from over blocks projected to the full schema (the duckeye CLI's
    projection), for the shared selector forms.
  - Converging on one engine stays possible later, as a proposal to sitting_duckling. It would be a
    departure from 046, and Teague's call.
- Parsing stays the self-hosting tree-sitter-css route in every option.
- **DCS splits comma lists itself.** Each selector in `h2, h3` becomes its own rule, so the
  selector-list bug doesn't matter.

## Setting fields and attributes

Teague (2026-11-13): "Isn't a slugged id set in blocks by default? Also, do we want a way to set
other fields and or attributes?"

**Ids, measured** (x86 p4 build: markdown v1.9.9, webbed 2.15.x):
- read_markdown_blocks gives every heading a slug id (`# Field guide` → `id=field-guide`).
- But it ignores Pandoc's `{#dabblers}`: the id came out as `dabbling-ducks-dabblers`, with the
  attribute block left in the title.
- read_html_blocks keeps only explicit ids: `<h2>No id</h2>` has none.

So ids differ by reader. That's filed as duckdb_markdown#111, duckdb_webbed#275 and
duckdb_panduck#182 (all filed with Teague's go). `-dcs-id` stays for whatever the readers leave
out, and `-dcs-id: auto` means "the shared slug rule, only where no id exists".

**Setting fields and attributes, the CSS way:**

| What | Property | Example |
|---|---|---|
| **Any attribute** | a CSS custom property: `--name: value` sets `attributes['name']` | `h2 { --audience: "kids"; }`, `--source: url();` |
| Remove an attribute | `--name: unset` | `img { --title: unset; }` |
| id | `-dcs-id: <value> \| auto` | `h2 { -dcs-id: slug(text()); }` |
| classes | `-dcs-class: +note -draft` (add and remove) | callout styling |
| type | `-dcs-as: <element_type>` | retag |
| heading rank | `-dcs-level: +1 \| 3` | demote |
| text | `content` on the element itself (not `::before`/`::after`) replaces its text, as CSS allows | `h1 { content: text() " (draft)"; }` |

- Custom properties for attributes are standard CSS, and values take the same functions as
  `content` (`attr()`, `text()`, `slug()`, `url()`, strings).
- **Reserved keys** (`heading_level`, `class`, `id`) are set only through their own properties,
  so a sheet can't produce a heading whose level and rank disagree.

## The duck_blocks vocabulary, as a duckent tree

Teague (2026-11-13): "In duckent .x is customizable as is [attr]. I think you should define our
classes, pseudo classes, etc. For duck_blocks. It could be based on what information we capture (or
should capture) from docs, which may already have classes."

duckent (~/Projects/duckent, design stage) defines what a selector means over any ordered tree of
rows. A tree declares its slots: a type, an id, a **classes** accessor (VARCHAR[]), attributes in
two tiers (named and typed, plus a map catch-all), and **pseudo-classes** bound per tree. It fails
legibly: a selector the tree can't answer is refused by name, never an empty result. So DCS's
vocabulary is a duckent shape for duck_blocks. DCS v1's matcher implements duckent's portable
selector fragment over that shape, and swaps to duckent when duckent lands.

```sql
CREATE TREE duck_blocks (SHAPE ONLY,
  ORDER element_order, LEVEL level,
  TYPE    element_type,                       -- heading, paragraph, list_item, table, code …
  ID      attributes['id'],
  CLASSES duck_block_classes(attributes),     -- the source's own classes (below)
  ATTR (heading_level := TRY_CAST(attributes['heading_level'] AS INT),
        language := attributes['language'], role := attributes['role'],
        href := attributes['href'], src := attributes['src'], alt := attributes['alt'],
        list_type := attributes['list_type'], page := TRY_CAST(attributes['page_number'] AS INT),
        format := attributes['format'], kind := kind, encoding := encoding,
        text := duck_block_text(content), words := duck_block_words(content)),
  ATTRS MAP attributes,                       -- every other key, string ops only
  PSEUDO (…)                                  -- the table below
);
```

### `.x`: the source's own classes, normalised

**`.x` keeps CSS's meaning: a class the document itself gave the block.** Computed facts are
pseudo-classes, so author intent and duckeye's inferences never mix.

| Source | What becomes a class | Captured today? |
|---|---|---|
| HTML | `class="…"`, split on spaces | only on `section` blocks (duck_blocks_spec.md:264) |
| Markdown (Pandoc attributes) | `{.note .wide}` | to check per reader |
| Obsidian and GitHub callouts | `> [!warning]` gives `.callout` and `.warning` | no |
| DOCX | the paragraph style name, slugged: "Heading 1" → `.heading-1`, "Intense Quote" → `.intense-quote` | no; worth asking panduck |
| ODT | the style name, as DOCX | no |
| EPUB | `epub:type` (`.footnote`, `.chapter`) as well as HTML class | no |
| LaTeX | the environment name: `theorem`, `lemma`, `abstract` | no |
| Code blocks | the fence's language also as a class: `.language-python` (the Markdown-to-HTML convention) | `language` only |

`duck_block_classes(attributes)` reads them from one agreed key, `attributes['class']`. That key is
already used on sections. The "Captured today?" column lists the reader changes that would fill it:
asks for panduck, webbed and markdown, filed only with Teague's word.

### `[attr]`: named attributes, then the map

The ATTR list above is the named, typed tier, so `[heading_level >= 2]` and `[page > 10]` compare
as numbers. Every other key in `attributes` is reachable through the map tier with string operators
(`[source_type=docx]`). Unknown names are refused by name, as duckent requires.

### Pseudo-classes

| Group | Pseudo-class | Meaning |
|---|---|---|
| **CSS structural** | `:first-child`, `:last-child`, `:only-child`, `:nth-child()`, `:nth-of-type()`, `:empty`, `:root` | as CSS, over the block tree |
| | `:lang(en)` | CSS's own; from `attributes['lang']` where a reader records it |
| **Text** | `:contains("…")` | the block's text contains it (jQuery's) |
| | `:matches(/re/)` | the text matches a regular expression |
| **Document roles** | `:heading`, `:h(2)` | any heading; one rank |
| | `:body` | renderable content, `duck_block_is_body` (duck_blocks_spec.md:136) |
| | `:meta` | front matter and metadata blobs |
| | `:main` | `role=main`, else the single main `article` (reader mode, duckeye#28 defect 3) |
| | `:chrome` | `role` in nav, header, footer or aside, plus link-only paragraphs: reader mode's prune target |
| | `:caption`, `:footnote`, `:callout`, `:callout(warning)` | by type and class |
| **Data** | `:data` | a `table`, or a `code` block in csv, tsv, json, jsonl or yaml: the embedded-data candidates (duck_block_utils#79) |
| | `:image-only`, `:link-only` | a paragraph holding only an image or a link |
| **Links** | `:external`, `:internal` | links off or on the document's own host |
| **Sections** | `section(sel)` | the scope: a heading matching `sel` and everything under it, as duck_blocks_sections draws it (duck_block_utils#82) |

**Reader mode becomes a short sheet:**
`:chrome, :meta { display: none } :main { … } script, style { display: none }`.
That is how DCS fixes the regression suite's defects (duckeye#28) once reader mode is a sheet.

**Customisable, as in duckent:** a sheet can refine the shape (duckent's `CREATE TREE … LIKE`).
For example, a site recipe binds `:chrome` to that site's own class names. In a `.dcs` file:
`@tree site LIKE duck_blocks { pseudo: chrome := [class*=sidebar], [class*=promo]; }`. This is
Claude's proposed spelling; duckent's own DDL is the reference.

## Selectors over blocks

| Selector | Matches | Note |
|---|---|---|
| `heading`, `paragraph`, `list`, `list_item`, `table`, `code`, `blockquote`, `image`, … | `element_type` | tag = block type |
| `h1` … `h6` | a heading of that rank (`attributes['heading_level']`) | shorthand |
| `#intro` | `attributes['id']` | CSS meaning; in ast_select `#` is the node name, so the projection maps a block's id to `name` |
| `[language=csv]`, `[level=2]`, `[href^=https]` | attributes | CSS attribute operators |
| `:has()`, `:not()`, `:empty`, `:nth-of-type()`, `:first-child` | as CSS | from ast_select |
| `:contains("text")` | the block's text | jQuery's, not standard CSS |
| `A B`, `A > B`, `A + B`, `A ~ B` | the tree from levels | from ast_select |
| `::before`, `::after` | generated content | DCS's own |
| `section(h2:contains("Comments"))` | a heading and every block under it, to the next heading of the same or higher rank | DCS's own scope |

## Properties

The principle (#56): **use CSS's own word where one exists, `-dcs-` only where none does.**

| Phase | Property | Effect |
|---|---|---|
| 1 identity | `-dcs-id: slug(text())` | sets `attributes['id']` |
| | `-dcs-class: …` | adds classes |
| 2 prune | `display: none` | drops the block |
| | `-dcs-prune: section` | drops a heading's whole section |
| | `-dcs-dedupe: text` | drops repeats |
| 3 structure | `display: contents` | unwraps (flattens) a container |
| | `-dcs-wrap: callout(note) \| blockquote \| details` | wraps blocks |
| | `-dcs-as: h3 \| paragraph \| table \| …` | retags. `code[language=csv] { -dcs-as: table }` parses embedded data through duck_block_utils#79 |
| | `-dcs-level: +1` | demotes headings |
| 4 content | `content` with `::before` and `::after` | strings, `attr()`, `text()`, `slug()`, `counter()` |
| | `-dcs-content-as: text \| md \| markdown \| rst \| latex \| html \| org \| …` | parsed through panduck's readers |
| | `-dcs-content-place: inline \| block` | same block, or a new one beside it |
| | `text-transform`, `text-overflow`, `-dcs-replace: "re" "with"` | text |
| | `-dcs-link: text \| absolute` | links |
| | `counter-reset`, `counter-increment` | numbering |
| 5 order | `order: n` | moves blocks (footnotes to the end) |
| 6 metadata | `@frontmatter { title: text(h1:first-of-type); source: url(); tags: "clippings"; }` | the document's properties |
| 7 presentation | `color`, `font-weight`, `font-style`, … any other CSS property | stored in `attributes['style']`; each renderer uses what it can, and warns on the rest |

**Format-specific rules:** `@media (format: html) { … }` scopes rules to one output format.
That's CSS's own at-rule, with a DCS media feature.

## The cascade

- **Matching happens once,** against the document after phase 1, as CSS matches the DOM and never
  its own generated content. Rules don't see what other rules inserted, except ids from phase 1
  (the sketch works this way).
- **Phases run in the order above.** Within a phase, the CSS cascade decides one value per property
  and block: specificity first, then source order.
- **`@import "reader.dcs";`** pulls in another sheet before the importing sheet's own rules.

## The SQL surface

| Function | Returns | For |
|---|---|---|
| `dcs_apply(blocks, sheet)` | the new blocks | everything |
| `dcs_rules(sheet)` | one row per rule and property: selector, property, value, specificity, order, line | the editor, linting |
| `dcs_explain(blocks, sheet)` | one row per change: block, rule, property, before, after | the flip view (#55) |
| `dcs_check(sheet)` | errors and warnings with line and column | the editor |

Errors never abort a document. Unknown properties, unparsable values and formats a renderer can't
express become warnings, and strict mode (`dcs_apply(…, strict := true)`) turns them into errors.

## Clients

- **CLI:** `duckeye --sheet anchors.dcs file.md` and `--reader` become built-in sheets.
- **Android:** the editor in the sketch is a Pro feature (#56), with saved sheets per site or file
  type. Ask an AI's format mode asks for a sheet (#59).
- **The regression suite** (duckeye#28) runs reader mode as a sheet once it is one.

## Tests (written first)

- A golden fixture per property: input blocks, sheet, expected blocks.
- Parity with the sketch's three examples.
- **The cascade:** specificity, source order, phases, the match-once rule.
- **Errors:** a broken sheet gives warnings with line and column, and the document still renders.
- **Reader-mode parity:** once reader mode is a sheet, it scores at least today's medians on the
  regression suite (token F1 0.898, 4-shingle F1 0.851, the 10-page baseline).

## Open questions (Teague's calls)

1. **How DCS matches selectors.** Options:
   - (a) **Its own small C++ matcher for the block subset,** differential-tested against
     ast_select_from on projected blocks. **Claude's recommendation.** It's fast enough for
     sheets, it doesn't wait on M3, and it doesn't touch spec 046's plan.
   - (b) Wait for spec 046 in sitting_duck. That brings C++ selector planning, but the row work
     stays SQL, so per-rule planning cost on blocks is unknown, and 046 isn't started.
   - (c) Propose to sitting_duckling a narrow node interface that both could use. That's a departure
     from 046, which keeps the full schema and moves the engine sitting_duck → sitting_duckling with
     no third repo. It's Teague's call, and best raised after DCS v1 shows the subset works.

   The earlier draft recommended "spec 046 built first in the DCS repo". That misread 046, as the
   sitting_duck session pointed out (2026-11-13).
2. **The dependency.** DCS needs tree-sitter-css to parse sheets. Full sitting_duck is about
   15.8 MB in the Android engine. Options: the CSS grammar alone, vendored like sitting_duck vendors
   its grammars (inferred to be small), or sitting_duckling with only the CSS grammar once it exists.
3. **The extension's name:** duck_block_mods (Teague's "DuckBlockMods"), duck_sheets, or dcs.
4. **Decided:** `.x` is the source's own classes, normalised into `attributes['class']`, and computed
   facts are pseudo-classes. It follows duckent's CLASSES and PSEUDO slots (Teague: "In duckent .x is
   customizable as is [attr]"). Open: which reader asks to file (panduck DOCX and ODT styles,
   callouts, EPUB epub:type, LaTeX environments).
