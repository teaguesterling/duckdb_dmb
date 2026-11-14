# dmb — Duck Mutation Blocks

dmb is a DuckDB extension that runs **DCS** (`.dcs`, Cascading Duck Sheets). A DCS sheet is a set
of CSS-shaped rules. dmb applies a sheet to a duck_blocks document and returns a new document.
Any duck_block_utils renderer can then write the result.

```css
heading  { -dcs-id: auto; }                       /* give every heading an anchor id */
.comment { --audience: "kids"; }                  /* set an attribute */
h2:contains("Comments") { -dcs-prune: section; }  /* drop a whole section */
```

**Status: early, local only.** Phases 1 and 2 of the [design](docs/superpowers/specs/2026-11-13-dcs-design.md)
work: ids, classes, attributes, and pruning. The other phases are not built yet. The
[implementation plan](docs/superpowers/plans/2026-11-13-dmb-v1.md) lists what comes next. dmb
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
| `dcs_rules(sheet)` | one row per rule and declaration: `rule, selector, pseudo_element, property, value, important, specificity, order, media, line, column` |
| `dcs_version()` | the dmb version |

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

**Properties.**

| Property | Effect |
|---|---|
| `-dcs-id: <value>` or `auto` | sets `attributes['id']`; `auto` sets it only where no id exists |
| `-dcs-class: +a -b c` | adds `a` and `c`, removes `b` |
| `--name: <value>` or `unset` | sets or removes `attributes['name']`; `--id`, `--class` and `--heading_level` are refused |
| `display: none` | drops the block and everything under it |
| `-dcs-prune: section` | drops a heading and its section: every row up to the next heading of the same or higher rank |
| `-dcs-dedupe: text` | drops a block whose text repeats the text of an earlier block that the same property matched |

Values are strings and the functions `text()`, `attr(name)`, `slug(value)` and `url()`. The
cascade is CSS's: `!important`, then specificity, then source order. Phase 1 (`-dcs-id`,
`-dcs-class`) matches the input. Every other property matches the document after phase 1, so it
sees the new ids and classes. After a prune, `element_order` is numbered again from 0.

## What does not work yet

dmb reports each of these as a `not-yet` diagnostic. It never skips one silently.

- The pseudo-classes `:meta`, `:chrome`, `:main`, `:body`, `:data`, `:callout`, `:lang()` and
  their group. A rule that uses one is skipped.
- `::before` and `::after`, `content`, `display: contents`, `-dcs-wrap`, `-dcs-as`,
  `-dcs-level`, counters, `order`, `@frontmatter`, `@import`, and presentation properties such
  as `color`.
- `@media (format: X)`: dmb parses it, but `dcs_apply` has no output format yet, so these rules
  never apply. This one has no diagnostic yet.

The functions `dcs_explain` and `dcs_check` do not exist yet.

## Errors

A broken sheet never stops a document. dmb parses a sheet one top-level rule at a time, so one bad
rule does not remove the rules after it. A rule dmb can't use is skipped and given a diagnostic.
Pass `strict := true` (the third argument) to raise the first diagnostic as an error:

```sql
SELECT dcs_apply(doc, 'p { color: red; }', true) FROM docs;
-- Invalid Input Error: dcs_apply: not-yet at line 1, column 5:
--   presentation property color is not supported yet
```

Lines are 1-based. Columns are 1-based and count characters, not bytes. `dcs_check` will expose
the diagnostics as rows; until then, they show only in strict mode.

## Ids and slugs

**The slug rule is changing.** Today `slug()` and `-dcs-id: auto` copy duckdb_markdown's
`GenerateSectionId` (fc9bbe0): ASCII lower-case, and any other byte becomes `-`. So `Café` gives
`caf`. On 2026-11-14 Teague chose pandoc's `auto_identifiers` rule for every reader and for dmb.
That rule keeps letters of any script (`café`). This section changes when dmb adopts it.

Two differences from the readers remain:

- `-dcs-id` de-duplicates ids in a document: a second `notes` becomes `notes-1`. The readers do
  not de-duplicate.
- read_html_blocks gives `.x` selectors classes only when you read with
  `capture_attributes := 'classes'` (or `'*'`). Its default captures `id`, `name`, `href` and
  `src` only.

## How it works

- **Parsing.** dmb vendors tree-sitter (the commit sitting_duck pins) and the tree-sitter-css
  grammar. See `third_party/tree-sitter-css/PROVENANCE.md`. dmb needs no other extension at run
  time.
- **Matching.** A C++ matcher runs right to left over an in-memory block tree, in one pass. It
  plans no SQL for each rule.

The design is in [`docs/superpowers/specs/`](docs/superpowers/specs/2026-11-13-dcs-design.md).
Spelling changes from it are recorded as decisions D1–D6 in the plan: `:section()` and
`:matches("…")`, for example.
