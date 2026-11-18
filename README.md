# dmb — Duck Mutation Blocks

dmb is a DuckDB extension that runs **DCS** (`.dcs`, Cascading Duck Sheets). A DCS sheet is a set
of CSS-shaped rules. dmb applies a sheet to a duck_blocks document and returns a new document.
Any duck_block_utils renderer can then write the result.

Documentation: https://duckdb-dmb.readthedocs.io/en/latest/

```css
heading  { -dcs-id: auto; }                       /* give every heading an anchor id */
.comment { --audience: "kids"; }                  /* set an attribute */
h2:contains("Comments") { -dcs-prune: section; }  /* drop a whole section */
```

**Status: early, local only, not released.** All seven phases of the
[design](docs/superpowers/specs/2026-11-13-dcs-design.md) have a first implementation: ids and
classes, attributes and pruning, structure, content, order, `@frontmatter`, and presentation. The
gaps are in [What does not work yet](#what-does-not-work-yet). The
[implementation plan](docs/superpowers/plans/2026-11-13-dmb-v1.md) records every decision. dmb
targets DuckDB v2.0.0.

## Terms

| Term | Meaning |
|---|---|
| document | One `duck_block[]` list: the rows of one file, in `element_order` |
| block | One row of a document. Inline rows (`kind = 'inline'`) are never matched; their text belongs to their block |
| sheet | The text of a `.dcs` file |
| rule | One selector and its declarations. A selector list (`h1, h2`) becomes one rule per selector |
| diagnostic | A problem dmb found in a sheet. It never stops a document |

## Build and load

```sh
git submodule update --init --recursive
make release GEN=ninja            # on longbottom: srun -n 1 -c 8 --mem=16G -p batch -t 120 make release GEN=ninja
build/release/test/unittest "test/sql/*"
```

```sql
LOAD 'build/release/extension/dmb/dmb.duckdb_extension';   -- start the CLI with -unsigned
```

A local build reports the DuckDB commit (`d54cbbe3df`) as its version, not `v2.0.0`. So it loads
only in `build/release/duckdb`, and that CLI can't load extensions installed for `v2.0.0`, such as
the markdown and webbed readers (measured 2026-11-14). The examples below build documents as
literal lists for that reason.

## Functions

| Function | Returns |
|---|---|
| `dcs_apply(blocks, sheet [, strict])` | the document with the sheet applied, in the input's own type |
| `dcs_select(blocks, selector)` | `INTEGER[]`: the `element_order` of each block the selector matches |
| `dcs_explain(blocks, sheet)` | `STRUCT(element_order, rule, selector, property, before, after, line, "column")[]`: one entry per change, naming the winning rule and its declaration's position |
| `dcs_check(sheet)` | one row per diagnostic: `severity, code, message, line, column`. It needs no document |
| `dcs_rules(sheet)` | one row per rule and declaration: `rule, selector, pseudo_element, property, value, important, specificity, order, media, line, column`. `@frontmatter` declarations have `rule` NULL |
| `dcs_parse_markdown(blocks)`, `dcs_parse_html(blocks)` | macros: replace the generated content a sheet marked with `-dcs-content-as: markdown` or `html` with that reader's rows. They need the markdown or webbed extension loaded |
| `dcs_version()` | the dmb version |
| `dcs_parser_abi()` | the tree-sitter-css ABI dmb was built with |

`blocks` is any list of structs that has the seven duck_block fields: `kind`, `element_type`,
`content`, `level`, `encoding`, `attributes MAP(VARCHAR, VARCHAR)` and `element_order`. Other
fields, such as `filename`, pass through. A missing field or a wrong type is a Binder Error that
names the field. `dcs_rules` needs a constant sheet. `dcs_apply` and `dcs_select` take a
different document or sheet on every row.

## Example

```sql
CREATE MACRO blk(t, c, lvl, attrs, o) AS
  {'kind': CASE WHEN t = 'text' THEN 'inline' ELSE 'block' END, 'element_type': t, 'content': c,
   'level': lvl, 'encoding': 'text', 'attributes': attrs::MAP(VARCHAR, VARCHAR), 'element_order': o};
CREATE TABLE docs AS SELECT [
  blk('heading',   'Field guide',      1, MAP {'heading_level': '1'}, 0),
  blk('paragraph', '',                 1, MAP {},                     1),
  blk('text',      'Ducks are birds.', 2, MAP {},                     2),
  blk('heading',   'Comments',         1, MAP {'heading_level': '2'}, 3),
  blk('paragraph', '',                 1, MAP {'class': 'comment'},   4),
  blk('text',      'Nice post!',       2, MAP {},                     5)
] AS doc;

SELECT u.element_order, u.element_type, u.attributes
FROM (SELECT unnest(dcs_apply(doc, 'heading { -dcs-id: auto; } .comment { --audience: "kids"; }')) AS u FROM docs);
```

| element_order | element_type | attributes |
|--:|---|---|
| 0 | heading | `{heading_level=1, id=field-guide}` |
| 1 | paragraph | `{}` |
| 2 | text | `{}` |
| 3 | heading | `{heading_level=2, id=comments}` |
| 4 | paragraph | `{audience=kids, class=comment}` |
| 5 | text | `{}` |

`dcs_apply(doc, 'h2:contains("Comments") { -dcs-prune: section; }')` keeps rows 0–2 only.
`dcs_select(doc, ':section(h2:contains("Comments"))')` gives `[3, 4]`.

`dcs_explain` shows which rule made each change, on the same document (measured 2026-11-15):

```sql
SELECT e.* FROM (SELECT unnest(dcs_explain(doc, 'heading { -dcs-id: auto; }
.comment { --audience: "kids"; }')) AS e FROM docs);
```

| element_order | rule | selector | property | before | after | line | column |
|--:|--:|---|---|---|---|--:|--:|
| 0 | 0 | heading | -dcs-id | NULL | field-guide | 1 | 11 |
| 3 | 0 | heading | -dcs-id | NULL | comments | 1 | 11 |
| 4 | 1 | .comment | --audience | NULL | kids | 2 | 12 |

## What works

**Selectors.** A type selector matches `element_type`. These also work:

- `h1` … `h6` (by `attributes['heading_level']`), and `p`, `img`, `li`, `pre` as names for
  `paragraph`, `image`, `list_item` and `code`.
- `#id`, `.class` (a word of `attributes['class']`), and `[attr]` with `=`, `~=`, `^=`, `$=`,
  `*=` and `|=`. `[kind]`, `[element_type]`, `[encoding]` and `[level]` read the fields.
- The combinators: descendant, `>`, `+` and `~`. A block's parent is the nearest earlier row with
  a smaller `level`.
- `:not()`, `:has()` (also `:has(> x)`), `:contains("…")`, `:matches("regex")` (RE2),
  `:heading`, `:h(n)`, `:section(sel)`, and `:first-child`, `:last-child`, `:only-child`,
  `:nth-child()`, `:nth-of-type()` and their relatives, `:empty` and `:root`.
- The role pseudo-classes: `:meta`, `:chrome`, `:main`, `:body`, `:data` (a table, or a code
  block in csv, tsv, json, jsonl or yaml), `:callout`, `:footnote`, `:caption`, `:link-only`,
  `:image-only` and `:lang()`.
- `::before` and `::after`, with `content`.

**Properties.**

| Property | Effect |
|---|---|
| `-dcs-id: <value>` or `auto` | sets `attributes['id']`; `auto` sets it only where no id exists |
| `-dcs-class: +a -b c` | adds `a` and `c`, removes `b` |
| `--name: <value>` or `unset` | sets or removes `attributes['name']`; `--id`, `--class` and `--heading_level` are refused |
| `display: none` | drops the block and everything under it |
| `-dcs-prune: section` | drops a heading and its section: every row up to the next heading of the same or higher rank |
| `-dcs-dedupe: text` | drops a block whose text repeats the text of an earlier block that the same property matched |
| `display: contents` | removes the block, keeps its children, and moves them up one level |
| `-dcs-wrap: blockquote \| details \| callout(x)` | puts the block in a new container |
| `-dcs-as: <element_type> \| h1`…`h6` | changes the block's type; `hN` sets `heading_level` |
| `-dcs-level: +n \| -n \| n` | changes a heading's rank, kept in 1–6 |
| `content` | on a block, replaces its text; on `::before` or `::after`, adds generated content (inline by default; `-dcs-content-place: block` makes a sibling block) |
| `-dcs-content-as: <format>` | marks generated content as markup in that format (see the `dcs_parse_*` macros) |
| `counter-reset`, `counter-increment` | counters, read with `counter(name)` in `content` |
| `text-transform`, `-dcs-replace: "re" "with"` | change a block's text (RE2, global replace) |
| `order: <integer>` | moves a block among its siblings, with its subtree, as a stable sort |
| `@frontmatter { key: value; }` | merges keys into the document's front-matter metadata block, or adds one |
| any other property, such as `color` | presentation: written to `attributes['style']` for a renderer |

Values are strings and the functions `text()`, `attr(name)`, `slug(value)`, `counter(name)` and
`url()`. The cascade is CSS's: `!important`, then specificity, then source order. Phase 1 (`-dcs-id`,
`-dcs-class`) matches the input. Every other property matches the document after phase 1, so it
sees the new ids and classes. After a prune, `element_order` is numbered again from 0.

## What does not work yet

dmb reports each of these as a `not-yet` diagnostic. It never skips one silently.

- `:external` and `:internal`. A rule that uses one is skipped.
- `text-overflow` and `-dcs-link`.
- `@import`, and at-rules that tree-sitter-css parses as a general at-rule, such as
  `@font-face`. `@keyframes`, `@supports`, `@charset` and `@namespace` are not part of DCS: they
  give `unsupported-at-rule` (measured 2026-11-16).
- `@media (format: X)`: dmb parses it, but `dcs_apply` has no output format, so these rules never
  apply.
- `-dcs-as: table` marks a data code block with `attributes['dcs-as'] = 'table'`, but nothing
  converts it to a table yet. The conversion waits for duckdb_panduck#188.
- `-dcs-content-as` formats other than markdown and html: no per-row reader exists, so the
  content stays a `raw` block with `attributes['format']` (diagnostic `no-reader`).

Parentheses and brackets may nest 32 levels deep in a sheet or a selector, outside strings and
comments. The selector compiler may nest 256 levels: compounds, suffixes such as `.a.b`, and
pseudo-class arguments. Deeper is a `syntax` error. The tests send 100,000-deep functions,
`:not()`, braces, `@media` and suffix runs: each gets a diagnostic, not a crash.

## Errors

A broken sheet never stops a document. dmb parses a sheet one top-level rule at a time, so one bad
rule does not remove the rules after it. A rule dmb can't use is skipped and given a diagnostic.
Pass `true` as the third argument to raise the first diagnostic as an error. It is positional:
`strict := true` does not bind (measured 2026-11-16).

```sql
SELECT dcs_apply(doc, 'p { text-overflow: ellipsis; }', true) FROM docs;
-- Invalid Input Error: dcs_apply: not-yet at line 1, column 5: text-overflow is not supported yet
```

`dcs_check` lists a sheet's diagnostics as rows, with no document (measured 2026-11-15):

```sql
SELECT * FROM dcs_check('h2 { -dcs-level: up; } p { --x: frob(); }');
```

| severity | code | message | line | column |
|---|---|---|--:|--:|
| error | bad-value | -dcs-level takes +n, -n or n, not up | 1 | 6 |
| error | unknown-function | --x: unknown-function: frob() | 1 | 28 |

Some diagnostics need a document, such as `-dcs-level` on a block that is not a heading. These
come only from `dcs_apply`, and show only in strict mode. Lines are 1-based. Columns are 1-based
and count characters, not bytes.

## Ids and slugs

`slug()` and `-dcs-id: auto` use pandoc's `auto_identifiers` rule, the rule every duck_blocks
reader is moving to (duckdb_markdown#111):

- Lower-case across all of Unicode, and keep letters of any script: `ÉCOLE Ünd` → `école-ünd`.
- Keep letters, numbers, `_`, `-` and `.`. Drop other characters: `Zoë's notes` → `zoës-notes`.
- Join whitespace-separated words with `-`: `a -- b` → `a----b`.
- Drop everything before the first letter: `1. Introduction` → `introduction`.
- If nothing is left, the id is `section`.

`-dcs-id: auto` also de-duplicates in a document: a second `notes` becomes `notes-1`, as in
pandoc. A reader that does not de-duplicate gives repeats the same id. An ASCII form,
`slug(v, ascii)` giving Punycode, is planned but not built.

## Checks outside the suite

| Script | Compares | Needs | Last result |
|---|---|---|---|
| `test/differential/slug_pandoc.sh` | `slug()` with pandoc, on `slug_cases.txt` | `pandoc` on the PATH | 33/33 match pandoc 3.1.3 |
| `test/differential/ast_select.sh` | `dcs_select` with sitting_duck's `ast_select_from`, on `ast_select_cases.txt` | sitting_duck, duckeye | 21/21 agree (exclusions listed in the cases file) |
| `test/integration/readers.sh` | the `dcs_parse_*` macros with the real readers | local markdown and webbed builds | 6/6 pass |

These results were measured on this branch on 2026-11-14 and 2026-11-15.

read_html_blocks gives `.x` selectors classes only when you read with
`capture_attributes := 'classes'` (or `'*'`). Its default captures `id`, `name`, `href` and `src`
only.

## How it works

- **Parsing.** dmb vendors tree-sitter (the commit sitting_duck pins) and the tree-sitter-css
  grammar. See `third_party/tree-sitter-css/PROVENANCE.md`. dmb needs no other extension at run
  time.
- **Matching.** A C++ matcher runs right to left over an in-memory block tree, in one pass. It
  plans no SQL for each rule.

The design is in [`docs/superpowers/specs/`](docs/superpowers/specs/2026-11-13-dcs-design.md).
Spelling changes from it are recorded as decisions D1–D11 in the plan: `:section()` and
`:matches("…")`, for example.

## License

Apache-2.0: see `LICENSE`. The bundled tree-sitter and tree-sitter-css sources keep their own MIT
licences, in `third_party/tree-sitter/LICENSE` and `third_party/tree-sitter-css/LICENSE`.
